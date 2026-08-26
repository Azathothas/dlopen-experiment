# Build the cross-libc dlopen fix.
#
# Build on the OLDEST glibc you intend to support, not the newest: the products
# only ever need symbols from that floor, so an old build runs everywhere and a
# new one does not. The measured builds in REPORT.md used debian:bullseye-slim
# (glibc 2.31) and the results run unchanged under the demo AppImage's bundled
# glibc 2.44.
#
#   make                       foreign-dlopen.so, gl-fwd.so, egl-fwd.so,
#                              runtime-select
#   make shim FLOOR=... TARGET=... MUSL=...   regenerate forward-shim.c
#   make traps AUDIT_LIBC=...  audit version-compat.c against a libc
#   make gl-syms GLVND=<dir>         regenerate the GL/EGL forwarding tables
#   make gl-syms-check GLVND=<dir>   fail if they no longer match
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
# grows with newer releases, so prefer the newest one you care about.
#
# Found rather than hardcoded: Debian and Ubuntu put it under a triplet
# directory, Arch, Fedora and openSUSE do not, and a default that exists on one
# of those and not the others makes `make traps` a Debian-only command.
AUDIT_LIBC ?= $(firstword $(wildcard \
                /lib/x86_64-linux-gnu/libc.so.6 \
                /usr/lib/x86_64-linux-gnu/libc.so.6 \
                /usr/lib64/libc.so.6 \
                /lib64/libc.so.6 \
                /usr/lib/libc.so.6))

# Where the BUNDLED libglvnd lives. The forwarding tables are read out of it,
# because the shim's one correctness rule is that it exports everything the
# object it replaces exports. Only `make gl-syms*` needs this; an ordinary
# build uses the tables already checked in.
GLVND   ?= $(ROOT)/.tmp/AppDir/lib

all: foreign-dlopen.so gl-fwd.so egl-fwd.so runtime-select

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

# The OpenGL and EGL forwarding shims. Each is built with the SONAME of the
# bundled dispatcher it replaces, so preloading it makes ld.so bind an
# application's DT_NEEDED to it instead. -fcf-protection so the object carries
# the IBT property note that matches the endbr64 the trampolines start with:
# without the note a CET-enforcing host turns indirect-branch tracking off for
# the whole process, which is a silent loss of a mitigation rather than a crash.
gl-fwd.so: gl-fwd.c gl-fwd-gl.h
	$(CC) $(SOFLAGS) $(CFLAGS) -fcf-protection=full \
		-DGLFWD_TABLE='"gl-fwd-gl.h"' -DGLFWD_TAG='"gl-fwd.so"' \
		-Wl,-soname,libGL.so.1 gl-fwd.c -o $@ -ldl
	@$(MAKE) --no-print-directory gl-fwd-verify SO=$@ TBL=gl-fwd-gl.h

# EGL's dispatcher finds its vendor through a JSON directory rather than a
# libGLX_*.so.0, so the marker differs; everything else is the same object.
egl-fwd.so: gl-fwd.c gl-fwd-egl.h
	$(CC) $(SOFLAGS) $(CFLAGS) -fcf-protection=full \
		-DGLFWD_TABLE='"gl-fwd-egl.h"' -DGLFWD_TAG='"egl-fwd.so"' \
		-DGLFWD_VENDOR_PREFIX='"libEGL_"' \
		-DGLFWD_VENDOR_DIR='"/usr/share/glvnd/egl_vendor.d"' \
		-DGLFWD_GETPROC='"eglGetProcAddress"' \
		-Wl,-soname,libEGL.so.1 gl-fwd.c -o $@ -ldl
	@$(MAKE) --no-print-directory gl-fwd-verify SO=$@ TBL=gl-fwd-egl.h

# A shim that exports fewer entry points than its table has is a shim that will
# hand some application `undefined symbol`, and the table is 3470 lines long, so
# the count is checked rather than eyeballed. The SONAME is checked too: without
# it the object is inert -- it loads, forwards nothing, and nothing says why.
gl-fwd-verify:
	@want=$$(sed -n 's/^#define GLFWD_COUNT *//p' $(TBL)); \
	 have=$$(nm -D --defined-only $(SO) | grep -cE ' (T|i) (gl|egl)'); \
	 son=$$(objdump -p $(SO) | sed -n 's/^ *SONAME *//p'); \
	 wson=$$(sed -n 's/^#define GLFWD_SONAME  *"//p' $(TBL) | tr -d '"'); \
	 if [ "$$have" != "$$want" ]; then \
	   echo "FAIL: $(SO) exports $$have entry points, the table says $$want"; exit 1; fi; \
	 if [ "$$son" != "$$wson" ]; then \
	   echo "FAIL: $(SO) soname is '$$son', want '$$wson'"; exit 1; fi; \
	 echo "ok: $(SO) is $$son with all $$have entry points"

runtime-select: runtime-select.c
	$(CC) $(CFLAGS) $< -o $@ -ldl

# Regenerate the shim for a different bundled glibc. This is 5.2 step 4: the
# treadmill becomes a build step instead of a per-bug-report patch.
shim:
	$(PYTHON) $(ROOT)/tools/gen_forward_shim.py \
		--floor $(FLOOR) --target $(TARGET) --musl $(MUSL) \
		--out forward-shim.c --manifest forward-shim-manifest.json

# Read the forwarding tables back out of the bundled libglvnd. Needed only when
# the bundled libglvnd changes; the check target is what makes a silent drift
# impossible, and experiments/40-appimage.sh runs it against the real AppDir.
gl-syms:
	$(PYTHON) $(ROOT)/tools/gen_gl_fwd.py $(GLVND)/libGL.so.1 \
		--prefix gl --soname libGL.so.1 --out gl-fwd-gl.h
	$(PYTHON) $(ROOT)/tools/gen_gl_fwd.py $(GLVND)/libEGL.so.1 \
		--prefix egl --prefix EGL --soname libEGL.so.1 --out gl-fwd-egl.h

gl-syms-check:
	$(PYTHON) $(ROOT)/tools/gen_gl_fwd.py $(GLVND)/libGL.so.1 \
		--prefix gl --soname libGL.so.1 --check gl-fwd-gl.h
	$(PYTHON) $(ROOT)/tools/gen_gl_fwd.py $(GLVND)/libEGL.so.1 \
		--prefix egl --prefix EGL --soname libEGL.so.1 --check gl-fwd-egl.h

# The aarch64 trampolines cannot be RUN here -- this machine is x86_64 -- so
# they are at least assembled, which catches the syntax errors that would
# otherwise sit unnoticed until someone has the hardware. "It assembles" is a
# weaker claim than "it works", and REPORT.md says so in those words.
gl-fwd-asm-check:
	@command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || { \
	   echo "SKIPPED: no aarch64-linux-gnu-gcc. On Debian or Ubuntu:"; \
	   echo "         apt-get install gcc-aarch64-linux-gnu libc6-dev-arm64-cross"; \
	   echo "         (the compiler alone has no headers and fails on dirent.h)"; \
	   exit 0; }
	aarch64-linux-gnu-gcc $(SOFLAGS) -O2 -c \
		-DGLFWD_TABLE='"gl-fwd-gl.h"' gl-fwd.c -o /tmp/gl-fwd-aarch64.o
	@echo "ok: the aarch64 trampolines assemble (not run: no hardware)"

# Does version-compat.c still cover every multi-version trap this libc has?
# Run it against the NEWEST glibc you care about: the set only grows.
traps:
	@if [ -z "$(AUDIT_LIBC)" ]; then \
	   echo "FAIL: no libc.so.6 found in any of the usual places."; \
	   echo "      Pass one: make traps AUDIT_LIBC=/path/to/libc.so.6"; \
	   exit 1; \
	 fi
	$(PYTHON) $(ROOT)/tools/version_traps.py $(AUDIT_LIBC) --check version-compat.c

clean:
	rm -f foreign-dlopen.so gl-fwd.so egl-fwd.so runtime-select

.PHONY: all shim traps clean gl-syms gl-syms-check gl-fwd-verify gl-fwd-asm-check
