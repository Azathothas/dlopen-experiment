# Build the cross-libc dlopen fix.
#
# Build on the OLDEST glibc you intend to support, not the newest: the products
# only ever need symbols from that floor, so an old build runs everywhere and a
# new one does not. The measured builds in REPORT.md used debian:bullseye-slim
# (glibc 2.31) and the results run unchanged under the demo AppImage's bundled
# glibc 2.44.
#
#   make                      foreign-dlopen.so + runtime-select
#   make shim FLOOR=... TARGET=... MUSL=...   regenerate forward-shim.c
#   make traps AUDIT_LIBC=...  audit version-compat.c against a libc
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
# The shipped forward-shim.c was generated WITH a musl inventory. Leaving it out
# drops 33 of 35 definitions and silently disarms the entire musl bridge, so it
# is a default here rather than something the caller has to remember (issue #1).
MUSL    ?= $(ROOT)/inventories/musl-1.2.5.json
# Whichever libc the trap audit runs against. Any glibc will do; the set only
# grows with newer releases.
AUDIT_LIBC ?= /lib/x86_64-linux-gnu/libc.so.6

all: foreign-dlopen.so runtime-select

# The preload carries the shim: it is already in the global lookup scope, so
# its exports satisfy undefined symbols in anything dlopen'd later (E2, E5).
foreign-dlopen.so: foreign-dlopen.c forward-shim.c version-compat.c
	$(CC) $(SOFLAGS) $(CFLAGS) $^ -o $@ -ldl
	@# 5.1's build check: exactly one exported definition, no accidental dupes.
	@# atexit comes from the musl bridge, pthread_cond_init and realpath from
	@# the version-trap forwarders. A collision between the two files, or a
	@# generator that emitted a name twice, shows up here as 2.
	@for s in atexit pthread_cond_init realpath; do \
	   n=$$(nm -D --defined-only $@ 2>/dev/null | grep -cw $$s); \
	   if [ "$$n" != "1" ]; then echo "FAIL: $$s exported $$n times, want 1"; exit 1; fi; \
	 done; \
	 echo "ok: atexit, pthread_cond_init, realpath each exported exactly once"

runtime-select: runtime-select.c
	$(CC) $(CFLAGS) $< -o $@ -ldl

# Regenerate the shim for a different bundled glibc. This is 5.2 step 4: the
# treadmill becomes a build step instead of a per-bug-report patch.
shim:
	$(PYTHON) $(ROOT)/tools/gen_forward_shim.py \
		--floor $(FLOOR) --target $(TARGET) --musl $(MUSL) \
		--out forward-shim.c --manifest forward-shim-manifest.json

# Does version-compat.c still cover every multi-version trap this libc has?
# Run it against the NEWEST glibc you care about: the set only grows.
traps:
	$(PYTHON) $(ROOT)/tools/version_traps.py $(AUDIT_LIBC) --check version-compat.c

clean:
	rm -f foreign-dlopen.so runtime-select

.PHONY: all shim traps clean
