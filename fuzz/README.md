# Fuzzing cix-cache

## This does not touch the toolchain mandate

cix-cache is built with TCC, exclusively, mirroring the Cix project's
ADR-0001. Nothing here changes that.

TCC has no sanitizers and no coverage instrumentation, so the harnesses
in this directory are built by clang instead. They are a **developer
tool**, not a build of the product:

  - `make fuzz` builds them; `make all` does not depend on `fuzz`.
  - `make install` never touches them.
  - No fuzz binary is packaged, deployed, or shipped to anybody.
  - `src/` itself is unchanged -- the harnesses compile the same
    `src/http.c` and `src/store.c` the daemon does, with different
    flags.

The shipped daemon is still the TCC build and only the TCC build. See
docs/adr/0014-fuzzing-under-clang.md.

## Targets

| target            | drives                                          |
|-------------------|-------------------------------------------------|
| `fuzz_http`       | `http_conn_feed` + `http_conn_try_parse`, whole input in one feed |
| `fuzz_http_split` | the same, fed in N chunks with a parse attempt after each -- the shape the epoll reactor actually produces |
| `fuzz_name`       | the name grammar: `store_name_is_valid`, `store_canonical_name`, `store_split_display`, `store_suffix_of`, `store_version_cmp` |

`fuzz_name` asserts properties, not just absence of crashes:

  - a name that canonicalises must produce a name the store accepts;
  - canonicalisation is a fixed point -- `canonical(canonical(x))`
    reports "already canonical" and does not move again;
  - a reported suffix is really a suffix of the name.

A canonicaliser that accepts a name and then emits an invalid one is a
real defect that no crash would ever reveal.

## Running

    make fuzz                  # build
    make fuzz-check            # a minute per target over the corpus
    make fuzz-check FUZZ_TIME=1800

For a long campaign by hand, give libFuzzer a scratch corpus FIRST and
the committed one after it:

    setarch -R ./build/fuzz/fuzz_name -max_total_time=3600 \
        build/fuzz/corpus-name fuzz/corpus/name

Both halves of that matter.

**The scratch directory first.** libFuzzer appends every
coverage-increasing input to its first corpus argument. Pointing it at
`fuzz/corpus/` directly turned eight curated seeds into thirteen
hundred files of random bytes on the first run. The committed corpus
is curated: seeds that get the fuzzer past the early rejects, plus one
named entry per real finding.

**setarch -R.** clang-14's ASan fails to map its shadow region against
this kernel's mmap randomness roughly one start in five and dies with
a bare SIGSEGV before printing a line -- which reads exactly like a
finding until you notice the crash has no input attached. Disabling
ASLR for the run makes it deterministic. It says nothing about
cix-cache: the shipped daemon carries no sanitizer runtime.

The corpora are seeded from real request lines and real artifact names
taken from the test suite, which gets the fuzzer past the parser's
early rejects immediately instead of making it rediscover "GET ".

## When something is found

Fix it in `src/`, then add the crashing input as a case in the ordinary
C test suite -- `test/test_http.c` or `test/test_store.c`. A regression
that lives only in `fuzz/corpus/` needs clang to catch, and the point
of a regression test is that `make test` catches it everywhere.
