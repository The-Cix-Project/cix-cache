# 0014 — fuzzing under clang, shipping under tcc

## Status

Accepted in v2.21.0. Narrows ADR-0001's toolchain mandate by stating
what it does *not* cover, and closes issue #20.

## Context

`cixcached` is a custom HTTP server on the public internet. Two of its
parsers read attacker-controlled bytes before anything has
authenticated the request:

- `http_conn_feed()` / `http_conn_try_parse()`, which the epoll reactor
  drives incrementally — bytes arrive in whatever sizes `read()`
  returns, and the parser runs again after every one of them;
- `store_name_is_valid()` and the name grammar around it, which is the
  whitelist that makes path traversal structurally impossible rather
  than filtered against.

Both were covered by hand-written unit tests, and both had already
shipped one defect that those tests did not catch. Issue #1 was a cap
that measured the wrong region once headers and body shared a buffer —
found in production, by a package that could not be published. The
tests covered what we thought of; the bug was in what we did not.

The project builds with TCC, exclusively, mirroring the Cix project's
ADR-0001. TCC has no sanitizers and no coverage instrumentation. So
either the mandate forbids fuzzing this code at all, or the mandate
means something narrower than "no other compiler may ever run".

## Decision

**The mandate governs what ships, not what a developer may run.**

`fuzz/` holds three libFuzzer harnesses built by clang with ASan,
UBSan and `-fsanitize=integer`. They compile the same `src/http.c` and
`src/store.c` the daemon does, with different flags. `make fuzz`
builds them; `make all` does not depend on it, `make install` never
touches them, and no fuzz binary is packaged or deployed. The shipped
daemon is a TCC build and only a TCC build.

Every clang flag in this repository lives in `fuzz/Makefile`, in one
file, so the boundary is a thing you can see rather than a thing you
have to trust.

Three harnesses and not one:

| harness | what only it can find |
|---|---|
| `fuzz_http` | headers and body in a single buffer — the shape of issue #1 |
| `fuzz_http_split` | the reactor's real shape: N chunks, a parse attempt after each. A terminator straddling two reads is invisible to the first harness |
| `fuzz_name` | the grammar, by **property** rather than by crash |

The third is the one that earns its place. Crashes are the cheap
finding in a name parser. The expensive one is a canonicaliser that
accepts a name and then emits a different one the store rejects —
`store_canonicalize()` renames published entries in place, so that is
an artifact renamed into something no request can resolve again. No
crash would ever reveal it, so the harness asserts it directly, along
with idempotence and comparator antisymmetry.

## Consequences

**Three findings in under twenty minutes of machine time**, two of them
real:

1. **Signed integer overflow in `store_split_display()`**, found in
   ~1,400 runs. The release field is bounded by "all digits" and not by
   a length, so a valid published name may carry two hundred of them;
   accumulating those into an `int` is undefined behaviour, on a
   listing any anonymous client can request. Fixed by clamping. The
   neighbouring canonicaliser never had this bug — it re-renders the
   release as *text* and carries a comment saying `strtol` would
   overflow. One of two places that touch the same digits had thought
   about it.

2. **A closure violation that the fuzzer found in the fix for the
   first one.** Chasing a cosmetic gap — `store_name_is_valid()`
   accepts 254- and 255-character names that then fail to
   canonicalise — the whitelist was narrowed to reserve the two
   characters canonicalisation adds. That narrows the input without
   narrowing the output: a valid 252-character name canonicalises to
   254, which the narrowed whitelist then refuses. The change was
   reverted and the reasoning written into the function, because it is
   the rule that *looks* tidier.

   The original behaviour is kept deliberately. Closure beats
   totality: what the store cannot survive is emitting a name it
   refuses. A name in the top two lengths is merely refused by every
   operation on it, with an error that is correct.

3. Nothing in the HTTP parser, across ~44 million runs between the two
   harnesses. That is the one result here worth reporting as a result:
   the parser that had already shipped a bug is, under ASan, UBSan and
   integer checks, clean at the lengths and split points libFuzzer can
   reach.

**Every finding gets a case in the ordinary C test suite**, not only a
corpus entry. `fuzz/corpus/` needs clang to catch anything; `make test`
runs everywhere, and it is what a deployment gates on.

**The cost is a dev-box dependency.** clang and `libfuzzer-14-dev` are
needed to run `make fuzz`. Nothing else in the project needs them, CI
for the shipped build does not need them, and a developer without them
loses fuzzing and nothing else.

## Alternatives considered

**gcc with ASan/UBSan and a hand-written mutation loop.** Installs
nothing new on a box that already has gcc. Rejected: without coverage
feedback the loop would have to rediscover `GET ` and `.tar.gz` by
chance. Both real findings here came from libFuzzer steering into a
narrow shape — a 20-digit release, a 252-character name — which random
mutation reaches only by accident.

**Not fuzzing, on the grounds that the mandate forbids it.** This is
the reading the decision above rejects. ADR-0001 exists so the
artifact Cix runs is built by one known compiler; a harness that is
never built by `make all`, never installed, and never deployed is not
that artifact. Reading the mandate to forbid it would have left two
defects in a public server to protect a property the harness does not
touch.
