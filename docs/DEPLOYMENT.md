# Deploying cix-cache

## Installing

```
make
sudo make install                      # binaries + web assets to /opt/cixcache
sudo install -m 0640 deploy/cixcache.conf.example /opt/cixcache/etc/cixcache.conf
sudo $EDITOR /opt/cixcache/etc/cixcache.conf      # set root= and push_token=
sudo install -m 0644 deploy/cixcache.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now cixcache
```

`PREFIX` and `DESTDIR` are honoured, so `make install PREFIX=/usr/local`
works. Installing never touches the store or an existing config -- a
store is operator data and a config holds a credential, so reinstalling
a binary must not overwrite either.

The unit runs unprivileged under `ProtectSystem=strict` with the store as
its only writable path. The registry serves opaque bytes and is never a
trust boundary, so it needs no privilege beyond reading its store and
binding one port. Point `ReadWritePaths=` at whatever `root=` is.

```
systemctl status cixcache
journalctl -u cixcache -f          # MISS lines land here
```

## Running the server

```
build/cixcached --root=cache --bind=0.0.0.0 --port=8080 --web-root=web
```

Options may also come from a key=value config file, which command-line
flags then override:

```
# /etc/cixcache.conf
root=/var/lib/cixcache
bind=0.0.0.0
port=8080
web_root=/usr/share/cixcache/web
push_token=<a long random string>
pull_token=
require_signature=.iso
```

```
build/cixcached --config=/etc/cixcache.conf
```

A key the file does not mention keeps its built-in default; only the keys
it names are overlaid. An empty value is itself a value — `pull_token=`
means pull is open, and `require_signature=` means nothing is refused for
being unsigned.

`pull_token` empty means pull is open. **Set `push_token`.** Pull may be
open because consumers verify every byte against a checksum from git; an
open push lets anyone fill the disk. The dashboard shows `push: OPEN` in a
warning colour when it is unset.

`require_signature` lists the artifact suffixes that may not be published
without a detached `.minisig` beside them, comma separated, matched with
or without the leading dot. The default `.iso` is the historical
behaviour and the one case that stands on its own: an ISO is booted, so
nothing downstream gets a chance to check it. Adding `.cixpkg` or
`.tar.gz` makes the store refuse unsigned packages too — worth knowing
that this will 409 a push from any Cix host whose signing key is not
configured, which is why it is not the default. A suffix the store does
not recognise is reported on stderr at startup and **not** enforced.

This is hygiene, not security. The store is not a trust boundary
(cix-build-system ADR-0001): what protects an installed package is the
`pkg_artifact_sha256` in its git recipe and the install-side signature
check, neither of which this setting replaces. See
[ADR-0012](adr/0012-cixpkg-and-signature-policy.md).

There is no TLS here, by design — the registry is not a trust boundary
(`docs/adr/0002-...`). Put it behind a reverse proxy if you want transport
encryption, but note the daemon will not carry a bearer token across a
cross-host redirect.

## Pointing a Cix host at it

The recipes and the binaries are two independent URLs. That separation is
the security model, not a filing convention.

```
# the binaries -- this server
cixctl --host=<newbox> pkg artifact-config set --url=http://<lan-ip>:8080

# the recipes -- git, separately
cixctl --host=<newbox> pkg repo-config set --repo-url=https://git.home.arpa/itdlabs/cix.git
cixctl --host=<newbox> pkg sync

# apply an image recipe: pulls the whole rootfs, compiles nothing
cixctl --host=<newbox> image apply-recipe gcc-tcc-bootstrap
```

Add `--token=<push_token>` to `artifact-config set` if `pull_token` is set.

## Migrating a static export

A hand-built export (plain tarballs under `packages/`) is
converted in place into the content-addressed store. Everything is on one
filesystem, so this is `rename()` only and the bytes are never copied.

```
build/cixcached --root=cache --import --dry-run     # report, change nothing
build/cixcached --root=cache --import               # do it
```

The import is idempotent — an entry that is already a symlink is skipped,
so an interrupted run resumes. Where the shipped `MANIFEST.json` records a
digest, the computed digest is compared against it and any disagreement is
reported and the file left alone. That is the last moment the export's own
record can be checked against its bytes.

It can also be triggered over REST, which is what `cix cache import` does:

```
cix cache import --token=<push_token>
cix cache import-status
```

## Canonicalizing names (required once, upgrading past v2.3.0)

Artifact names are canonically `<name>-<version>-<release>`, with an
omitted release meaning `1`. From v2.3.0 the server resolves canonical
names, so a store written before that must be migrated once:

```
build/cixcached --root=cache --canonicalize --dry-run   # report, change nothing
build/cixcached --root=cache --canonicalize             # do it
```

This renames symlinks only. No blob is touched and no bytes move, so it
is instant regardless of store size and cannot lose data. A rename whose
target already exists is refused and counted rather than overwriting
anything.

**Existing hosts need no change.** A fetch under the old bare name is
served as an alias of the canonical entry, so recipes that have not been
updated keep hitting the cache instead of quietly rebuilding from
source. Those hits are logged and counted:

```
cix cache status | grep aliases       # should fall to zero over time
```

Skipping the migration is the one failure worth knowing about: an
unmigrated entry stays listed but stops resolving, which looks exactly
like a cache that is simply never used.

## Stamping the architecture

Artifact names carry the machine they were built for. Nothing infers
one — storing a bare push as `x86_64` would attach a claim the pusher
never made — so an existing store is stamped once, explicitly:

```
build/cixcached --root=cache --set-arch=x86_64 --dry-run   # report
build/cixcached --root=cache --set-arch=x86_64             # do it
```

Symlinks only; no blob moves. It is an assertion that everything
already in the store was built for that machine, which is why it takes
the architecture as an argument rather than guessing it from `uname`.

**Existing hosts need no change.** A request without an architecture
resolves while exactly one is published. When a second appears, that
same request returns `409` instead of guessing:

```
{"error":"that name exists for more than one architecture -- ask for one"}
```

That is the intended behaviour, not a regression: a content checksum
cannot tell an aarch64 binary from an x86_64 one, so a name that could
mean either must mean neither.

## Publishing an installer ISO

Signature first, then the ISO — an unsigned bootable is refused, and
the refusal lands before the body is uploaded:

```
minisign -Sm cix-installer-2.2.0-1-x86_64.iso        # produces .iso.minisig
cix cache publish cix-installer-2.2.0-1-x86_64.iso.minisig --token=<t> ...
cix cache publish cix-installer-2.2.0-1-x86_64.iso     --token=<t> ...
```

Consumers verify with the pinned public key, on a trusted machine,
**before** writing the stick. The key does not come from this server.

`cix cache status` grows an `installers` line, and `list` a
`SIGNED` column, once there is an installer to report. Installers are counted
apart from packages, matching `MANIFEST.json`'s two sections.

## Operating

```
cix cache status                        # store and server summary
cix cache list                          # every published artifact, paged
cix cache log [-f]                      # what the server has been doing
cix cache manifest                      # MANIFEST.json, generated live
cix cache gc --dry-run                  # what collection would remove
cix cache gc --token=<t>                # remove unreferenced blobs
cix cache delete NAME --token=<t>       # unpublish a name
cix cache publish FILE --name=N --sha256=H --token=<t>
```

Verbs follow the Cix CLI grammar: `list`, `publish`, `delete`. The
older `ls`, `put` and `rm` keep working as aliases and are not going
away on a schedule — muscle memory and existing scripts are the real
cost of a rename, and breaking them buys nothing.

`list` shows artifact, version and release in separate columns; the
release is Cix's number, not upstream's. It lists newest-published
first, and grows an architecture column only once two artifacts differ
by one. `status` grows an `unstamped` line only when there is something
to stamp.

A line is one artifact rather than one file. An artifact published in
more than one encoding — a `.tar.gz` and a `.cixpkg` of the same
identity — gets a Format column and one line per encoding, with the
artifact, version and release columns filled in on the first only. The
footer then reads "N artifacts in M files"; while every artifact has a
single encoding the two numbers are equal and it says neither twice.

`HEAD` works on artifacts — it returns the size and `X-Cix-Sha256`
without re-reading the file, which is how to check what is already
published before pushing over it — and on dashboard assets.

`delete` unpublishes a name; the blob survives until collected. A push whose
body does not match its declared `X-Cix-Sha256` is refused with 400, and
republishing a name with *different* bytes is refused with 409 — a recipe
version is immutable, so a published name may only ever mean one byte
sequence.

## Verifying a deployment

```
# the URL shape a host actually uses
curl -fsSL http://<host>:8080/bash-5.2.37-2.tar.gz | sha256sum

# should equal the recipe's pkg_artifact_sha256
```

Packages are the only tier; `/images/...` is not served and never was
after v2.0.0 (ADR-0006). A host applying an image recipe composes it from
these packages.
