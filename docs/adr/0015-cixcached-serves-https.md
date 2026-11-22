# 0015 — cixcached serves HTTPS itself

## Status

Accepted, 2026-11-22. Closes the cix-cache half of issue #23: the
server, and `cix cache` as its client. Replaces the
"no TLS here, by design" note `docs/DEPLOYMENT.md` carried; ADR-0002's
invariant is unchanged.

## Context

The owner's direction on #23: move the cache into a Cix container on
gibsson-1, serving `https://cache.uk.home.arpa` with a certificate from
the site CA, and retire the plain-HTTP instance on 192.168.15.31:8080.
Two ways to get there were considered and one was chosen: **TLS inside
`cixcached`**, rather than a proxy in front of it. Cix's recipe catalogue
has no TLS front end (no nginx, haproxy, caddy or stunnel), so a proxy
would have been a second service to package and keep in step, for one
listener.

`DEPLOYMENT.md` said there was no TLS "by design — the registry is not a
trust boundary". The second half is still true and is not what TLS is
for here. A host checks every artifact against the checksum in its git
recipe, so TLS adds nothing to whether a host installs the right bytes,
and this ADR does not claim it does. What crossed the network in clear
was the **push token**, and the operator surface it opens: publish,
delete, gc, import, and an activity log naming every artifact as it
lands. A token sent in clear can be used by anyone on the network path.

Facts this rests on, from the cix maintainer on 2026-11-22:

- Cix containers ship OpenSSL **3.0** (`openssl@3.0.20-9`). A recipe that
  declares the tool gets its headers, and TCC builds against it are
  routine (hostapd). The development box has 3.5.7, so the code is held to
  the 3.0 API. Every OpenSSL call in `src/tls.c` and
  `client/src/httpclient.c` exists in 3.0, and the
  newest is `SSL_OP_IGNORE_UNEXPECTED_EOF`, added in 3.0. Compiling with
  `OPENSSL_API_COMPAT=30000` and `OPENSSL_NO_DEPRECATED` rules out
  deprecated calls but not newer ones. The build against 3.0.20 in the
  Cix recipe is the real proof.
- Cix's PKI writes `tls.crt` (a chain PEM: the leaf, then any
  intermediates) and `tls.key` (RSA 2048, PKCS#8) under `/etc/cix-tls`,
  staged into the container before it starts. A future reissue signals
  the service (Cix ADR-0384 `reload_signal`) after rewriting the files.
- cixd fetches with libcurl, verifying against its bundle plus operator
  trust anchors, with SNI and libcurl's default floor of TLS 1.2. No
  client certificates.

## Decision

**1. One OpenSSL dependency, in one file.** `src/tls.c` is the only file
that includes OpenSSL. The server sees a context and a session and moves
bytes through `tls_recv()`/`tls_send()`, which answer like `read()` and
`write()` on a non-blocking socket. It is linked dynamically, the way
cixd links it. This is the daemon's first link dependency beyond glibc,
and it is taken deliberately: implementing TLS is not an option, and a
static OpenSSL would contradict ADR-0001's "dynamic, never -static".

**2. Settings.**

| key | flag | meaning |
|---|---|---|
| `tls_port=` | `--tls-port=` | HTTPS listener; absent or 0 means none |
| `tls_cert=` | `--tls-cert=` | chain PEM, leaf first |
| `tls_key=` | `--tls-key=` | PEM private key |
| `plain_http=` | `--plain-http=` | `rw` (default), `ro` or `off` |

A config that does not mention them is exactly what it was before.
Combinations that would start a daemon not serving what it was asked
for are refused at startup (`conf_check()`): a TLS port without both
files, files without a port, the same port twice, or `plain_http=off`
with nothing else to serve.

**3. `plain_http=ro` refuses the token, not just writes.** On a read-only
plain port, every method but GET and HEAD gets a 403, before any body is
read. And `operator_ok()`, which every push-token check goes through,
says no on that port however correct the token is. That covers the
operator *reads* (the log, the gc dry run, `root` in status) without a
second list of which endpoints are operator endpoints. Unauthenticated
pulls work as before, so existing hosts keep working during the move.

**4. The certificate is checked at startup and reloadable.** A
certificate or key that cannot be loaded, or that do not match, stops
the server, the same rule as a port that cannot be bound (00f0b63): a
daemon serving plain HTTP when HTTPS was asked for looks healthy and is
not. `SIGHUP` builds a new context from the files. If that fails, the
old context stays in service and the log says why, because a botched
reissue must not take the cache off the network. Connections already
open finish on the context they started with. Before this change SIGHUP
had its default action and terminated the daemon. The unit now carries
`ExecReload=` for it.

**5. TLS 1.2 is the floor.** That matches libcurl's default, so nothing
a Cix host sends is refused. OpenSSL 3's default security level already
refuses anything older, so today the explicit floor changes nothing
observable. It is stated anyway so the floor does not depend on a
library default or a system-wide OpenSSL config. `test_tls` shows 1.1 is
refused; it does not show that this setting is what refuses it.

**6. The reactor stays one thread; the state decides the work.** ADR-0004
is unchanged in shape. An HTTPS connection differs in three ways, all in
`tls_service()`:

- the handler runs according to the connection's *state*, not the event
  that fired, because a TLS read can be waiting for the socket to become
  writable and a write for it to become readable;
- the fd is re-armed for whichever direction the session last waited on;
- bytes the session has already decrypted are invisible to epoll, so a
  reader keeps going while the session holds any.

**7. HTTPS bodies are copied, plain ones are not.** `sendfile()` cannot
pass through TLS. An HTTPS body is read from the blob 64 KiB at a time
and written through the session, and a partial write is resumed with
the same bytes. The cost is one buffer per HTTPS connection and a copy
plus encryption per byte. Plain connections keep `sendfile()`.

## What this does not change

- ADR-0002: the registry is not a trust boundary. A host still verifies
  bytes against its recipe, and losing this store still costs only
  rebuild time.
- How a host finds an artifact: one exact URL, now with an `https://`
  base. A host must use a name in the certificate's SANs
  (`cache.uk.home.arpa`), not an IP address.

## The client: `cix cache` owns its HTTP client now

`cix cache` used a verbatim copy of the Cix repository's
`client/src/httpclient.c`, whose rule was "fix upstream first". That
copy took IPv4 literals only and spoke no TLS, so it could reach
neither `cache.uk.home.arpa` nor any HTTPS port. The
`--host=cache.cix.world --port=443` example in `DEPLOYMENT.md` had
never worked. Upstream had neither feature and nothing in progress. The
cix maintainer filed cix#726 on 2026-11-22 and had not started it. The
owner chose to include the client in #23 rather than wait.

So the copy is replaced, not edited, along with `include/iohelpers.h`,
which only the copy used. The client resolves names with
`getaddrinfo()`, takes `--server=http(s)://HOST[:PORT]`, and verifies
like libcurl does for cixd:

- the chain against `--ca-file`, then `$CIX_CACHE_CA_FILE`, then the
  system store;
- the name (with SNI) or the address against the certificate;
- a TLS 1.2 floor.

A certificate that does not verify is reported as such and not
retried, because it will not verify next time either. The flag and
variable names are the ones cix#726 proposes for cixctl, so the two
tools agree if that lands. A response shorter than its Content-Length
is an error, since a TLS client that reads to close has no other way
to tell a cut-off body from a complete one.

One limit is known. A server that refuses a publish answers before
reading the body, then closes. Over plain HTTP the client still reads
that answer and reports it. Over HTTPS, a write that fails ends the
session, so for a body larger than the socket buffers the status is
lost, and `cix cache` says the server refused partway through without
saying why. Closing that gap needs `Expect: 100-continue` in the
server, which is a change of its own.

The cost is that cix-cache no longer shares a client with Cix. If
cix#726 lands, copying it back is a choice to make then, not an
obligation.

## Out of scope here

The CPDL recipe, the container and its certificate, trusting the site CA
on hosts, repointing repositories, and retiring the old instance all
belong to the Cix side of #23. The copy itself is `tools/seed.sh`, which
uses curl and so already speaks HTTPS.

## Verification

`test_tls` drives a real server with curl and `openssl s_client`. It
covers:

- a 5 MB artifact published and fetched byte for byte over HTTPS, again
  through a rate-limited reader, and again over the read-only plain port;
- every write and every use of the token refused on the plain port and
  accepted over HTTPS;
- TLS 1.1 refused and 1.2 accepted (see section 5 for what that does
  and does not prove);
- SIGHUP swapping the certificate, and a broken reissue leaving the old
  one in service;
- each refused configuration exiting instead of serving.

- `cix cache` over HTTPS by name: status, a 3 MB publish, list,
  `CIX_CACHE_CA_FILE`, a certificate the system store does not trust
  refused without retrying, a trusted certificate that does not name
  the host refused, and a publish to the read-only port reporting its
  403.

Breaking the code on purpose shows the tests catch it:

- removing the read-only gate fails three checks;
- discarding the unsent part of a partial write fails both
  byte-for-byte checks;
- removing the client's hostname check fails the check that a trusted
  certificate must name the host;
- letting SIGPIPE through fails the check that a refused publish
  reports its 403.
