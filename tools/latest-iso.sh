#!/bin/sh
#
# cix-cache -- render a download block for the newest installer ISO.
#
# Runs on the WEBSITE's machine, on a timer, and writes a fragment the
# site includes. Nothing runs in the visitor's browser, so there is no
# CORS to arrange and no second copy of anything on the page.
#
# Why this is allowed to read /api/v1 at all
# ------------------------------------------
# ADR-0002 is blunt: /api/v1/* is operator-facing observability, and no
# Cix host code path may ever consult it. That invariant is about the
# recipe -> artifact trust chain -- a daemon resolving name@version for
# install must know the exact name from a recipe in git, never from
# asking this server what exists.
#
# An installer ISO is not on that path. ADR-0010 section 7 says so
# directly: `packages` is consumed by a daemon resolving name@version
# for install; an ISO is never installed that way. It is chosen by a
# PERSON, downloaded, and written to a USB stick. This script reads the
# listing the way the dashboard reads it -- for a human -- and a
# download page is exactly the audience the listing is for.
#
# It does not parse names
# -----------------------
# The listing already carries `artifact`, `version`, `release` and
# `arch` as separate fields, split server-side by store_split_display().
# ADR-0013 made that split server-side for this exact reason: a client
# splitting the string itself would be a second implementation of the
# grammar, and the hyphen inside `cix-installer` is what a naive split
# gets wrong. So this script reads fields; it never cuts a string.
#
# Choosing the newest is the same story. `version_rank` is the ordering
# the daemon computed with store_rank_versions() -- store_version_cmp()
# on the version, then the release as a number. Sorting here would
# disagree with the store; `sort -V` puts release 10 below release 2.
#
# Failure keeps yesterday's page
# ------------------------------
# Every output is written to a temporary file beside the target and
# moved over it only once everything has succeeded. If the cache is
# unreachable, serving no ISO, or serving an unsigned one, this exits
# non-zero having touched nothing, and the site keeps showing the last
# good block. A visitor never sees an empty download page or half a
# file, and a failed cron tick is silent to them on purpose.
#
set -eu

BASE="${CIX_BASE:-https://cache.cix.world}"
ARTIFACT="${CIX_ARTIFACT:-cix-installer}"
ARCH=""
FORMAT="html"
OUTPUT=""
PUBKEY="${CIX_PUBKEY:-}"

usage() {
	cat <<'USAGE'
usage: latest-iso.sh [options]

  --base=URL        cache to ask       (default https://cache.cix.world)
  --artifact=NAME   installer to find  (default cix-installer)
  --arch=ARCH       only this architecture; default is every one present
  --format=html|json                   (default html)
  --output=FILE     write there atomically; default is stdout
  --pubkey=KEY      minisign public key, to render the verify command

Reads the newest signed ISO per architecture and renders a block for a
website to include. An unsigned ISO is never advertised; if anything
fails, the output file is left exactly as it was.
USAGE
}

for arg in "$@"; do
	case "$arg" in
	--base=*)     BASE="${arg#*=}" ;;
	--artifact=*) ARTIFACT="${arg#*=}" ;;
	--arch=*)     ARCH="${arg#*=}" ;;
	--format=*)   FORMAT="${arg#*=}" ;;
	--output=*)   OUTPUT="${arg#*=}" ;;
	--pubkey=*)   PUBKEY="${arg#*=}" ;;
	-h|--help)    usage; exit 0 ;;
	*) echo "latest-iso: unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

die() { echo "latest-iso: $*" >&2; exit 1; }

case "$FORMAT" in html|json) ;; *) die "--format must be html or json" ;; esac
command -v curl >/dev/null 2>&1    || die "curl is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"

BASE="${BASE%/}"

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT INT TERM

curl -fsS --max-time 30 "$BASE/api/v1/artifacts" -o "$TMP" \
	|| die "cannot read the listing at $BASE -- leaving any existing output alone"

render() {
	python3 - "$TMP" "$BASE" "$ARTIFACT" "$ARCH" "$FORMAT" "$PUBKEY" <<'PY'
import html
import json
import sys

path, base, artifact, arch, fmt, pubkey = sys.argv[1:7]

with open(path) as f:
    records = json.load(f).get("artifacts", [])

#
# Filter to the installer FIRST, then rank within it. version_rank is a
# global ordinal and the comparator behind it ignores the artifact name
# until its final tiebreak, so it orders correctly only among records
# that share a name -- ranking across artifacts would be comparing
# cix-installer against curl.
#
best = {}
for rec in records:
    if rec.get("artifact") != artifact:
        continue
    iso = None
    for f in rec.get("formats", []):
        if f.get("format") == ".iso":
            iso = f
            break
    if iso is None:
        continue
    a = rec.get("arch") or ""
    if arch and a != arch:
        continue
    if a not in best or rec.get("version_rank", 0) > best[a][0].get("version_rank", 0):
        best[a] = (rec, iso)

if not best:
    what = "%s .iso" % artifact
    if arch:
        what += " for %s" % arch
    sys.exit("latest-iso: no %s found at %s" % (what, base))

out = []
for a in sorted(best):
    rec, iso = best[a]
    #
    # ADR-0010: an ISO may not be published unsigned, so one that is
    # unsigned means something is wrong at the source. Refusing is the
    # only safe answer -- advertising an unverifiable install image is
    # the failure this whole tier exists to prevent.
    #
    if not iso.get("signed"):
        sys.exit("latest-iso: %s is not signed -- refusing to advertise it" % iso["name"])
    name = iso["name"]
    out.append({
        "artifact": rec.get("artifact"),
        "version": rec.get("version"),
        "release": rec.get("release"),
        "arch": a,
        "name": name,
        "url": "%s/%s" % (base, name),
        "sha256": iso.get("sha256"),
        "bytes": iso.get("bytes", 0),
        "signature_name": name + ".minisig",
        "signature_url": "%s/%s.minisig" % (base, name),
    })

if fmt == "json":
    print(json.dumps({"installers": out}, indent=2))
    sys.exit(0)

def mb(n):
    return "%.0f MB" % (n / 1048576.0)

e = html.escape
print('<div class="cix-downloads">')
for d in out:
    print('  <div class="cix-download" data-arch="%s">' % e(d["arch"]))
    print('    <h3>Cix %s <span class="cix-arch">%s</span></h3>'
          % (e(d["version"]), e(d["arch"])))
    print('    <p class="cix-release">release %s &middot; %s</p>'
          % (e(str(d["release"])), e(mb(d["bytes"]))))
    print('    <p><a class="cix-iso" href="%s">Download %s</a></p>'
          % (e(d["url"]), e(d["name"])))
    print('    <p class="cix-verify">')
    print('      <a href="%s">signature</a>' % e(d["signature_url"]))
    print('      &middot; sha256 <code>%s</code>' % e(d["sha256"]))
    print('    </p>')
    if pubkey:
        # The public key is a fact about the project, not about the
        # cache, so it is an argument here rather than something fetched.
        print('    <pre class="cix-verify-cmd"><code>minisign -Vm %s -P %s</code></pre>'
              % (e(d["name"]), e(pubkey)))
    print('  </div>')
print('</div>')
PY
}

if [ -z "$OUTPUT" ]; then
	render
	exit 0
fi

#
# Same directory, so the mv is a rename within one filesystem and is
# therefore atomic: a reader sees the old file or the new one, never a
# partial write. /tmp could be a different filesystem, which would turn
# this into a copy and reintroduce the very window it is here to close.
#
dir=$(dirname "$OUTPUT")
[ -d "$dir" ] || die "no such directory: $dir"
staged="$OUTPUT.tmp.$$"
trap 'rm -f "$TMP" "$staged"' EXIT INT TERM

render > "$staged" || die "nothing rendered -- $OUTPUT is unchanged"
chmod 644 "$staged"
mv -f "$staged" "$OUTPUT"
echo "latest-iso: wrote $OUTPUT" >&2
