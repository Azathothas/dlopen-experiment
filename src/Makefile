# Build the cross-libc dlopen fix.
#
# Build on the OLDEST glibc you intend to support, not the newest: the products
# only ever need symbols from that floor, so an old build runs everywhere and a
# new one does not. The measured builds in REPORT.md used debian:bullseye-slim
# (glibc 2.31) and the results run unchanged under the demo AppImage's bundled
# glibc 2.44.
#
#   make                      foreign-dlopen.so + runtime-select
#   make shim FLOOR=... TARGET=...   regenerate forward-shim.c for another floor
#   make clean

CC      ?= cc
PYTHON  ?= python3

# -Wno-format-truncation: every snprintf here is deliberately truncating into a
# PATH_MAX buffer, which is the safe behaviour, not a bug to fix.
CFLAGS  ?= -O2 -Wall -Wextra -Wno-format-truncation
SOFLAGS  = -shared -fPIC

ROOT     = ..
FLOOR   ?= $(ROOT)/inventories/appdir.json
TARGET  ?= $(ROOT)/inventories/glibc-2.44.json

all: foreign-dlopen.so runtime-select

# The preload carries the shim: it is already in the global lookup scope, so
# its exports satisfy undefined symbols in anything dlopen'd later (E2, E5).
foreign-dlopen.so: foreign-dlopen.c forward-shim.c
	$(CC) $(SOFLAGS) $(CFLAGS) $^ -o $@ -ldl
	@# 5.1's build check: exactly one exported definition, no accidental dupes.
	@n=$$(nm -D --defined-only $@ 2>/dev/null | grep -cw atexit); \
	 if [ "$$n" != "1" ]; then echo "FAIL: atexit exported $$n times, want 1"; exit 1; fi; \
	 echo "ok: atexit exported exactly once"

runtime-select: runtime-select.c
	$(CC) $(CFLAGS) $< -o $@ -ldl

# Regenerate the shim for a different bundled glibc. This is 5.2 step 4: the
# treadmill becomes a build step instead of a per-bug-report patch.
shim:
	$(PYTHON) $(ROOT)/tools/gen_forward_shim.py \
		--floor $(FLOOR) --target $(TARGET) \
		--out forward-shim.c --manifest forward-shim-manifest.json

clean:
	rm -f foreign-dlopen.so runtime-select

.PHONY: all shim clean
