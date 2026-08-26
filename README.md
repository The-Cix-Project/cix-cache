# export-artifacts — images pulled off a running Cix host

Whole-image rootfs artifacts extracted from 192.168.15.95, so a new host
can install them instead of rebuilding from source. Gitignored: this is
compiled output, hundreds of MB to GB.

## Layout

`packages/<name>-<version>.tar.gz` — individual package artifacts, in
ADR-0122's package-tier shape. A consuming host fetches
`<base_url>/<name>-<version>.tar.gz`, so any package can be installed
into any image without compiling -- not just the whole images below.

`images/<name>-<image_version>.tar.gz` — exactly the filename an
importing host computes for the image-artifact fast path (ADR-0123):
it fetches `<base_url>/images/<name>-<hash>.tar.gz`, verifies it against
the recipe's own `image_artifact_sha256`, and extracts it as that
image's entire rootfs. So this directory can be served as-is, with no
translation step.

`MANIFEST.json` — each image's version, artifact sha256 and size. The
sha256 is what an image recipe's `image_artifact_sha256=` must contain.

## What is here, and what is deliberately NOT

Binaries only. Every artifact in this directory is opaque bytes that
some recipe elsewhere vouches for.

The recipes themselves live in the repository (`recipes/image/*` and
`recipes/package/*`) and are NOT served from here. That separation is
ADR-0122's two-URL split and it is the whole security model:

  1. the recipe comes from git -- versioned, reviewable, diffable text
  2. the artifact comes from a plain HTTP server -- this directory
  3. the recipe's own checksum validates the artifact

so the artifact server is never a trust boundary. A server that shipped
both the payload and the checksum that approves it would be vouching for
itself, which is worth nothing.

## Provisioning a new host

    # 1. serve THIS directory (binaries) from a machine the box can reach
    cd export-artifacts && python3 -m http.server 8080 --bind <lan-ip>

    # 2. point the box at the binaries...
    cixctl --host=<newbox> pkg artifact-config set --base-url=http://<lan-ip>:8080

    # 3. ...and, separately, at the recipes in git
    cixctl --host=<newbox> pkg repo-config set --repo-url=https://git.home.arpa/itdlabs/cix.git
    cixctl --host=<newbox> pkg sync

    # 4. apply an image recipe -- it pulls the whole rootfs and compiles nothing
    cixctl --host=<newbox> image apply-recipe gcc-tcc-bootstrap

## Serving it

    cd export-artifacts && python3 -m http.server 8080 --bind <lan-ip>

Then on the importing host:

    cixctl --host=<newbox> pkg artifact-config set --base-url=http://<lan-ip>:8080

## How these were extracted

The host's own export endpoint could not be used: `tar -z` shells out to
a bare `gzip` through PATH, and the daemon runs as PID 1 with no PATH
(issue #125), so every host-side compress fails. Extraction instead ran
tar *inside a container* built from each image -- where PATH is normal --
split the result into bounded chunks, and pulled each chunk through
`GET /containers/{name}/files`. Purely REST, no shell on the host.

Images lacking a shell or archiver were handled two ways: `tar`/`bash`
was installed into them first (`jumpbox`, `cix-hosttools`, `dev`), or --
where the image had no tooling at all and its packages were small
(`dns`, `syslog`, `ldap`, `chrony`, `auditbuild`) -- each recorded file
was pulled individually through the same files endpoint, which needs
nothing inside the image.

Per-package artifacts were then derived LOCALLY from the image
tarballs using each package's own recorded file list, so no package
needed a second trip to the host.

Not included: `auditbuild`'s `ncurses@6.6-5` and `libc-dev@2.36-4`
(10,670 files between them, and both are duplicate revisions of
versions already here), and the `__hostbuild` entries, which are this
project's own build outputs rather than installable packages.
