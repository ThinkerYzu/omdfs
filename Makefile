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

.PHONY: all clean test
all: $(BIN)

test: $(BIN)
	@tests/run-tests.sh ./$(BIN)
	@tests/crash-recovery.sh ./$(BIN)
	@tests/eviction.sh ./$(BIN)
	@tests/hardening.sh ./$(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
