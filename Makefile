# omdfs build
#
# Requires libfuse 3 (libfuse3-dev) and pkg-config.

FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS   := $(shell pkg-config --libs fuse3)

CC      ?= cc
# -Wno-format-truncation: we build paths from PATH_MAX-bounded components into
# PATH_MAX buffers; a real backend path can't exceed PATH_MAX, and truncation
# degrades safely (the path simply fails to resolve and the entry is skipped).
CFLAGS  ?= -Wall -Wextra -Wno-format-truncation -O2 -std=c11 $(FUSE_CFLAGS)
LDFLAGS ?=
LDLIBS  ?= -lpthread $(FUSE_LIBS)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := omdfs

.PHONY: all clean test stress verify
all: $(BIN)

test: $(BIN)
	@tests/run-tests.sh ./$(BIN)
	@tests/crash-recovery.sh ./$(BIN)
	@tests/eviction.sh ./$(BIN)
	@tests/hardening.sh ./$(BIN)
	@tests/clobber.sh ./$(BIN)

# Differential consistency stress test (not part of `make test`; can be long).
# Knobs: STRESS_SEED STRESS_OPS STRESS_ROUNDS STRESS_CACHE STRESS_MAXSZ.
stress: $(BIN)
	@tests/stress.sh ./$(BIN)

# SPIN formal verification (needs `spin`). Does not build omdfs; checks the Promela
# models in spin/ (flush exclusion, write-back engine, crash recovery). See
# spin/run.sh and proj_docs/omdfs/SPIN-VERIFICATION.md.  `make verify VFLAGS=--quick`
# skips the largest bound.
VFLAGS ?=
verify:
	@spin/run.sh --check $(VFLAGS)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
