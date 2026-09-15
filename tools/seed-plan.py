#!/usr/bin/env python3
"""
Turns two listings into a plan. Reads nothing else and writes no bytes:
seed.sh does every request, this decides what the requests should be.

Split out rather than embedded in the shell because the interesting
part of seeding is the SELECTION, and a selection you cannot run on a
saved pair of listings is one you cannot check without touching a
public server.

Selection never re-implements the store's ordering. The listing
already carries version_rank, which the daemon computes with
store_rank_versions() -- store_version_cmp() on the version, then the
release as a number, then the name. It is a global ordinal over every
stem, but the comparator ignores the artifact name until the final
tiebreak, so within one (artifact, arch) group a higher rank is a
newer artifact, which is exactly the question asked here. Sorting the
names in this file instead would be a second grammar, and `sort -V`
disagrees with the daemon on the case the daemon has a comment about:
zlib release 10 against release 2.
"""

import json
import re
import sys


def load(path):
    with open(path) as f:
        return json.load(f).get("artifacts", [])


def names_of(ident):
    """Every file an identity is made of: each format, plus its detached
    signature where the listing says one exists.

    The listing reports `signed` per format and does not list the
    signature as a format of its own -- it is a sibling object sharing
    the artifact's stem (STORE_SIG_EXT), which is the store's
    documented contract, so the name is constructed rather than
    discovered."""
    out = []
    for fmt in ident.get("formats", []):
        name = fmt["name"]
        if fmt.get("signed"):
            out.append((name + ".minisig", None, 0, True))
        out.append((name, fmt.get("sha256"), fmt.get("bytes", 0), False))
    return out


def main():
    local_path, remote_path, keep, since, exclude, prune = sys.argv[1:7]
    keep = int(keep)
    since = int(since) if since else 0
    exclude_re = re.compile(exclude) if exclude else None
    prune = prune == "1"

    local = load(local_path)
    remote = load(remote_path)

    # name -> sha256, for the conflict check and the skip.
    remote_have = {}
    for ident in remote:
        for fmt in ident.get("formats", []):
            remote_have[fmt["name"]] = fmt.get("sha256")
            if fmt.get("signed"):
                remote_have.setdefault(fmt["name"] + ".minisig", None)

    groups = {}
    excluded = set()
    for ident in local:
        artifact = ident.get("artifact", "")
        key = (artifact, ident.get("arch") or "-")
        if exclude_re is not None and exclude_re.search(artifact):
            excluded.add(artifact)
            continue
        groups.setdefault(key, []).append(ident)

    plan = []
    selected_stems = {}
    for key in sorted(groups):
        # Newest first by the daemon's own ordering, then keep N.
        ranked = sorted(groups[key], key=lambda i: i.get("version_rank", 0), reverse=True)
        chosen = ranked[:keep]
        #
        # --since narrows AFTER the newest-N choice, never before.
        # Filtering first would let an old-but-current release be
        # dropped and a superseded one promoted in its place, which is
        # the opposite of what a retention rule should ever do.
        #
        if since:
            chosen = [i for i in chosen if i.get("modified", 0) >= since]
        if not chosen:
            continue
        selected_stems[key] = {i["stem"] for i in chosen}
        for ident in chosen:
            for name, sha, size, is_sig in names_of(ident):
                if name in remote_have:
                    have = remote_have[name]
                    if sha and have and have != sha:
                        plan.append(("conflict", name, sha, size, key[0], ident["stem"]))
                    else:
                        plan.append(("skip", name, sha or "", size, key[0], ident["stem"]))
                else:
                    plan.append(("push", name, sha or "", size, key[0], ident["stem"]))

    #
    # Pruning is scoped to identities this run actually manages. An
    # identity the local store has never heard of is left alone: the
    # remote may be seeded from more than one place, and "delete what I
    # cannot account for" is how a second seeder's work disappears.
    #
    if prune:
        for ident in remote:
            key = (ident.get("artifact", ""), ident.get("arch") or "-")
            if key not in selected_stems:
                continue
            if ident["stem"] in selected_stems[key]:
                continue
            for fmt in ident.get("formats", []):
                if fmt.get("signed"):
                    plan.append(("prune", fmt["name"] + ".minisig", "", 0, key[0], ident["stem"]))
                plan.append(("prune", fmt["name"], "", fmt.get("bytes", 0), key[0], ident["stem"]))

    #
    # "-" and never an empty field. A tab is IFS whitespace, so a shell
    # `read` with IFS=tab COLLAPSES consecutive tabs and drops empty
    # fields -- a signature row, whose sha256 the listing does not
    # carry, then shifts every field after it and the reader assigns a
    # package name to a byte count. Emitting a placeholder keeps the
    # row six fields wide whatever is missing from it.
    #
    def row(cells):
        return "\t".join(str(c) if str(c) != "" else "-" for c in cells)

    for r in plan:
        print(row(r))
    for name in sorted(excluded):
        print(row(("excluded", name, "-", 0, name, "-")))


if __name__ == "__main__":
    main()
