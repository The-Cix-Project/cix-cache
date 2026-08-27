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
```

```
build/cixcached --config=/etc/cixcache.conf
```

`pull_token` empty means pull is open. **Set `push_token`.** Pull may be
open because consumers verify every byte against a checksum from git; an
open push lets anyone fill the disk. The dashboard shows `push: OPEN` in a
warning colour when it is unset.

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

A hand-built export (plain tarballs under `packages/` and `images/`) is
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

It can also be triggered over REST, which is what `cixcachectl import` does:

```
cixcachectl import --token=<push_token>
cixcachectl import-status
```

## Operating

```
cixcachectl status                        # store and server summary
cixcachectl ls                            # every published artifact, paged
cixcachectl log [-f]                      # what the server has been doing
cixcachectl manifest                      # MANIFEST.json, generated live
cixcachectl gc --dry-run                  # what collection would remove
cixcachectl gc --token=<t>                # remove unreferenced blobs
cixcachectl rm NAME --token=<t>           # unpublish a name
cixcachectl put FILE --name=N --sha256=H --token=<t> [--images]
```

`rm` unpublishes a name; the blob survives until collected. A push whose
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

For an image, the filename's hash is the sha256 of the sorted
`name@version,…` manifest string from the recipe, with no trailing newline:

```
printf '%s' 'bash@5.2.37,bc@1.08.1,...,zlib@1.3.2-3' | sha256sum
```
