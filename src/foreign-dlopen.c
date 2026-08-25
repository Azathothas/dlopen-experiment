/* Foreign dlopen loader
 *
 * Allows loading OpenGL/Vulkan drivers (and their dependency closure) from
 * the HOST system even when those were built against a different libc, like
 * a newer glibc or musl, instead of shipping any driver in the AppImage.
 *
 * Enabled by building with USE_HOST_DRIVERS_EXPERIMENTAL=1 (which drops a
 * .foreign-dlopen-enabled marker into the AppDir) or at runtime with
 * ANYLINUX_LIB_FOREIGN_DLOPEN=1 / =0 to override.
 *
 * Must be preloaded AFTER anylinux.so so its pass-throughs still go
 * through anylinux.so first.
 *
 * Modified version of the foreign dlopen mode previously living in
 * https://github.com/pkgforge-dev/Anylinux-AppImages useful-tools/lib/anylinux.c
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef void *(*dlopen_func_t)(const char *filename, int flags);

#define VISIBLE __attribute__((visibility("default")))

// print to stderr when ANYLINUX_LIB_DEBUG=1
static int foreign_dlopen_debug_enabled(void) {
	const char *v = getenv("ANYLINUX_LIB_DEBUG");
	return v && strcmp(v, "1") == 0;
}

#define DEBUG_PRINT(...) do \
	if (foreign_dlopen_debug_enabled()) \
		fprintf(stderr, " [foreign-dlopen.so] >> " __VA_ARGS__); \
	while (0)

// Foreign dlopen
//
// Loads OpenGL/Vulkan drivers straight from the HOST system even when they
// were built against a different libc (newer glibc or musl), the host libc
// itself never enters the process.
//
// Host objects get their symbol version requirements stripped from a memfd
// copy, turning every reference into a plain name lookup against whatever
// is already loaded. Every version tag goes at once, a verdef without its
// versym table segfaults ld.so. musl drivers carry no version info, they
// just ride along the same dependency resolver.
//
// Enabled with ANYLINUX_LIB_FOREIGN_DLOPEN=1 or =0 to override, otherwise
// automatic when $APPDIR/.foreign-dlopen-enabled exists, a marker file
// quick-sharun creates for USE_HOST_DRIVERS_EXPERIMENTAL builds.
//
// Anything outside of $APPDIR counts as host library, sharun already hands
// the dynamic linker the full search list so bundled libraries keep winning.
// ---------------------------------------------------------------------------

// unknown dynamic tags are ignored by ld.so
#define FGN_NEUTRAL_TAG 0x414e594c /* 'ANYL' */

// prefix for the rewritten images emitted next to XDG_RUNTIME_DIR
#define FGN_TMP_PREFIX ".anylinux-fgn-"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#endif

static int foreign_dlopen_mode(void) {
	const char *v = getenv("ANYLINUX_LIB_FOREIGN_DLOPEN");
	if (v && *v)
		return strcmp(v, "1") == 0 ? 1 : 0;

	// no override, honor the build time opt-in from quick-sharun
	const char *appdir = getenv("APPDIR");
	if (!appdir || !*appdir)
		return 0;

	char marker[PATH_MAX];
	snprintf(marker, sizeof(marker), "%s/.foreign-dlopen-enabled", appdir);
	if (access(marker, F_OK) == 0)
		return 1;

	return 0;
}

// Rewritten images pile up on tmpfs across runs, clear out anything
// older than a day before going about our business
__attribute__((constructor))
static void sweep_stale_foreign_tmp(void) {
	if (!foreign_dlopen_mode())
		return;

	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !*dir)
		dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/tmp";

	DIR *d = opendir(dir);
	if (!d)
		return;

	time_t cutoff = time(NULL) - 86400;
	struct dirent *de;
	char path[PATH_MAX];
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, FGN_TMP_PREFIX, strlen(FGN_TMP_PREFIX)) != 0 &&
		    strncmp(de->d_name, FGN_TMP_PREFIX + 1, strlen(FGN_TMP_PREFIX) - 1) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(path, &st) == 0 && st.st_mtime < cutoff)
			unlink(path);
	}
	closedir(d);
}

static int is_host_library_path(const char *path) {
	const char *appdir = getenv("APPDIR");

	if (!path || *path != '/')
		return 0;

	// never treat anything shipped inside the AppImage as foreign,
	// mind the boundary so /opt/app does not swallow /opt/app-other
	if (appdir && *appdir) {
		size_t len = strlen(appdir);
		while (len > 1 && appdir[len - 1] == '/')
			len--;
		if (strncmp(path, appdir, len) == 0 && (path[len] == '\0' || path[len] == '/'))
			return 0;
	}

	return 1;
}

struct fgn_elf {
	char *map;               // whole file contents, mutable private copy
	size_t size;
	const ElfW(Ehdr) *ehdr;
	const ElfW(Phdr) *phdr;
	int phnum;
	ElfW(Dyn) *dyn;          // dynamic array inside map
	size_t dyn_num;
	const char *strtab;      // dynamic string table inside map
	size_t strsz;
	ElfW(Sym) *dynsym;       // dynamic symbol table inside map, NULL when absent
	size_t dynsym_num;
	size_t gnu_symoffset;    // DT_GNU_HASH symoffset: first HASHED symbol index
};

// Translate a virtual address to a file offset through the PT_LOAD headers,
// (size_t)-1 when not backed by the file
static size_t fgn_vaddr_to_offset(const struct fgn_elf *e, ElfW(Addr) vaddr) {
	for (int i = 0; i < e->phnum; i++) {
		if (e->phdr[i].p_type != PT_LOAD)
			continue;
		if (vaddr >= e->phdr[i].p_vaddr && vaddr < e->phdr[i].p_vaddr + e->phdr[i].p_filesz) {
			size_t off = (size_t)(e->phdr[i].p_offset + (vaddr - e->phdr[i].p_vaddr));
			if (off < e->size)
				return off;
			return (size_t)-1;
		}
	}
	return (size_t)-1;
}

static void fgn_free_elf(struct fgn_elf *e) {
	free(e->map);
	memset(e, 0, sizeof(*e));
}

// Locate .dynsym and work out how many entries it has.
//
// There is no DT_ tag for the symbol count. ld.so never needs one: it reaches
// symbols through the hash tables. We do need one, because the ___environ
// remap (Design B) walks the table. Two sources, in order of trust:
//
//   DT_HASH    nchain IS the symbol count, exactly. Authoritative.
//   DT_GNU_HASH  no count; the last hashed symbol is found by walking the
//                bucket with the highest start index until the chain's
//                terminator bit. symoffset also tells us where the HASHED
//                region begins, which is what makes the remap safe: every
//                index below it is undefined and therefore unhashed.
//
// Anything we cannot bound leaves dynsym NULL and the remap simply declines.
static void fgn_locate_dynsym(struct fgn_elf *e) {
	ElfW(Addr) symtab_vaddr = 0, hash_vaddr = 0, gnu_hash_vaddr = 0;
	ElfW(Xword) syment = sizeof(ElfW(Sym));

	for (size_t i = 0; i < e->dyn_num && e->dyn[i].d_tag != DT_NULL; i++) {
		switch (e->dyn[i].d_tag) {
		case DT_SYMTAB:   symtab_vaddr   = (ElfW(Addr))e->dyn[i].d_un.d_ptr; break;
		case DT_SYMENT:   syment         = (ElfW(Xword))e->dyn[i].d_un.d_val; break;
		case DT_HASH:     hash_vaddr     = (ElfW(Addr))e->dyn[i].d_un.d_ptr; break;
		case DT_GNU_HASH: gnu_hash_vaddr = (ElfW(Addr))e->dyn[i].d_un.d_ptr; break;
		default: break;
		}
	}
	if (!symtab_vaddr || syment != sizeof(ElfW(Sym)))
		return;

	size_t symoff = fgn_vaddr_to_offset(e, symtab_vaddr);
	if (symoff == (size_t)-1)
		return;

	size_t count = 0;

	if (hash_vaddr) {
		size_t ho = fgn_vaddr_to_offset(e, hash_vaddr);
		if (ho != (size_t)-1 && ho + 2 * sizeof(uint32_t) <= e->size) {
			uint32_t nchain;
			memcpy(&nchain, e->map + ho + sizeof(uint32_t), sizeof(nchain));
			count = nchain;
		}
	}

	if (gnu_hash_vaddr) {
		size_t go = fgn_vaddr_to_offset(e, gnu_hash_vaddr);
		if (go != (size_t)-1 && go + 4 * sizeof(uint32_t) <= e->size) {
			uint32_t hdr[4];
			memcpy(hdr, e->map + go, sizeof(hdr));
			uint32_t nbucket = hdr[0], symoffset = hdr[1], bloom_size = hdr[2];
			e->gnu_symoffset = symoffset;

			// Only walk the chains when DT_HASH did not already give us
			// an exact count -- nchain is authoritative, this is not.
			if (!count && nbucket && nbucket < (1u << 24) && bloom_size < (1u << 24)) {
				size_t bo = go + 4 * sizeof(uint32_t) + (size_t)bloom_size * sizeof(ElfW(Addr));
				if (bo + (size_t)nbucket * sizeof(uint32_t) <= e->size) {
					uint32_t last = symoffset;
					for (uint32_t i = 0; i < nbucket; i++) {
						uint32_t b;
						memcpy(&b, e->map + bo + (size_t)i * sizeof(uint32_t), sizeof(b));
						if (b > last)
							last = b;
					}
					size_t co = bo + (size_t)nbucket * sizeof(uint32_t);
					if (last >= symoffset) {
						uint32_t idx = last - symoffset;
						// bounded: the chain array cannot exceed the file
						while (co + ((size_t)idx + 1) * sizeof(uint32_t) <= e->size) {
							uint32_t h;
							memcpy(&h, e->map + co + (size_t)idx * sizeof(uint32_t), sizeof(h));
							if (h & 1)
								break;
							idx++;
						}
						count = (size_t)symoffset + idx + 1;
					} else {
						count = symoffset;
					}
				}
			}
		}
	}

	if (!count)
		return;
	if (count > (e->size - symoff) / sizeof(ElfW(Sym)))
		return;                          // count disagrees with the file, distrust it

	e->dynsym = (ElfW(Sym) *)(e->map + symoff);
	e->dynsym_num = count;
}

static int fgn_parse_elf(struct fgn_elf *e, const char *path) {
	memset(e, 0, sizeof(*e));

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;

	struct stat st;
	// no driver closure comes close to 1 GiB
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(ElfW(Ehdr)) || st.st_size > ((off_t)1 << 30)) {
		close(fd);
		return 0;
	}
	e->size = (size_t)st.st_size;

	e->map = malloc(e->size);
	if (!e->map) {
		close(fd);
		return 0;
	}

	size_t done = 0;
	while (done < e->size) {
		ssize_t n = read(fd, e->map + done, e->size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			fgn_free_elf(e);
			return 0;
		}
		done += (size_t)n;
	}
	close(fd);

	e->ehdr = (const ElfW(Ehdr) *)e->map;
	if (memcmp(e->ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
	    e->ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
	    e->ehdr->e_type != ET_DYN ||
	    e->ehdr->e_ident[EI_CLASS] != (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32)) {
		fgn_free_elf(e);
		return 0;
	}

	if (!e->ehdr->e_phnum || e->ehdr->e_phnum > 4096 ||
	    e->ehdr->e_phoff >= e->size ||
	    e->ehdr->e_phoff + (size_t)e->ehdr->e_phnum * sizeof(ElfW(Phdr)) > e->size) {
		fgn_free_elf(e);
		return 0;
	}
	e->phdr = (const ElfW(Phdr) *)(e->map + e->ehdr->e_phoff);
	e->phnum = (int)e->ehdr->e_phnum;

	// keep hostile PT_LOAD headers in bounds
	for (int i = 0; i < e->phnum; i++) {
		if (e->phdr[i].p_type != PT_LOAD)
			continue;
		if (e->phdr[i].p_offset >= e->size ||
		    e->phdr[i].p_filesz > e->size - e->phdr[i].p_offset) {
			fgn_free_elf(e);
			return 0;
		}
	}

	for (int i = 0; i < e->phnum; i++) {
		if (e->phdr[i].p_type != PT_DYNAMIC)
			continue;
		if (e->phdr[i].p_offset >= e->size ||
		    e->phdr[i].p_filesz > e->size - e->phdr[i].p_offset ||
		    e->phdr[i].p_filesz < sizeof(ElfW(Dyn)))
			break;
		e->dyn = (ElfW(Dyn) *)(e->map + e->phdr[i].p_offset);
		e->dyn_num = e->phdr[i].p_filesz / sizeof(ElfW(Dyn));
		break;
	}
	if (!e->dyn) {
		fgn_free_elf(e);
		return 0;
	}

	ElfW(Addr) strtab_vaddr = 0;
	for (size_t i = 0; i < e->dyn_num && e->dyn[i].d_tag != DT_NULL; i++) {
		if (e->dyn[i].d_tag == DT_STRTAB)
			strtab_vaddr = (ElfW(Addr))e->dyn[i].d_un.d_ptr;
		else if (e->dyn[i].d_tag == DT_STRSZ)
			e->strsz = (size_t)e->dyn[i].d_un.d_val;
	}
	if (strtab_vaddr) {
		size_t off = fgn_vaddr_to_offset(e, strtab_vaddr);
		if (off != (size_t)-1 && off + e->strsz <= e->size)
			e->strtab = e->map + off;
	}

	fgn_locate_dynsym(e);

	return 1;
}

static int fgn_dyn_find(const struct fgn_elf *e, ElfW(Sxword) tag, ElfW(Addr) *out) {
	for (size_t i = 0; i < e->dyn_num && e->dyn[i].d_tag != DT_NULL; i++) {
		if (e->dyn[i].d_tag == tag) {
			*out = (ElfW(Addr))e->dyn[i].d_un.d_ptr;
			return 1;
		}
	}
	return 0;
}

// only trust strings terminated inside the string table
static int fgn_valid_cstr(const struct fgn_elf *e, size_t off) {
	if (!e->strtab || off >= e->strsz)
		return 0;
	return memchr(e->strtab + off, '\0', e->strsz - off) != NULL;
}

static int fgn_has_version_tags(const struct fgn_elf *e) {
	ElfW(Addr) dummy;
	return fgn_dyn_find(e, DT_VERSYM, &dummy) || fgn_dyn_find(e, DT_VERNEED, &dummy);
}

// versions our own libc provides, decides if a host object loads as-is
#define FGN_MAX_VERSIONS 1024
static char *fgn_provider_versions[FGN_MAX_VERSIONS];
static size_t fgn_provider_version_count;
static int fgn_providers_scanned;

static void fgn_collect_versions_from_file(const char *path) {
	struct fgn_elf e;
	if (!fgn_parse_elf(&e, path))
		return;

	ElfW(Addr) verdef_vaddr = 0;
	if (!fgn_dyn_find(&e, DT_VERDEF, &verdef_vaddr) || !e.strtab) {
		fgn_free_elf(&e);
		return;
	}

	size_t off = fgn_vaddr_to_offset(&e, verdef_vaddr);
	if (off == (size_t)-1) {
		fgn_free_elf(&e);
		return;
	}

	const char *base = e.map + off;
	size_t pos = 0;
	for (size_t guard = 0; guard < 4096; guard++) {
		if (pos + sizeof(ElfW(Verdef)) > e.size)
			break;
		const ElfW(Verdef) *vd = (const ElfW(Verdef) *)(base + pos);
		if (vd->vd_version != 1)
			break;
		// skip the VER_FLG_BASE entry, it names the file itself
		if (!(vd->vd_flags & VER_FLG_BASE) && vd->vd_aux && fgn_provider_version_count < FGN_MAX_VERSIONS) {
			const ElfW(Verdaux) *aux = (const ElfW(Verdaux) *)((const char *)vd + vd->vd_aux);
			if (fgn_valid_cstr(&e, aux->vda_name))
				fgn_provider_versions[fgn_provider_version_count++] = strdup(e.strtab + aux->vda_name);
		}
		if (!vd->vd_next)
			break;
		pos += vd->vd_next;
	}

	fgn_free_elf(&e);
}

#define FGN_CACHE_MAX 128
static struct {
	char *key;    // canonicalized source path
	void *handle; // dlopen handle once fully loaded
} fgn_cache[FGN_CACHE_MAX];
static volatile int fgn_cache_lock;

static void fgn_lock_cache(void) {
	while (__sync_lock_test_and_set(&fgn_cache_lock, 1))
		sched_yield();
}

static void fgn_unlock_cache(void) {
	__sync_lock_release(&fgn_cache_lock);
}

static void *fgn_cache_get(const char *key) {
	void *handle = NULL;
	fgn_lock_cache();
	for (size_t i = 0; i < FGN_CACHE_MAX; i++) {
		if (fgn_cache[i].key && strcmp(fgn_cache[i].key, key) == 0) {
			handle = fgn_cache[i].handle;
			break;
		}
	}
	fgn_unlock_cache();
	return handle;
}

static void fgn_scan_providers(void) {
	// lock guards against concurrent first scans duplicating entries
	fgn_lock_cache();
	if (fgn_providers_scanned) {
		fgn_unlock_cache();
		return;
	}
	fgn_providers_scanned = 1;

	// Our own libc is the only meaningful provider of glibc symbols, find
	// its on-disk location through a symbol known to live in it and parse
	// that file so we get the same view of the version definitions as ld.so
	Dl_info info;
	void *sym = dlsym(RTLD_DEFAULT, "malloc");
	if (sym && dladdr(sym, &info) && info.dli_fname)
		fgn_collect_versions_from_file(info.dli_fname);

	fgn_unlock_cache();

	DEBUG_PRINT("foreign: our libc provides %zu known versions\n", fgn_provider_version_count);
}

static int fgn_have_version(const char *name) {
	for (size_t i = 0; i < fgn_provider_version_count; i++) {
		if (strcmp(fgn_provider_versions[i], name) == 0)
			return 1;
	}
	return 0;
}

// 1 when every required version comes from our libc, objects without
// version info (musl built) are trivially satisfied
static int fgn_requirements_satisfied(const struct fgn_elf *e) {
	ElfW(Addr) verneed_vaddr = 0;
	if (!fgn_dyn_find(e, DT_VERNEED, &verneed_vaddr))
		return 1;
	if (!e->strtab)
		return 0;

	size_t off = fgn_vaddr_to_offset(e, verneed_vaddr);
	if (off == (size_t)-1)
		return 0;

	const char *base = e->map + off;
	size_t pos = 0;
	for (size_t guard = 0; guard < 4096; guard++) {
		if (pos + sizeof(ElfW(Verneed)) > e->size)
			return 0;
		const ElfW(Verneed) *vn = (const ElfW(Verneed) *)(base + pos);
		if (vn->vn_version != 1)
			return 0;

		size_t apos = pos + vn->vn_aux;
		for (size_t aux_guard = 0; aux_guard < 4096; aux_guard++) {
			if (apos + sizeof(ElfW(Vernaux)) > e->size)
				return 0;
			const ElfW(Vernaux) *aux = (const ElfW(Vernaux) *)(base + apos);
			if (!fgn_valid_cstr(e, aux->vna_name))
				return 0;
			if (!fgn_have_version(e->strtab + aux->vna_name))
				return 0;
			if (!aux->vna_next)
				break;
			apos += aux->vna_next;
		}

		if (!vn->vn_next)
			break;
		pos += vn->vn_next;
	}

	return 1;
}

// references become plain name lookups, every tag must go together or
// the orphaned remainder segfaults ld.so
static void fgn_strip_versions(struct fgn_elf *e) {
	for (size_t i = 0; i < e->dyn_num && e->dyn[i].d_tag != DT_NULL; i++) {
		if (e->dyn[i].d_tag == DT_VERSYM || e->dyn[i].d_tag == DT_VERNEED ||
		    e->dyn[i].d_tag == DT_VERDEF || e->dyn[i].d_tag == DT_VERDEFNUM)
			e->dyn[i].d_tag = FGN_NEUTRAL_TAG;
	}
}

// ---------------------------------------------------------------------------
// Undefined-symbol renaming (Design B)
//
// Some musl names differ from their glibc equivalent only cosmetically. musl's
// environ pointer is ___environ (three underscores), glibc's is __environ
// (two). The reference is a WEAK import, so it does not stop the load -- it
// silently resolves to 0 and the driver reads a NULL environment. Latent, and
// exactly the class of bug that "just works until it doesn't".
//
// The fix costs no string edits at all: "___environ" + 1 IS "__environ", so
// advancing st_name by one byte renames the reference. Two properties make
// this total rather than merely likely:
//
//   * the symbol is UNDEFINED, so DT_GNU_HASH does not index it (GNU hash
//     covers only defined symbols, from symoffset onward). No hash fixup.
//   * nothing is written to .dynstr, so .dynstr tail-merging -- measured real:
//     16 of 647 names in libvulkan_lvp.so are suffixes of another -- cannot
//     bite. We only move a pointer that already pointed into that string.
//
// The general case (rename X to Y where Y is NOT a suffix of X) needs an
// in-place .dynstr write, which tail-merging CAN break. fgn_dynstr_write_safe()
// proves no other referenced offset falls inside the range before allowing it,
// and refuses when it cannot prove that. T0.7 checks that it does refuse.
// ---------------------------------------------------------------------------
struct fgn_rename {
	const char *from;
	const char *to;
	const char *why;
};

static const struct fgn_rename fgn_renames[] = {
	{ "___environ", "__environ",
	  "musl spells the environ pointer with three underscores; glibc uses two" },
	{ NULL, NULL, NULL }
};

// Does `off` name a string that some OTHER part of the file still points at
// inside [lo, hi)? Used to refuse an unsafe in-place .dynstr write.
//
// Checks every offset that can reference .dynstr: symbol names, DT_NEEDED /
// SONAME / RPATH / RUNPATH, and the version tables' file and version names.
// Anything it cannot enumerate makes it answer "occupied", i.e. refuse.
static int fgn_dynstr_range_occupied(const struct fgn_elf *e, size_t lo, size_t hi,
                                     size_t exempt_off) {
	if (!e->strtab)
		return 1;

	for (size_t i = 0; i < e->dynsym_num; i++) {
		size_t off = (size_t)e->dynsym[i].st_name;
		if (off == exempt_off || !off)
			continue;
		if (off >= lo && off < hi)
			return 1;
	}

	for (size_t i = 0; i < e->dyn_num && e->dyn[i].d_tag != DT_NULL; i++) {
		ElfW(Sxword) t = e->dyn[i].d_tag;
		if (t != DT_NEEDED && t != DT_SONAME && t != DT_RPATH && t != DT_RUNPATH)
			continue;
		size_t off = (size_t)e->dyn[i].d_un.d_val;
		if (off >= lo && off < hi)
			return 1;
	}

	// Version tables. If either is present but unreadable we refuse rather
	// than assume it holds nothing in range.
	ElfW(Addr) vn_vaddr = 0;
	if (fgn_dyn_find(e, DT_VERNEED, &vn_vaddr)) {
		size_t base = fgn_vaddr_to_offset(e, vn_vaddr);
		if (base == (size_t)-1)
			return 1;
		size_t pos = 0;
		for (size_t guard = 0; guard < 4096; guard++) {
			if (base + pos + sizeof(ElfW(Verneed)) > e->size)
				return 1;
			const ElfW(Verneed) *vn = (const ElfW(Verneed) *)(e->map + base + pos);
			if (vn->vn_file >= lo && vn->vn_file < hi)
				return 1;
			size_t apos = pos + vn->vn_aux;
			for (size_t ag = 0; ag < 4096; ag++) {
				if (base + apos + sizeof(ElfW(Vernaux)) > e->size)
					return 1;
				const ElfW(Vernaux) *aux = (const ElfW(Vernaux) *)(e->map + base + apos);
				if (aux->vna_name >= lo && aux->vna_name < hi)
					return 1;
				if (!aux->vna_next)
					break;
				apos += aux->vna_next;
			}
			if (!vn->vn_next)
				break;
			pos += vn->vn_next;
		}
	}

	ElfW(Addr) vd_vaddr = 0;
	if (fgn_dyn_find(e, DT_VERDEF, &vd_vaddr)) {
		size_t base = fgn_vaddr_to_offset(e, vd_vaddr);
		if (base == (size_t)-1)
			return 1;
		size_t pos = 0;
		for (size_t guard = 0; guard < 4096; guard++) {
			if (base + pos + sizeof(ElfW(Verdef)) > e->size)
				return 1;
			const ElfW(Verdef) *vd = (const ElfW(Verdef) *)(e->map + base + pos);
			size_t apos = pos + vd->vd_aux;
			for (size_t ag = 0; ag < vd->vd_cnt && ag < 4096; ag++) {
				if (base + apos + sizeof(ElfW(Verdaux)) > e->size)
					return 1;
				const ElfW(Verdaux) *aux = (const ElfW(Verdaux) *)(e->map + base + apos);
				if (aux->vda_name >= lo && aux->vda_name < hi)
					return 1;
				if (!aux->vda_next)
					break;
				apos += aux->vda_next;
			}
			if (!vd->vd_next)
				break;
			pos += vd->vd_next;
		}
	}

	return 0;
}

// Rename one undefined symbol. Returns 1 when the rename happened.
//
// Only two routes are allowed, and only the first is ever needed in practice:
//
//   suffix   `to` is a suffix of `from` -> bump st_name. No write at all.
//   in-place `to` is no longer than `from` -> overwrite the bytes, but only
//            after proving nothing else points inside the range (T0.7).
static int fgn_rename_undef_symbol(struct fgn_elf *e, const struct fgn_rename *r,
                                   int dry_run) {
	if (!e->dynsym || !e->strtab)
		return 0;

	size_t flen = strlen(r->from), tlen = strlen(r->to);
	if (tlen > flen)
		return 0;                        // would need to grow .dynstr

	int done = 0;
	for (size_t i = 0; i < e->dynsym_num; i++) {
		ElfW(Sym) *s = &e->dynsym[i];
		if (s->st_shndx != SHN_UNDEF || !s->st_name)
			continue;
		size_t off = (size_t)s->st_name;
		if (!fgn_valid_cstr(e, off))
			continue;
		if (strcmp(e->strtab + off, r->from) != 0)
			continue;

		// Defined symbols are indexed by DT_GNU_HASH; renaming one without
		// rebuilding the chains corrupts lookup. Undefined symbols live below
		// symoffset and are not hashed. Belt and braces: check the index too.
		if (e->gnu_symoffset && i >= e->gnu_symoffset) {
			DEBUG_PRINT("foreign: refusing to rename %s at index %zu: "
			            "inside the DT_GNU_HASH region (symoffset %zu)\n",
			            r->from, i, e->gnu_symoffset);
			continue;
		}

		if (strcmp(e->strtab + off + (flen - tlen), r->to) == 0) {
			// suffix identity: no bytes change, only where we point
			if (!dry_run)
				s->st_name = (ElfW(Word))(off + (flen - tlen));
			DEBUG_PRINT("foreign: %s -> %s (st_name +%zu, no .dynstr write)\n",
			            r->from, r->to, flen - tlen);
			done = 1;
			continue;
		}

		// General case. Clobbering [off, off+flen] breaks any other name that
		// is a suffix of this one -- tail-merging makes that a live hazard,
		// not a theoretical one. Prove it is safe or decline.
		if (fgn_dynstr_range_occupied(e, off + 1, off + flen + 1, off)) {
			DEBUG_PRINT("foreign: refusing in-place rename %s -> %s: another "
			            ".dynstr reference falls inside the range "
			            "(tail-merged strings)\n", r->from, r->to);
			continue;
		}
		if (!dry_run) {
			char *w = (char *)e->strtab + off;
			memcpy(w, r->to, tlen + 1);
		}
		DEBUG_PRINT("foreign: %s -> %s (in-place .dynstr write, %zu bytes)\n",
		            r->from, r->to, tlen + 1);
		done = 1;
	}
	return done;
}

static int fgn_apply_renames(struct fgn_elf *e, int dry_run) {
	// Escape hatch: renaming is the one rewrite that changes SEMANTICS rather
	// than just relaxing a check, so it needs to be switchable when bisecting
	// a misbehaving driver. ANYLINUX_LIB_FOREIGN_NORENAME=1 turns it off.
	const char *off = getenv("ANYLINUX_LIB_FOREIGN_NORENAME");
	if (off && strcmp(off, "1") == 0) {
		DEBUG_PRINT("foreign: symbol renaming disabled by "
		            "ANYLINUX_LIB_FOREIGN_NORENAME=1\n");
		return 0;
	}

	int n = 0;
	for (size_t i = 0; fgn_renames[i].from; i++)
		n += fgn_rename_undef_symbol(e, &fgn_renames[i], dry_run);
	return n;
}

// load chain of the foreign dlopen currently in progress, used both to
// cut off cycles and to know which directories a refused dependency may
// still be hiding in. Thread local, every thread walks its own chain
static __thread const char *fgn_active_stack[16];

// When the linker refuses a dependency over symbol versions the error
// names both the refusing file and who wanted it, e.g.:
//
//   /usr/lib/libLLVM.so: version `GLIBC_2.38' not found (required by ./foo_dri.so)
//
// The one that needs rewriting is the required-by side, harvest its path.
// NULL for every other failure kind, those have nothing to recover
static char *fgn_path_from_dlerror(void) {
	char *err = dlerror();
	if (!err)
		return NULL;

	const char *marker = strstr(err, "(required by ");
	if (!marker)
		return NULL;
	marker += strlen("(required by ");

	const char *end = strchr(marker, ')');
	if (!end || end == marker)
		return NULL;

	// only trust absolute paths, relative ones depend on caller cwd
	if (*marker != '/')
		return NULL;

	return strndup(marker, (size_t)(end - marker));
}

// Fallback hunt next to the current load chain
static char *fgn_find_candidate(const char *name, int depth) {
	char path[PATH_MAX * 2];
	const char *dirs[2 * sizeof(fgn_active_stack) / sizeof(*fgn_active_stack)] = { NULL };
	size_t dir_count = 0;

	for (int i = 0; i <= depth && i < (int)(sizeof(fgn_active_stack) / sizeof(*fgn_active_stack)); i++) {
		const char *slash = strrchr(fgn_active_stack[i], '/');
		if (!slash || slash == fgn_active_stack[i] || dir_count + 1 >= sizeof(dirs) / sizeof(*dirs))
			continue;
		// dedupe is not worth the effort, access() misses are cheap
		dirs[dir_count++] = strndupa(fgn_active_stack[i], slash - fgn_active_stack[i]);
	}

	for (size_t i = 0; i < dir_count; i++) {
		snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
		if (access(path, R_OK) == 0)
			return strdup(path);
	}

	return NULL;
}

// rewritten images land as real files (so realpath() works on them) under
// a content-derived name, meaning repeated runs overwrite instead of
// filling up the runtime dir
#define FGN_TMP_PREFIX ".anylinux-fgn-"

static unsigned fgn_content_hash(const char *s, size_t extra) {
	unsigned h = 2166136261u;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	h ^= (unsigned)extra;
	h *= 16777619u;
	return h;
}

static char *fgn_emit_copy(const char *buf, size_t len, const char *key) {
	int fd = -1;
	char dir[PATH_MAX];
	const char *tmp = getenv("XDG_RUNTIME_DIR");

	if (!tmp || !*tmp)
		tmp = getenv("TMPDIR");
	if (!tmp || !*tmp)
		tmp = "/tmp";
	snprintf(dir, sizeof(dir), "%s", tmp);

	char final[PATH_MAX * 2];
	snprintf(final, sizeof(final), "%s/" FGN_TMP_PREFIX "%08x.so", dir, fgn_content_hash(key, len));
	unlink(final);

	// preferred: invisible tmpfile linked into place, self-cleans on crash
	// and survives as a normal file so realpath() works on it
	fd = open(dir, O_TMPFILE | O_WRONLY, 0600);
	if (fd >= 0) {
		size_t done = 0;
		while (done < len) {
			ssize_t n = write(fd, buf + done, len - done);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			done += (size_t)n;
		}
		if (done == len) {
			int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
			if (dirfd >= 0 && linkat(fd, "", dirfd, strrchr(final, '/') + 1, AT_EMPTY_PATH) == 0) {
				close(dirfd);
				close(fd);
				return strdup(final);
			}
			if (dirfd >= 0)
				close(dirfd);
		}
		close(fd);
	}

	// fallback for filesystems/kernels without O_TMPFILE support, the
	// renamed result intentionally stays so realpath() keeps working
	char *template = malloc(strlen(final) + 8);
	if (!template)
		return NULL;
	strcpy(template, final);
	strcat(template, "XXXXXX");
	fd = mkstemp(template);
	if (fd < 0) {
		free(template);
		return NULL;
	}

	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, buf + done, len - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			free(template);
			return NULL;
		}
		done += (size_t)n;
	}
	close(fd);

	// atomically place it under the deterministic name
	if (rename(template, final) != 0) {
		unlink(template);
		free(template);
		return NULL;
	}
	free(template);
	return strdup(final);
}

static void fgn_cache_put(const char *key, void *handle) {
	fgn_lock_cache();
	for (size_t i = 0; i < FGN_CACHE_MAX; i++) {
		if (!fgn_cache[i].key) {
			fgn_cache[i].key = strdup(key);
			fgn_cache[i].handle = handle;
			break;
		}
	}
	fgn_unlock_cache();
}

// ---------------------------------------------------------------------------
// Unresolved-symbol reporting (Design B, dry-run mode)
//
// Every Mesa object is DF_BIND_NOW, so ld.so resolves the whole symbol table
// at load: one missing symbol makes the library unloadable and the error names
// only the FIRST one. Walking the imports ourselves and testing each against
// the process's own lookup scope reports ALL of them at once, and does it
// without loading anything -- which is what makes this testable with no GPU
// and no Alpine.
//
// This is the generalisable half of the gconv lesson: a plugin
// subsystem that fails loudly with the symbol named is debuggable; one that
// "just randomly breaks" is not.
#define FGN_REPORT_MAX 24
#define FGN_DEPS_MAX   64

// Handles of the DT_NEEDED closure we loaded for the object under
// consideration. An object's imports are satisfied by its own dependencies,
// not only by the process's global scope, so a report that consults only
// RTLD_DEFAULT accuses the object of missing symbols its siblings provide.
// Measured: without this, loading libvulkan_lvp.so "reported" 446 missing
// symbols -- LLVMBuildAdd, drmIoctl, xcb_* -- every one of which its own
// DT_NEEDED closure supplies. The diagnostic was louder than the bug.
struct fgn_deps {
	void *h[FGN_DEPS_MAX];
	size_t n;
};

static int fgn_resolvable(const char *name, const struct fgn_deps *deps) {
	// RTLD_DEFAULT walks the global scope, which includes this preload's own
	// exports -- that is how the shim satisfies imports (E2, E5).
	//
	// NB: no dlerror() call anywhere in here. dlerror() is destructive, and
	// the caller of the intercepted dlopen() needs the real message intact.
	if (dlsym(RTLD_DEFAULT, name))
		return 1;
	if (deps)
		for (size_t i = 0; i < deps->n; i++)
			if (deps->h[i] && dlsym(deps->h[i], name))
				return 1;
	return 0;
}

static int fgn_report_unresolved(const struct fgn_elf *e, const char *what,
                                 const struct fgn_deps *deps, int always) {
	if (!e->dynsym || !e->strtab)
		return -1;

	int missing = 0;
	char names[FGN_REPORT_MAX][128];

	for (size_t i = 0; i < e->dynsym_num; i++) {
		const ElfW(Sym) *s = &e->dynsym[i];
		if (s->st_shndx != SHN_UNDEF || !s->st_name)
			continue;
		// ELF32_ST_BIND and ELF64_ST_BIND are the same shift; spelling it
		// out avoids depending on which of the two link.h picked.
		unsigned char bind = (unsigned char)(s->st_info >> 4);
		if (bind != STB_GLOBAL)
			continue;                    // weak resolves to 0, never fatal
		if (!fgn_valid_cstr(e, (size_t)s->st_name))
			continue;
		const char *name = e->strtab + s->st_name;
		if (!*name)
			continue;

		if (fgn_resolvable(name, deps))
			continue;

		if (missing < FGN_REPORT_MAX) {
			snprintf(names[missing], sizeof(names[0]), "%s", name);
		}
		missing++;
	}

	if (missing && always) {
		fprintf(stderr,
		        "\n [foreign-dlopen.so] >> %s needs %d symbol%s nothing in this process\n"
		        " [foreign-dlopen.so] >> nor its own dependency closure provides:\n",
		        what, missing, missing == 1 ? "" : "s");
		for (int i = 0; i < missing && i < FGN_REPORT_MAX; i++)
			fprintf(stderr, " [foreign-dlopen.so] >>     %s\n", names[i]);
		if (missing > FGN_REPORT_MAX)
			fprintf(stderr, " [foreign-dlopen.so] >>     ... and %d more\n",
			        missing - FGN_REPORT_MAX);
		fprintf(stderr,
		        " [foreign-dlopen.so] >> Most likely the bundled glibc predates them. "
		        "ANYLINUX_RUNTIME=host\n"
		        " [foreign-dlopen.so] >> runs against the host's own libc, which will have them.\n\n");
	} else if (missing) {
		DEBUG_PRINT("%s: %d unresolvable symbol(s), first is %s\n",
		            what, missing, missing ? names[0] : "?");
	} else {
		DEBUG_PRINT("%s: every strong import resolvable\n", what);
	}
	return missing;
}

static int fgn_dryrun_enabled(void) {
	const char *v = getenv("ANYLINUX_LIB_FOREIGN_DRYRUN");
	return v && strcmp(v, "1") == 0;
}

// ---------------------------------------------------------------------------
// Keep the whole glibc family in the GLOBAL scope (Design B, global-scope libraries)
//
// Stripping a version tag turns a reference into a plain name lookup. That
// only works if the name is visible in the process's global scope, and a
// symbol can sit in a DIFFERENT library on the guest's libc than on ours.
// Two independent re-homings bite, and both are measured:
//
//  1. glibc 2.34 merged libpthread/libdl/librt/libutil/libanl into libc.so.6.
//     A modern build emits pthread_create@GLIBC_2.34 with NO DT_NEEDED on
//     libpthread. On a pre-2.34 bundled runtime that symbol lives only in
//     libpthread.so.0, so the stripped lookup succeeds if and only if that
//     library is already loaded (E7) and fails if it is not (E6).
//
//  2. musl puts libm, libpthread, libdl, librt and the resolver INSIDE its
//     libc; glibc splits them out. A musl-built object therefore imports
//     fmod, fesetround, log10, pow with no DT_NEEDED on anything, because on
//     musl its libc edge covered them -- and that edge is exactly the one we
//     drop. Measured on Alpine v3.22: libxml2 failed on `fmod` and libstdc++
//     on `fesetround`, which cascaded into libLLVM and took the whole ICD
//     down. libm.so.6 was simply not in the process.
//
// Both are the same bug shape and both are fixed by making sure every glibc
// library that could hold a re-homed name is loaded RTLD_GLOBAL up front.
// Nothing else guarantees it: the app's own binaries may have no reason to
// pull libm in.
//
// This is rung 6 of the diagnostic ladder applied as a policy rather
// than per incident -- load the library instead of shimming the symbol.
static const char *fgn_global_scope_libs[] = {
	// musl folds these into libc.so; glibc splits them out
	"libm.so.6",
	"libresolv.so.2",
	"libcrypt.so.1",
	// glibc's own pre-2.34 split libraries (E6/E7)
	"libpthread.so.0",
	"libdl.so.2",
	"librt.so.1",
	"libutil.so.1",
	"libanl.so.1",
	NULL
};

__attribute__((constructor))
static void fgn_load_global_scope_libs(void) {
	if (!foreign_dlopen_mode())
		return;

	const char *appdir = getenv("APPDIR");
	char path[PATH_MAX];

	for (size_t i = 0; fgn_global_scope_libs[i]; i++) {
		const char *name = fgn_global_scope_libs[i];

		// Prefer the bundled copy: it is guaranteed to match the bundled
		// libc, and a host copy from a different glibc is precisely the
		// mixed set that segfaults (E11).
		void *h = NULL;
		if (appdir && *appdir) {
			snprintf(path, sizeof(path), "%s/lib/%s", appdir, name);
			if (access(path, R_OK) == 0)
				h = dlopen(path, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
		}
		if (!h)
			h = dlopen(name, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);

		DEBUG_PRINT("global-scope lib %s: %s\n", name, h ? "loaded" : "absent");
	}
}

// core libraries are never stripped nor loaded twice, rewriting the
// dynamic linker or libc is a one way ticket to segfault city. ld-linux
// carries no soname so RTLD_NOLOAD cannot catch it, hence this list
static const char *fgn_never_touch[] = {
	"ld-linux",
	"ld-musl",
	"libc.so.",
	"libc.musl",
	"libm.so.",
	"libm.musl",
	"libpthread.so.",
	"libpthread.musl",
	"libdl.so.",
	"librt.so.",
	NULL
};

// musl objects demand their own libc through sonames like
// libc.musl-x86_64.so.1, a second libc may never enter the process, so
// these dependencies get dropped and every import binds to ours by name
static int fgn_is_musl_libc(const char *name) {
	return strstr(name, ".musl") != NULL || strstr(name, "ld-musl") != NULL;
}

static int fgn_is_core_library(const char *path) {
	const char *basename = strrchr(path, '/');
	basename = basename ? basename + 1 : path;

	for (size_t i = 0; fgn_never_touch[i]; i++) {
		if (strncmp(basename, fgn_never_touch[i], strlen(fgn_never_touch[i])) == 0)
			return 1;
	}
	return 0;
}

// Does the AppDir bundle a library with this soname? Fills `out` and returns 1.
//
// Only $APPDIR/lib is consulted, which is where sharun's lib4bin puts the
// collected closure. Anything reachable only through a lib.path subdirectory
// is deliberately not searched here: this is a "does the bundle already own
// this soname" question, not a second library-search implementation (Design P).
static int fgn_bundled_dep_path(const char *soname, char *out, size_t outsz) {
	const char *appdir = getenv("APPDIR");
	if (!appdir || !*appdir || !soname || !*soname)
		return 0;
	if (strchr(soname, '/'))
		return 0;                        // a path, not a soname
	snprintf(out, outsz, "%s/lib/%s", appdir, soname);
	return access(out, R_OK) == 0;
}

static void *fgn_load(dlopen_func_t dlopen_orig, const char *canon, int flags, int depth) {
	if (depth >= (int)(sizeof(fgn_active_stack) / sizeof(*fgn_active_stack))) {
		DEBUG_PRINT("foreign: %s nested too deep, giving up\n", canon);
		return NULL;
	}

	void *cached = fgn_cache_get(canon);
	if (cached)
		return cached;

	// fast path for anything the process already has loaded, this also
	// keeps our hands off libc, the dynamic linker itself and every
	// bundled library, none of those may ever be rewritten
	void *already_loaded = dlopen_orig(canon, flags | RTLD_NOLOAD);
	if (already_loaded)
		return already_loaded;

	// libc, the dynamic linker and friends are off limits no matter what
	if (fgn_is_core_library(canon))
		return dlopen_orig(canon, flags);

	for (int i = 0; i < depth; i++) {
		if (strcmp(fgn_active_stack[i], canon) == 0) {
			DEBUG_PRINT("foreign: dependency cycle at %s\n", canon);
			return NULL;
		}
	}
	fgn_active_stack[depth] = canon;

	struct fgn_elf e;
	if (!fgn_parse_elf(&e, canon)) {
		DEBUG_PRINT("foreign: could not parse %s\n", canon);
		return dlopen_orig(canon, flags);
	}

	int has_tags = fgn_has_version_tags(&e);
	// the satisfaction check only knows our libc's set, whatever it
	// cannot vouch for gets stripped
	int needs_strip = has_tags && !fgn_requirements_satisfied(&e);

	// pre-pass: classify the dependency edges. musl flavored objects
	// demand musl libc, which gets dropped so nothing poisons the process
	int musl_guest = 0;
	size_t drop_idx[64];
	size_t drop_count = 0;
	for (size_t i = 0; i < e.dyn_num && e.dyn[i].d_tag != DT_NULL; i++) {
		if (e.dyn[i].d_tag != DT_NEEDED || !e.strtab)
			continue;
		ElfW(Word) off = (ElfW(Word))e.dyn[i].d_un.d_val;
		if (!fgn_valid_cstr(&e, off) || !*(&e.strtab[off]))
			continue;
		if (fgn_is_musl_libc(e.strtab + off)) {
			musl_guest = 1;
			if (drop_count < sizeof(drop_idx) / sizeof(*drop_idx))
				drop_idx[drop_count++] = i;
		}
	}
	if (musl_guest)
		needs_strip = 1;

	struct fgn_deps deps = { { NULL }, 0 };

	// closure first, even a satisfiable parent may pull in children
	// that need stripping. The linker resolves glibc world dependencies
	// wherever they live, only a refusal sends us hunting for the file.
	// musl guests skip probing entirely, loading any of their closures
	// unstripped would drag musl libc into the process
	for (size_t i = 0; i < e.dyn_num && e.dyn[i].d_tag != DT_NULL; i++) {
		if (e.dyn[i].d_tag != DT_NEEDED || !e.strtab)
			continue;
		ElfW(Word) off = (ElfW(Word))e.dyn[i].d_un.d_val;
		if (!fgn_valid_cstr(&e, off) || !*(&e.strtab[off]))
			continue;
		const char *dep = e.strtab + off;

		if (fgn_is_core_library(dep))
			continue;

		// T4.2: bundled libraries must beat host libraries.
		//
		// This has to happen BEFORE the host hunt, and it has to happen for
		// musl guests too. Upstream skipped the probe entirely for musl
		// guests -- correctly refusing to load the HOST copy unstripped,
		// since that would drag musl libc in -- but the skip sent them
		// straight to fgn_find_candidate(), which only searches directories
		// on the active load stack. For a host object that is /usr/lib, so a
		// bundled soname could never win.
		//
		// Measured on Alpine: the AppDir bundles libstdc++.so.6.0.36 and
		// libgcc_s.so.1, yet the host's libstdc++.so.6.0.33 and libgcc_s were
		// loaded alongside them. Two libstdc++ and two unwinders in one
		// process is the classic "every symbol resolves and nothing works"
		// configuration, and it is exactly what T4.2 exists to catch.
		//
		// Loading the bundled copy is always safe: it is a glibc object built
		// against the runtime we are already running.
		char bundled[PATH_MAX];
		if (fgn_bundled_dep_path(dep, bundled, sizeof(bundled))) {
			void *bh = dlopen_orig(bundled, flags);
			if (bh) {
				DEBUG_PRINT("foreign: %s -> bundled %s (host copy not used)\n",
				            dep, bundled);
				if (deps.n < FGN_DEPS_MAX)
					deps.h[deps.n++] = bh;
				continue;
			}
			DEBUG_PRINT("foreign: bundled %s present but would not load\n", bundled);
		}

		char *candidate = NULL;
		if (!musl_guest) {
			void *dep_probe = dlopen_orig(dep, flags);
			if (dep_probe)
				continue;
			DEBUG_PRINT("foreign: linker refused %s, looking for the file\n", dep);
			candidate = fgn_path_from_dlerror();
		}
		if (!candidate)
			candidate = fgn_find_candidate(dep, depth);
		if (!candidate)
			continue; // parent load below surfaces the classic error

		char *dep_canon = canonicalize_file_name(candidate);
		free(candidate);
		if (!dep_canon)
			continue;
		DEBUG_PRINT("foreign: loading dependency %s -> %s\n", dep, dep_canon);
		if (is_host_library_path(dep_canon)) {
			void *dh = fgn_load(dlopen_orig, dep_canon, flags, depth + 1);
			// Kept so a later failure report can tell "this object's own
			// closure provides it" from "genuinely nobody provides it".
			if (dh && deps.n < FGN_DEPS_MAX)
				deps.h[deps.n++] = dh;
		}
		free(dep_canon);
	}

	void *handle = NULL;
	char *load_path = NULL;
	int dry_run = fgn_dryrun_enabled();

	if (needs_strip) {
		DEBUG_PRINT("foreign: rewriting %s\n", canon);
		fgn_strip_versions(&e);
		// drop the edges that would pull musl libc in, every import the
		// guest owns binds to our libc by name instead
		for (size_t i = 0; i < drop_count; i++)
			e.dyn[drop_idx[i]].d_tag = FGN_NEUTRAL_TAG;
		// rename the handful of musl spellings that differ only cosmetically
		// from ours, so a WEAK import does not silently resolve to 0
		fgn_apply_renames(&e, dry_run);

		if (dry_run) {
			// Report what WOULD happen and load nothing. Makes the whole
			// rewrite path testable with no GPU and no Alpine.
			fprintf(stderr,
			        " [foreign-dlopen.so] >> DRYRUN %s\n"
			        " [foreign-dlopen.so] >>   version tags: %s\n"
			        " [foreign-dlopen.so] >>   musl NEEDED dropped: %zu\n",
			        canon, has_tags ? "stripped" : "none present", drop_count);
			fgn_report_unresolved(&e, canon, &deps, 1);
			fgn_free_elf(&e);
			return NULL;
		}

		load_path = fgn_emit_copy(e.map, e.size, canon);
		if (load_path) {
			handle = dlopen_orig(load_path, flags);
			if (!handle) {
				// dlerror() is destructive. Read it only when the user asked
				// for a trace; otherwise the plain fallback below produces a
				// fresh message and the CALLER must be able to read it.
				if (foreign_dlopen_debug_enabled()) {
					const char *err = dlerror();
					DEBUG_PRINT("foreign: rewritten load failed: %s\n",
					            err ? err : "unknown");
					// The rewrite is ours, so a failure here is ours to
					// explain. Name every symbol, not just ld.so's first.
					fgn_report_unresolved(&e, canon, &deps, 1);
				}
			}
		}
	} else if (dry_run) {
		fprintf(stderr,
		        " [foreign-dlopen.so] >> DRYRUN %s: no rewrite needed "
		        "(every required version is satisfied)\n", canon);
		fgn_report_unresolved(&e, canon, &deps, 1);
		fgn_free_elf(&e);
		return NULL;
	}

	if (!handle) {
		// plain load fallback, surfaces the classic error message
		// users know how to read
		//
		// Upstream read dlerror() here unconditionally and only DEBUG_PRINTed
		// it, which CONSUMED the message: with debug off the caller's own
		// dlerror() returned NULL and the "classic error message" never
		// reached anyone. Measured -- the T2 harness printed
		// "FAILED: dlopen: (null)". Leave it in place unless tracing.
		handle = dlopen_orig(canon, flags);
		if (!handle && foreign_dlopen_debug_enabled()) {
			const char *err = dlerror();
			DEBUG_PRINT("foreign: plain fallback failed: %s\n", err ? err : "unknown");
			fgn_report_unresolved(&e, canon, &deps, 1);
		}
	}

	fgn_free_elf(&e);
	if (handle)
		fgn_cache_put(canon, handle);
	return handle;
}

// Attempt a foreign load of a host library, *handled tells the caller
// whether an attempt was made
static void *fgn_attempt(dlopen_func_t dlopen_orig, const char *filename, int flags, int *handled) {
	*handled = 0;

	int mode = foreign_dlopen_mode();
	if (!mode || !filename || !*filename) {
		DEBUG_PRINT("attempt bail: mode=%d filename=%s\n", mode, filename ? filename : "(null)");
		return NULL;
	}
	if (flags & RTLD_NOLOAD) {
		DEBUG_PRINT("attempt bail: NOLOAD %s\n", filename);
		return NULL;
	}

	if (*filename != '/')
		return NULL;

	if (!is_host_library_path(filename))
		return NULL;

	char *canon = canonicalize_file_name(filename);
	if (!canon)
		return NULL;
	if (!is_host_library_path(canon)) {
		// symlinks pointing back into the AppImage stay untouched
		free(canon);
		return NULL;
	}

	*handled = 1;
	fgn_scan_providers();

	// host drivers are load once, NODELETE makes sure a stray dlclose
	// from one caller cannot yank mappings other callers still hold,
	// the cache hands the same handle out to everyone
	flags |= RTLD_NODELETE;

	void *result = fgn_load(dlopen_orig, canon, flags, 0);
	free(canon);
	return result;
}

// Intercept dlopen for host libraries
VISIBLE void *dlopen(const char *filename, int flags) {
	dlopen_func_t dlopen_orig = dlsym(RTLD_NEXT, "dlopen");
	if (!dlopen_orig) {
		DEBUG_PRINT("Error getting original dlopen symbol: %s\n", dlerror());
		return NULL;
	}

	// NULL filename means the caller wants a handle to the main program
	if (!filename || !*filename)
		return dlopen_orig(filename, flags);

	int handled = 0;
	void *foreign = fgn_attempt(dlopen_orig, filename, flags, &handled);
	if (handled) {
		if (foreign)
			DEBUG_PRINT("Foreign dlopen success: %s\n", filename);
		else
			DEBUG_PRINT("Foreign dlopen failed: %s\n", filename);
		return foreign;
	}

	DEBUG_PRINT("dlopen pass-through: %s\n", filename);
	return dlopen_orig(filename, flags);
}
