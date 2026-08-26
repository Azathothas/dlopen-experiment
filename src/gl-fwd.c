/* gl-fwd.c -- stand in for one bundled dispatcher whose plugin the host lacks.
 *
 * THE SHAPE OF THE BUG. Vulkan survives a foreign host because its
 * loader/driver boundary is thin and universal: the bundled libvulkan.so.1
 * dlopens the host's ICD, which exposes one entry point, and foreign-dlopen.so
 * carries that one object across the libc gap. OpenGL has no such boundary on
 * every host. The AppImage bundles libglvnd, whose libGL.so.1 is a DISPATCHER
 * that dlopens a VENDOR library (libGLX_mesa.so.0) -- and a host whose Mesa was
 * built without glvnd ships no vendor library at all. That is every musl distro
 * and every pre-glvnd glibc distro (Ubuntu 14.04, Debian 8). There the bundled
 * dispatcher has nothing to dispatch to, glXChooseVisual returns NULL, and the
 * application prints "couldn't get an RGB, Double-buffered visual" -- a message
 * about visuals, for a fault that is about neither visuals nor libc.
 *
 * THE REPAIR. Replace the dispatcher rather than supply the missing plugin.
 * This object is built with the SONAME of the library it replaces, so ld.so
 * binds an application's DT_NEEDED to it and never loads the bundled one. Its
 * constructor picks a target and every entry point tail-jumps there:
 *
 *   - classic-Mesa host: the host's own libGL.so.1, loaded through
 *     foreign-dlopen.so, which strips version tags, drops the musl libc edge
 *     and bridges the remaining imports. RTLD_GLOBAL, because that is the
 *     shape a DT_NEEDED libGL has natively and classic Mesa's DRI driver
 *     relies on it (see glfwd_open_target).
 *   - glvnd host: the BUNDLED dispatcher, which works there. The shim becomes
 *     one extra jump and nothing else changes.
 *
 * WHY THE TABLE IS GENERATED. A shim that replaces a library must export
 * everything that library exports; anything less is `undefined symbol` for the
 * first application that links a name outside the list. The bundled libGL.so.1
 * exports 3470 entry points. So the list is READ OUT of the object being
 * replaced by tools/gen_gl_fwd.py, and `make gl-syms-check` fails the build if
 * the checked-in table drifts from it.
 *
 * WHY TRAMPOLINES AND NOT WRAPPERS. Each entry point is a two-instruction tail
 * jump through a table slot, not a C function with a hand-written prototype.
 * A tail jump preserves every argument register, the return value and the
 * varargs count in %al, so it forwards ANY signature correctly -- including the
 * ones nobody typed out. A hand-written prototype that disagrees with the real
 * one corrupts arguments silently, and with 3470 entry points that class of bug
 * is not worth carrying. The cost is that a trampoline forwards a CALL:
 * exported data objects cannot be forwarded, and the generator says so.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GLFWD_TABLE
#error "build with -DGLFWD_TABLE=... naming a generated table (see src/Makefile)"
#endif

/* GLFWD_SONAME, GLFWD_COUNT and one GLFWD_SYM(index, name) per entry point.
 * Included several times below with GLFWD_SYM defined differently each time. */
#define GLFWD_SYM(i, n)
#include GLFWD_TABLE
#undef GLFWD_SYM

#ifndef GLFWD_TAG
#define GLFWD_TAG "gl-fwd.so"
#endif
/* The bundled dispatcher looks for a vendor library with this prefix/suffix;
 * its presence on the host is what says the bundled one can still work. */
#ifndef GLFWD_VENDOR_PREFIX
#define GLFWD_VENDOR_PREFIX "libGLX_"
#endif
#ifndef GLFWD_VENDOR_SUFFIX
#define GLFWD_VENDOR_SUFFIX ".so.0"
#endif
/* A directory whose mere non-emptiness also proves a vendor exists, or NULL. */
#ifndef GLFWD_VENDOR_DIR
#define GLFWD_VENDOR_DIR NULL
#endif
/* The API's own extension-resolution entry point. Half of what a glvnd libGL
 * exports is extension entry points that a classic Mesa libGL implements but
 * does not put in .dynsym: the designed way to reach those has always been
 * glXGetProcAddress, so a name dlsym cannot find is asked for that way before
 * it is given up on. Measured on Alpine: 1357 of 3470 by dlsym alone. */
#ifndef GLFWD_GETPROC
#define GLFWD_GETPROC "glXGetProcAddressARB"
#endif

#if defined(__x86_64__)
#  define GLFWD_TRIPLET "x86_64-linux-gnu"
#elif defined(__aarch64__)
#  define GLFWD_TRIPLET "aarch64-linux-gnu"
#elif defined(__i386__)
#  define GLFWD_TRIPLET "i386-linux-gnu"
#else
#  define GLFWD_TRIPLET "unknown"
#endif

static int glfwd_debug(void) {
	const char *v = getenv("ANYLINUX_LIB_DEBUG");
	return v && strcmp(v, "1") == 0;
}

static void glfwd_log(const char *fmt, ...) {
	if (!glfwd_debug())
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs(" [" GLFWD_TAG "] >> ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------ trampolines */
/*
 * glfwd_absent is where an entry point the target does not provide lands. It
 * returns zero in both return registers, which is the right shape for the void,
 * integer and pointer cases and wrong only for a float return -- a case that
 * cannot arise without the target also being wrong. Jumping through a NULL slot
 * instead would be a crash inside a GL call with no explanation, so every slot
 * is initialised to this before anything is resolved.
 */
extern void glfwd_absent(void) __attribute__((visibility("hidden")));

#if defined(__x86_64__)
/* endbr64 is spelled as bytes so the floor's assembler cannot be too old for
 * it, and it is present because an application built with indirect-branch
 * tracking reaches these through a PLT, which is an indirect jump. */
__asm__(".text\n"
        ".globl  glfwd_absent\n"
        ".hidden glfwd_absent\n"
        ".type   glfwd_absent,@function\n"
        "glfwd_absent:\n"
        "	.byte 0xf3,0x0f,0x1e,0xfa\n"
        "	pxor %xmm0, %xmm0\n"
        "	xor  %eax, %eax\n"
        "	ret\n"
        ".size glfwd_absent, .-glfwd_absent\n");
/* No prologue: the callee sees the caller's registers and stack exactly as they
 * were, so the signature never has to be known here. */
#define GLFWD_TRAMPOLINE(i, n)                                                     \
	__asm__(".text\n"                                                         \
	        ".globl " #n "\n"                                                 \
	        ".type " #n ",@function\n"                                        \
	        ".p2align 4\n"                                                    \
	        #n ":\n"                                                          \
	        "	.byte 0xf3,0x0f,0x1e,0xfa\n"                                  \
	        "	jmp *glfwd_tab+8*" #i "(%rip)\n"                              \
	        ".size " #n ", .-" #n "\n");
#elif defined(__aarch64__)
/* UNVERIFIED ON HARDWARE: this machine is x86_64, so the aarch64 form is only
 * assembled (make gl-fwd-asm-check), never run. Same caveat as the arch
 * parameterisation in runtime-select.c, and recorded the same way. */
__asm__(".text\n"
        ".globl  glfwd_absent\n"
        ".hidden glfwd_absent\n"
        ".type   glfwd_absent,%function\n"
        "glfwd_absent:\n"
        "	hint #34\n"
        "	mov  x0, #0\n"
        "	movi d0, #0\n"
        "	ret\n"
        ".size glfwd_absent, .-glfwd_absent\n");
#define GLFWD_TRAMPOLINE(i, n)                                                     \
	__asm__(".text\n"                                                         \
	        ".globl " #n "\n"                                                 \
	        ".type " #n ",%function\n"                                        \
	        ".p2align 4\n"                                                    \
	        #n ":\n"                                                          \
	        "	hint #34\n"                                                   \
	        "	adrp x16, glfwd_tab+8*" #i "\n"                               \
	        "	ldr  x16, [x16, #:lo12:glfwd_tab+8*" #i "]\n"                 \
	        "	br   x16\n"                                                   \
	        ".size " #n ", .-" #n "\n");
#else
#error "gl-fwd needs a tail-jump trampoline for this architecture"
#endif

/* Hidden, so the trampolines reach it PC-relative with no GOT hop and with no
 * chance of another object preempting the table out from under them. */
__attribute__((visibility("hidden"))) void *glfwd_tab[GLFWD_COUNT] = {
#define GLFWD_SYM(i, n) [i] = (void *)(void (*)(void))glfwd_absent,
#include GLFWD_TABLE
#undef GLFWD_SYM
};

/* Emit the trampolines. The arch-specific macro is bound to GLFWD_SYM only for
 * the length of this include: leaving it bound would silently win over the
 * next expansion of the same table, which is exactly what it did once. */
#define GLFWD_SYM(i, n) GLFWD_TRAMPOLINE(i, n)
#include GLFWD_TABLE
#undef GLFWD_SYM

static const char *const glfwd_names[GLFWD_COUNT] = {
#define GLFWD_SYM(i, n) [i] = #n,
#include GLFWD_TABLE
#undef GLFWD_SYM
};

/* ------------------------------------------------------------- discovery */
/*
 * This is a SINGLE-SONAME lookup, not a library search. ld.so cannot answer it
 * -- the name is taken by this object -- so it is answered here, for exactly
 * one name, over the conventional host library directories plus whatever
 * ANYLINUX_GL_HOST_DIR names. Nothing here ever opens a library it was not
 * handed by name, which is the rule foreign-dlopen.c keeps.
 *
 * /usr/lib/<triplet>/mesa and /usr/lib/mesa are on the list because a Debian or
 * Ubuntu host with the alternatives layout keeps classic Mesa's libGL there and
 * glvnd's in the parent directory. The same distros keep classic Mesa's EGL in
 * a *separate* alternatives directory, /usr/lib/<triplet>/mesa-egl (not mesa),
 * so both names are listed for both library families.
 */
static const char *const glfwd_host_dirs[] = {
	"/usr/lib/" GLFWD_TRIPLET, "/lib/" GLFWD_TRIPLET,
	"/usr/lib/" GLFWD_TRIPLET "/mesa",
	"/usr/lib/" GLFWD_TRIPLET "/mesa-egl",
	"/usr/lib64", "/lib64", "/usr/lib64/mesa", "/usr/lib64/mesa-egl",
	"/usr/lib", "/lib", "/usr/lib/mesa", "/usr/lib/mesa-egl",
	"/usr/local/lib", "/usr/local/lib64",
	NULL
};

/* Call fn(dir) for every candidate directory until it returns non-zero. */
static int glfwd_each_dir(int (*fn)(const char *dir, void *ctx), void *ctx) {
	const char *env = getenv("ANYLINUX_GL_HOST_DIR");
	if (env && *env) {
		char *copy = strdup(env);
		if (copy) {
			for (char *p = strtok(copy, ":"); p; p = strtok(NULL, ":")) {
				if (*p == '/' && fn(p, ctx)) {
					free(copy);
					return 1;
				}
			}
			free(copy);
		}
	}
	for (size_t i = 0; glfwd_host_dirs[i]; i++)
		if (fn(glfwd_host_dirs[i], ctx))
			return 1;
	return 0;
}

static int glfwd_try_soname(const char *dir, void *ctx) {
	char *out = ctx;
	char path[PATH_MAX];
	if (snprintf(path, sizeof path, "%s/%s", dir, GLFWD_SONAME) >= (int)sizeof path)
		return 0;
	if (access(path, R_OK) != 0)
		return 0;
	snprintf(out, PATH_MAX, "%s", path);
	return 1;
}

static int glfwd_try_vendor(const char *dir, void *ctx) {
	(void)ctx;
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	int found = 0;
	struct dirent *de;
	size_t plen = strlen(GLFWD_VENDOR_PREFIX), slen = strlen(GLFWD_VENDOR_SUFFIX);
	while (!found && (de = readdir(d)) != NULL) {
		size_t n = strlen(de->d_name);
		if (n > plen + slen &&
		    strncmp(de->d_name, GLFWD_VENDOR_PREFIX, plen) == 0 &&
		    strcmp(de->d_name + n - slen, GLFWD_VENDOR_SUFFIX) == 0)
			found = 1;
	}
	closedir(d);
	return found;
}

/* Does the host carry a plugin the BUNDLED dispatcher could still use? */
static int glfwd_host_has_vendor(void) {
	const char *vdir = GLFWD_VENDOR_DIR;
	if (vdir) {
		DIR *d = opendir(vdir);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.')
					continue;
				glfwd_log("vendor found: %s/%s\n", vdir, de->d_name);
				closedir(d);
				return 1;
			}
			closedir(d);
		}
	}
	return glfwd_each_dir(glfwd_try_vendor, NULL);
}

/* --------------------------------------------------------------- the load */

static void *glfwd_open_target(const char **how) {
	/* foreign-dlopen.so preloads the bundled libc runtime set into the global
	 * scope, and a host object whose musl libc edge was dropped needs that set
	 * to be there BEFORE it is loaded. Preload constructors run in REVERSE of
	 * the .preload order (measured -- E56), so this object's constructor runs
	 * FIRST when it is listed last, which is the wrong way round. Rather than
	 * depend on a loader ordering nobody documents, ask for it. */
	void (*ready)(void) =
	    (void (*)(void))(uintptr_t)dlsym(RTLD_DEFAULT, "foreign_dlopen_init_now");
	if (ready)
		ready();
	else
		glfwd_log("foreign-dlopen.so is not in this process; a host library "
		          "built against another libc will not load\n");
	dlerror();                                  /* a miss above left a message */

	const char *want = getenv("ANYLINUX_GL_FWD_TARGET");
	int force_host    = want && strcmp(want, "host") == 0;
	int force_bundled = want && strcmp(want, "bundled") == 0;

	char bundled[PATH_MAX];
	const char *appdir = getenv("APPDIR");
	int have_bundled = 0;
	if (appdir && *appdir) {
		snprintf(bundled, sizeof bundled, "%s/lib/%s", appdir, GLFWD_SONAME);
		have_bundled = access(bundled, R_OK) == 0;
	}

	int use_bundled = force_bundled ||
	                  (!force_host && have_bundled && glfwd_host_has_vendor());
	if (use_bundled && have_bundled) {
		/* RTLD_GLOBAL so the dispatcher's whole export table reaches the global
		 * scope: the entry points this shim does not own must still resolve. */
		void *h = dlopen(bundled, RTLD_LAZY | RTLD_GLOBAL | RTLD_NODELETE);
		if (h) {
			*how = "bundled dispatcher (the host has a vendor library)";
			glfwd_log("target %s -- %s\n", bundled, *how);
			return h;
		}
		glfwd_log("bundled %s present but would not load: %s\n",
		          bundled, dlerror());
	}

	char host[PATH_MAX];
	host[0] = '\0';
	if (!force_bundled && glfwd_each_dir(glfwd_try_soname, host)) {
		/* RTLD_GLOBAL is the point here, not a detail. Natively an
		 * application's DT_NEEDED libGL.so.1 sits in the global scope, and
		 * classic Mesa's DRI driver imports _glapi_* with NO DT_NEEDED edge on
		 * libglapi.so.0: it expects to find them there because libGL pulled
		 * libglapi in. Loaded RTLD_LOCAL that linkage is invisible and the
		 * driver dies with "undefined symbol: _glapi_tls_Dispatch". Asking for
		 * it HERE, for this one object, reproduces the native shape; making
		 * EVERY foreign dlopen global would additionally hand host definitions
		 * a win over bundled ones that they do not have natively either. */
		void *h = dlopen(host, RTLD_LAZY | RTLD_GLOBAL);
		if (h) {
			*how = "host library (no vendor library for the bundled one)";
			glfwd_log("target %s -- %s\n", host, *how);
			return h;
		}
		glfwd_log("host %s would not load: %s\n", host, dlerror());
	}

	if (!use_bundled && have_bundled) {
		void *h = dlopen(bundled, RTLD_LAZY | RTLD_GLOBAL | RTLD_NODELETE);
		if (h) {
			*how = "bundled dispatcher (nothing better on this host)";
			glfwd_log("target %s -- %s\n", bundled, *how);
			return h;
		}
	}
	*how = "nothing";
	return NULL;
}

__attribute__((constructor))
static void glfwd_init(void) {
	const char *how = "nothing";
	void *target = glfwd_open_target(&how);
	if (!target) {
		/* Every slot still points at glfwd_absent, so GL calls return zero and
		 * the application prints its own documented failure instead of crashing
		 * through a NULL. Say so: "no vendor" and "no host library" produce the
		 * same message from the application. */
		glfwd_log("%s: no target; all %d entry points return zero\n",
		          GLFWD_SONAME, (int)GLFWD_COUNT);
		dlerror();
		return;
	}

	/* REFUSE TO FORWARD TO OURSELVES.
	 *
	 * This object's SONAME is the one it is impersonating, so if anything ever
	 * resolves that name back to this object -- ld.so matching a request
	 * against our own libname list, an ANYLINUX_GL_HOST_DIR pointing at the
	 * preload's own directory, a future glibc that dedups by SONAME after load
	 * -- every trampoline would jump to itself. That is an unbounded recursion
	 * inside the first GL call, with no message and a stack overflow for a
	 * diagnostic. It costs one dladdr to make it a sentence instead.
	 *
	 * dladdr rather than comparing handles: the handle for a path and the
	 * handle for a soname can differ for the same object, and the question here
	 * is which FILE the addresses land in. */
	Dl_info self, tgt;
	void *probe = dlsym(target, glfwd_names[0]);
	if (probe && dladdr((void *)(uintptr_t)glfwd_absent, &self) && dladdr(probe, &tgt) &&
	    self.dli_fbase == tgt.dli_fbase) {
		glfwd_log("%s: the target resolves back to this shim (%s); refusing to "
		          "forward to ourselves, all %d entry points return zero\n",
		          GLFWD_SONAME, tgt.dli_fname ? tgt.dli_fname : "?",
		          (int)GLFWD_COUNT);
		dlerror();
		return;
	}
	dlerror();

	/* Read as a pointer, called as a function: forbidden by C, required by
	 * POSIX, and the cast through a union is how you say so without inviting
	 * -Wpedantic to argue about it. */
	union { void *p; void *(*fn)(const unsigned char *); } getproc;
	getproc.p = dlsym(target, GLFWD_GETPROC);
	dlerror();

	int got = 0, via_dlsym = 0, via_getproc = 0;
	for (int i = 0; i < (int)GLFWD_COUNT; i++) {
		void *p = dlsym(target, glfwd_names[i]);
		if (p) {
			via_dlsym++;
		} else if (getproc.fn) {
			/* An extension entry point the implementation has but does not
			 * export. Native code reaches these exactly this way. */
			p = getproc.fn((const unsigned char *)glfwd_names[i]);
			if (p)
				via_getproc++;
		}
		if (p) {
			glfwd_tab[i] = p;
			got++;
		}
	}
	/* dlsym sets a message on every miss and the application is entitled to a
	 * clean dlerror(). Same destructive-dlerror family as the two already
	 * recorded in foreign-dlopen.c, reached from a third side. */
	dlerror();

	glfwd_log("%s: %d of %d entry points resolved from the %s "
	          "(%d exported, %d via " GLFWD_GETPROC ", %d absent)\n",
	          GLFWD_SONAME, got, (int)GLFWD_COUNT, how,
	          via_dlsym, via_getproc, (int)GLFWD_COUNT - got);
	if (got == 0)
		glfwd_log("%s: NOT FORWARDED: the target loaded but provided none of "
		          "the %d entry points, so every call returns zero\n",
		          GLFWD_SONAME, (int)GLFWD_COUNT);
}
