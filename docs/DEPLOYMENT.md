# Deployment Guide

## Running the Artifact Server

### Local/LAN Testing
```bash
cd /path/to/cix-cache
python3 -m http.server 8080 --bind <lan-ip>
```

### Configure Cix Hosts to Use This Registry

On each Cix host that will consume artifacts:

```bash
# Point the host at this registry
cixctl --host=<newbox> pkg artifact-config set --base-url=http://<lan-ip>:8080

# Point the host at the recipes in git
cixctl --host=<newbox> pkg repo-config set --repo-url=https://git.home.arpa/itdlabs/cix.git

# Sync package metadata
cixctl --host=<newbox> pkg sync

# Apply an image recipe (pulls pre-built rootfs, no compilation)
cixctl --host=<newbox> image apply-recipe gcc-tcc-bootstrap
```

## Security Model

- **Registry is NOT a trust boundary** — it serves opaque bytes only
- **Recipes are in git** — versioned, reviewable, immutable
- **Checksums validate artifacts** — each recipe's `image_artifact_sha256` or `pkg_artifact_sha256` validates the downloaded binary
- **Separation of concerns** — this design ensures the registry cannot be exploited to inject malicious code

See `docs/DESIGN.md` for full architectural rationale and constraints.

## Push Capability (Planned)

Future versions will support builders pushing new artifacts via `PUT`:
```bash
PUT /artifacts/<name>-<version>.tar.gz
PUT /artifacts/images/<name>-<hash>.tar.gz
HEAD /artifacts/<name>-<version>.tar.gz  # Check if already exists
```

## Contents

- `cache/images/` — 5 whole-image rootfs artifacts (3.6 GB)
  - `cix-builder` — complete build environment with GCC 16.2.0-11
  - `cix-hosttools` — minimal host tools
  - `dev` — development environment
  - `gcc-tcc-bootstrap` — bootstrap GCC self-hosted from TCC (no external compiler)
  - `jumpbox` — standalone utility container

- `cache/packages/` — 66 individual package artifacts (912 MB)
  - Core tools: bash, coreutils, grep, sed, make, etc.
  - Compilers: gcc, binutils, tcc
  - Libraries: openssl, curl, zlib, libc-dev, etc.

All artifacts are listed in `MANIFEST.json` with version, size, and SHA256.
