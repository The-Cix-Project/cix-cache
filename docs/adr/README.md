# Architecture decision records

Each of these records one decision, the state of the world when it was
made, and what it cost. **The figures in them are dated observations,
not current facts** — an ADR's numbers are the evidence that justified
the decision, so they are left alone and date-stamped rather than
refreshed. Restating ADR-0006 with today's store size would make its own
reasoning incoherent.

| | Decision | Status |
|---|---|---|
| [0001](0001-content-addressed-artifact-store.md) | Content-addressed store: blobs keyed by digest, published names as symlinks | accepted |
| [0002](0002-registry-is-a-cache-not-a-catalogue.md) | The registry is a cache, never a catalogue, and never a trust boundary | accepted |
| [0003](0003-push-protocol-and-artifact-immutability.md) | Push protocol, and what a published name may mean | accepted, amended by #7 |
| [0004](0004-epoll-reactor-with-streamed-bodies.md) | epoll reactor with streamed bodies | accepted |
| [0005](0005-a-miss-must-be-visible.md) | A miss must be visible | accepted |
| [0006](0006-packages-are-the-only-tier.md) | Packages are the only tier | accepted; supersedes the image half of 0001–0003 |
| [0007](0007-canonical-artifact-names.md) | Canonical names `name-version-release`, and aliasing the old ones | accepted; architecture section superseded by 0008 |
| [0008](0008-architecture-in-artifact-names.md) | Architecture is part of an artifact's identity | accepted; supersedes 0007's architecture section |
| [0009](0009-ordering.md) | How listings, manifests and versions are ordered | accepted |
| [0010](0010-installer-isos-are-served-and-signed.md) | Installer ISOs are served, and signed | accepted; refines 0006 |
| [0011](0011-brand-typography-and-palette.md) | The dashboard follows the Cix brand system | accepted |

Nothing is currently undecided. Work in flight is tracked in the issue
tracker rather than here.
