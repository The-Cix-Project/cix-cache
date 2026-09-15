#
# cix-cache -- fuzzing harnesses.
#
# EVERY clang invocation in this project lives in this file. The top
# Makefile builds with TCC and only TCC; it reaches this one through a
# `fuzz` target that nothing else depends on, so the shipped daemon and
# `make install` never see a flag from here. See fuzz/README.md and
# docs/adr/0014-fuzzing-under-clang.md.
#
# clang and not gcc because libFuzzer's coverage instrumentation is a
# clang feature; a self-written mutation loop under gcc would explore
# a parser this shape far more slowly for no gain.
#
FUZZ_CC ?= clang
ROOT := ..
BUILD := $(ROOT)/build/fuzz

# -fno-sanitize-recover=all: a sanitizer report has to END the run.
#   Without it UBSan prints and continues, and libFuzzer records no
#   crash -- the fuzzer reports success over a real finding.
#
# -fsanitize=integer: not part of `undefined`, and it is the whole bug
#   class in a parser written against size_t/int/long. headers_end is
#   an int that becomes a size_t before a subtraction.
#
# -D_FORTIFY_SOURCE=0 to match the shipped build: fortify would change
#   which glibc branches are taken, and the point is to fuzz the code
#   as it is compiled for real.
FUZZ_CFLAGS := -g -O1 -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -I$(ROOT)/include \
	-fsanitize=fuzzer,address,undefined,integer \
	-fno-sanitize-recover=all \
	-fno-omit-frame-pointer

TARGETS := $(BUILD)/fuzz_http $(BUILD)/fuzz_http_split $(BUILD)/fuzz_name

.PHONY: all clean check

all: $(TARGETS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/fuzz_http: fuzz_http.c $(ROOT)/src/http.c | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $^ -o $@

$(BUILD)/fuzz_http_split: fuzz_http_split.c $(ROOT)/src/http.c | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $^ -o $@

$(BUILD)/fuzz_name: fuzz_name.c $(ROOT)/src/store.c | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $^ -o $@

#
# ASLR is disabled for every run.
#
# clang-14's ASan fails to map its shadow region against this kernel's
# mmap randomness perhaps one start in five, and dies with a bare
# SIGSEGV before printing a line. That is indistinguishable from a
# finding until you notice the crash carries no input, and a fuzz
# target that fails one run in five for reasons unrelated to the code
# is a target nobody will trust. setarch -R is not a workaround for
# anything in cix-cache -- it is a property of running a 2022 sanitizer
# runtime on a 2026 kernel, and the shipped daemon has no sanitizer
# runtime at all.
#
SETARCH := $(shell command -v setarch 2>/dev/null)
ifeq ($(SETARCH),)
RUN :=
else
RUN := $(SETARCH) -R
endif

#
# A short run over the committed corpus -- a minute per target, not an
# afternoon. Long campaigns are something a developer starts by hand;
# this is the version that belongs in a habit, and it re-runs every
# seed and every committed regression input as it goes.
#
# Findings are written to build/fuzz/corpus-*, NOT back into
# fuzz/corpus/. libFuzzer appends every coverage-increasing input to
# its first corpus argument, which turned eight curated seeds into
# thirteen hundred files of random bytes the first time this was run
# by hand. The committed corpus is a curated thing: seeds that get the
# fuzzer past the early rejects, plus one named entry per real finding.
#
FUZZ_TIME ?= 60

check: all
	@set -e; \
	for t in $(TARGETS); do \
		case $$t in *name) c=name;; *) c=http;; esac; \
		mkdir -p $(BUILD)/corpus-$$c; \
		echo "== $$(basename $$t) over corpus/$$c"; \
		$(RUN) $$t -max_total_time=$(FUZZ_TIME) -print_final_stats=1 \
			-artifact_prefix=$(BUILD)/ \
			$(BUILD)/corpus-$$c corpus/$$c; \
	done
	@echo "fuzz check passed"

clean:
	rm -rf $(BUILD)
