# CONTINUE

Start here if you have no context on this repository. This file is written to
be self-contained: it states what the project is, what is already done, what is
left, and exactly how to reproduce every result so you can trust or refute it.

Read [README.md](README.md) for the design and [REPORT.md](REPORT.md) for the
full per-test results. This file is the working handover.

---

## 1. What this is

An AppImage bundles its own glibc so it runs on any distro. It does **not**
bundle GPU drivers, because Mesa plus LLVM is 100-200 MB. So it must `dlopen`
the **host's** drivers, which were built against a **different libc**: a newer
glibc, or musl on Alpine.

The upstream implementation being fixed is `foreign-dlopen.c` from
[Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages). It is
`LD_PRELOAD`ed, intercepts `dlopen`, and rewrites host objects in a private copy
so their symbol version requirements stop mattering.

The patched version lives in [`src/foreign-dlopen.c`](src/foreign-dlopen.c).

## 2. Environment

Everything runs in throwaway containers. No GPU is needed for any mandatory
test; Mesa's software rasterisers (**lavapipe** for Vulkan, **llvmpipe** for GL)
exercise the identical `dlopen` path.

```
podman 5.8.6 at %LOCALAPPDATA%\Programs\Podman\podman.exe   (NOT on PATH)
podman machine: podman-machine-default, a running WSL2 Fedora 44 VM
Python 3.13 on PATH as `py -3`
```

`scripts/wsl-ephemeral.ps1` creates throwaway WSL2 distros from any OCI image
if you need a real init-less VM rather than a container.

**Bind mounts from Git Bash need `MSYS_NO_PATHCONV=1`**, otherwise Git Bash
mangles the container-side path:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm -v "$PWD:/repo:ro" alpine:3.22 sh -c '...'
```

## 3. Reproduce the current state

### 3.1 The evidence table (3 minutes, the regression gate)

```powershell
.\experiments\run.ps1
```

Expect **22/22 predictions held**. Run this before every commit. A MISMATCH is
a finding, not a harness bug: investigate before coding.

Three container stages over one shared volume: `alpine:3.22` builds a faithful
musl probe, `debian:trixie-slim` (glibc 2.41) builds libraries needing new
symbols, `debian:bullseye-slim` (glibc 2.31) plays "an AppImage bundling an
older glibc" and runs the tests. The repo is mounted at `/repo`, so cases
E14-E21 build and test `src/` as it actually ships.

### 3.2 The musl symbol gap (Tier 0, no Linux needed)

```bash
mkdir -p /tmp/gap && cd /tmp/gap
PYTHONPATH=<repo> py -3 <repo>/gap.py --fetch
```

Expect the union over the Mesa+LLVM closure to be exactly
`['___environ', 'atexit']`.

### 3.3 The Alpine end-to-end suite

This needs the demo AppImage, which is not in the repo:

```bash
curl -sSL -o demo.AppImage \
  "https://github.com/Samueru-sama/Anylinux-AppImages/releases/download/demo/vkcube+glxgears-host-drivers-demo-x86_64.AppImage"
# sha256 712766f8a4dc6b5ea3193ed7bb0282b64c7b781f7334056416edd3d00e8960bd
```

Extract it **inside a container** (the embedded filesystem is DwarFS, and
`--appimage-extract` runs the ELF runtime):

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./demo.AppImage --appimage-extract
# produces ./squashfs-root -> ./AppDir
```

Then build the preload on **bullseye** (glibc 2.31) so it only needs old
symbols, drop it into the AppDir, and run against Alpine:

```bash
# in debian:bullseye-slim, with the repo mounted
cd src && make                      # produces foreign-dlopen.so

# in alpine:3.22, with AppDir and the built .so mounted
apk add --no-cache mesa-vulkan-swrast vulkan-tools
export APPDIR=/w/AppDir XDG_RUNTIME_DIR=/tmp/xdg
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
mkdir -p $XDG_RUNTIME_DIR

# A/B: the same command, feature off then on. Never trust a single-sided run.
ANYLINUX_LIB_FOREIGN_DLOPEN=0 \
  "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib" ./harness /usr/lib/libvulkan_lvp.so

ANYLINUX_LIB_FOREIGN_DLOPEN=1 \
  "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib" \
  --preload "$APPDIR/lib/foreign-dlopen.so" ./harness /usr/lib/libvulkan_lvp.so
```

> Use `ld.so --preload` rather than `LD_PRELOAD` when a musl binary is anywhere
> in the pipeline (`strace`, `env`). `LD_PRELOAD` applies to those too, and
> musl's loader cannot load a glibc `.so`.

Test programs, all built on a glibc host and run under the bundled loader:

| File | What it does |
|---|---|
| [`tests/icd-harness.c`](tests/icd-harness.c) | foreign-load one ICD, resolve `vk_icdGetInstanceProcAddr`, call through it |
| [`tests/vkprobe.c`](tests/vkprobe.c) | bundled Vulkan loader plus host ICD, `vkCreateInstance` then `vkEnumeratePhysicalDevices`, no window system |
| [`tests/corpus.c`](tests/corpus.c) | `dlopen` every `.so` in a directory, print OK/FAIL with the reason |
| [`tests/invariants.c`](tests/invariants.c) | one libc family in `/proc/self/maps`, bundled sonames win |
| [`tests/allocprobe.c`](tests/allocprobe.c) | interpose the allocator family, report every NULL return |

## 4. The one thing that does not work

**`vkcube` does not render on Alpine.** This is the remaining goal.

Everything up to rendering works:

```
vkCreateInstance          : 0 (VK_SUCCESS)
vkEnumeratePhysicalDevices: -1, count=0
```

`-1` is `VK_ERROR_OUT_OF_HOST_MEMORY`, returned by lavapipe from inside
`vkEnumeratePhysicalDevices`. The failure is **past symbol resolution**.

### Already ruled out, by measurement

| Hypothesis | How it was tested | Result |
|---|---|---|
| Missing symbols | `ANYLINUX_LIB_FOREIGN_DRYRUN=1` over the whole ICD closure | zero unresolvable strong imports |
| A real allocation failure | [`tests/allocprobe.c`](tests/allocprobe.c) interposing `malloc`/`calloc`/`realloc`/`posix_memalign`/`aligned_alloc` | **0 NULL returns**, so the error code is a stand-in |
| The `___environ` rename | A/B with `ANYLINUX_LIB_FOREIGN_NORENAME=1` | byte-identical failure |
| Duplicate `libstdc++`/`libgcc_s` | [`tests/invariants.c`](tests/invariants.c) provenance check | was real, is fixed, failure persists |
| `issetugid` missing | added to the shim | corpus went 243 to 247, failure persists |
| Display or WSI, not libc | run under `xvfb-run -a`; the error is a device error | not the cause |
| Host driver broken | `vulkaninfo --summary` natively on Alpine | lavapipe healthy |

### The strongest lead

`strace` shows the working musl path and the failing glibc path are
**byte-identical up to and including the two `sysinfo` calls** in lavapipe's
device init. The musl path then continues to `/proc/meminfo`; the glibc path
makes **no further syscalls at all** and returns the error. The divergence is a
pure userspace decision inside Mesa, after the memory queries and before any
allocation.

Capture it like this:

```bash
# needs --cap-add SYS_PTRACE --security-opt seccomp=unconfined
apk add --no-cache strace
strace -f -o /tmp/glibc.log "$APPDIR/lib/ld-linux-x86-64.so.2" \
  --library-path "$APPDIR/lib" --preload "$APPDIR/lib/foreign-dlopen.so" ./vkprobe
strace -f -o /tmp/musl.log vulkaninfo --summary      # the working reference
```

### What to try next, in order

1. **Port Solo's `dev/abi_probe.c`** and run it over the loaded closure. This is
   the highest-value remaining test and the most probable cause. The
   glibc-vs-musl divergences that matter on x86-64:

   | Probe | glibc | musl | Risk |
   |---|---|---|---|
   | `regmatch_t` size | 8 | **16** | index corruption |
   | `struct rusage` | 144 | **272** | overrun |
   | `struct sched_param` | 4 | **48** | overrun on write |
   | `ucontext_t` | 968 | **936** | overrun on write |
   | `FTW_F`/`FTW_D`/`FTW_SL`/`FTW_NS` | 0/1/4/3 | 1/2/5/4 | **silently wrong branch** |
   | `HOST_NAME_MAX` / `NI_MAXHOST` | 64 / 1025 | 255 / 255 | truncation |
   | `O_LARGEFILE` | 0 | 32768 | flag confusion |
   | `FILE` size | 216 | opaque | see 3 below |

   Known to match, so not worth probing: `struct stat`, `dirent`, `sigaction`,
   `siginfo`, `termios`, `tm`, `msghdr`, `passwd`, `group`, `addrinfo`,
   `statvfs`, `statfs`, `flock`, `epoll_event`, `glob`, `hostent`.

2. **Build Mesa from source with symbols** and get a real backtrace out of
   `lvp_enumerate_physical_devices`. Alpine's `libvulkan_lvp.so` is stripped, so
   `gdb` gives nothing useful. This is slower than (1) but definitive.

3. **Test `FILE*` crossing directly.** glibc's `FILE` is 216 bytes and musl's is
   opaque. Pass `stdout` into a foreign object, pass a foreign-`fopen`'d handle
   back out, check ordering and that neither side crashes. This is the hazard
   above that is easiest to hit accidentally.

4. **Interpose more of libc**, the way `allocprobe.c` interposes the allocator,
   and log the last calls before the failure. `getenv`, `sysconf`, `getauxval`,
   `pthread_create` are the interesting ones.

## 5. Everything else that is not done

| Item | Why | Effort |
|---|---|---|
| `glxgears` (the OpenGL path) | shares the ICD/DRI loading path and the same blocker as `vkcube` | follows from section 4 |
| Cross-libc ABI microtests: allocator crossing, `errno` coherence, `FILE*` crossing, mutex/cond sharing | not written | small each, see section 4 item 1 |
| 100 load/unload cycles, 60 s `vkcube`, RSS and fd leak check | blocked: `vkcube` does not render | follows from section 4 |
| Real GPU validation (`radv`/`anv`/`radeonsi`) | no discrete GPU available | needs hardware |
| NVIDIA proprietary driver on musl | no NVIDIA hardware | needs hardware |
| aarch64 | no aarch64 hardware. The code is arch-parameterised (`RS_LDSO`, `RS_TRIPLET`, syscall number fallbacks) but this is unverified | needs hardware |
| Upstreaming the sharun patch | deliberately not done, it is a different repository | hand [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch) to the maintainer |
| Design R running a real GPU workload | the only end-to-end target is the musl case, where Design R correctly declines to switch | needs a newer-glibc host with a GPU |

## 6. Things that will waste your time

Each of these cost real time here. They are recorded so they cost you none.

- **`/bin/true` is not always an ELF binary.** On Rocky 9 it is a 51-byte shell
  script, and `ld.so` answers `file too short`, which looks exactly like a
  broken runtime and is not. `runtime-select` re-execs itself instead.
- **`/proc/self/exe` is the loader, not you**, when a program is started as
  `ld-linux.so --library-path ... ./prog`. The kernel exec'd the loader. Use
  `argv[0]`. This produced a false `SELF-TEST FAILED` on every newer host.
- **`dlerror()` is destructive.** Reading it to log it consumes it, and the
  caller then gets `NULL`. This was a live bug in upstream.
- **`mkstemp` rewrites its template in place.** Reusing a spent template makes
  the second loop a silent no-op. This made a fuzz test "pass" nothing.
- **glibc serves a 16 KB `malloc` from its arena; musl `mmap`s it.** So an
  absent `mmap` in an `strace` comparison proves nothing about whether the
  allocation happened. Compare on syscalls that must appear in both, such as
  `openat` of a specific file.
- **`RTLD_DEFAULT` does not see an object's own dependencies.** A "missing
  symbol" report that only consults the global scope accuses a library of
  missing 446 symbols its own `DT_NEEDED` closure supplies. Check the dependency
  handles too.
- **glibc puts version *names* in `.dynsym`** as zero-sized `SHN_ABS` entries
  (`GLIBC_2.32`, `GLIBC_ABI_DT_RELR`). They are ABI markers, not API. A
  generator that treats them as symbols emits C identifiers containing a dot.
- **Run every runtime test twice**, `ANYLINUX_LIB_FOREIGN_DLOPEN=0` then `=1`. A
  single-sided result cannot distinguish "the fix worked" from "it was already
  falling back to bundled software rendering".
- **Shell scripts must be LF.** A CR becomes `$'...\r'` and yields baffling
  "not found" errors. `.gitattributes` enforces it and `run.ps1` verifies rather
  than trusts.
- **PowerShell corrupts a string piped to a native process.** Mount scripts into
  the container instead. A PowerShell function that leaves native output on the
  success stream returns an array, not your exit code.

## 7. Diagnostic ladder

When something fails, report **which rung caught it**.

1. **Host driver sane?** `vulkaninfo --summary` natively. If this fails, stop.
2. **Display, not libc?** Re-run under `xvfb-run -a`. WSI errors are not this
   project's bug.
3. **Feature on?** `ANYLINUX_LIB_DEBUG=1`. No ` [foreign-dlopen.so] >> ` lines
   means the marker, the env switch, or the `.preload` order is wrong.
4. **Which object, which symbol?** `ANYLINUX_LIB_FOREIGN_DRYRUN=1`, or
   `LD_DEBUG=libs,bindings`. An `undefined symbol: X` names the next candidate.
5. **Is `X` really absent?** Check with `elfsym.py` against the **bundled**
   `libc.so.6`. If present, this is a scope or visibility problem, not
   availability, and needs a different fix.
6. **Is `X` merely re-homed?** musl folds `libm`, `libpthread`, `libdl`, `librt`
   and the resolver into `libc`; glibc splits them out, and glibc 2.34 merged its
   own split libraries back in. Load the library instead of shimming the symbol.
   `fgn_global_scope_libs[]` in `src/foreign-dlopen.c` is the list.
7. **Did the rewrite corrupt the image?** Re-parse
   `$XDG_RUNTIME_DIR/.anylinux-fgn-*.so` with `elfsym.py`.
8. **Loads but misbehaves?** ABI territory. Section 4 of this file.

## 8. Rules that must not be broken

- **Never modify a host file.** Every write goes under `$XDG_RUNTIME_DIR` or
  `$TMPDIR`. `tests/invariants.c` and the checksum comparison in REPORT.md guard
  this.
- **Bundled libraries always beat host libraries**, for everything except the
  libc runtime set when Design R deliberately switches it.
- **Exactly one libc family per process.** Never `dlopen` a second libc. It is
  impossible anyway, and the reason is measured in E8 and E9.
- **Never strip symbol versions partially.** `DT_VERSYM`, `DT_VERNEED`,
  `DT_VERDEF` and `DT_VERDEFNUM` go together. A verdef without its versym
  segfaults `ld.so`.
- **Never touch `ld-linux*`, `libc.so.*`, `ld-musl*`.** `fgn_never_touch[]`
  exists for this. `ld-linux` has no `SONAME`, so `RTLD_NOLOAD` cannot catch it.
- **Do not add library searching to `foreign-dlopen.c`.** Finding libraries is
  `ld.so`'s job, driven by `--library-path`, which sharun assembles. Two search
  implementations would diverge and the C one would be the buggy one. The fix
  belongs in [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch).
- **Regenerate the shim when the bundled glibc changes.**
  `make shim FLOOR=... TARGET=...`. A stale shim interposes over symbols libc now
  provides. `src/forward-shim-manifest.json` records the floor it targets.
- **A test you cannot run is SKIPPED with the specific missing capability
  named.** Never silently omitted, never guessed.

## 9. Repository permissions

Work in `https://github.com/Azathothas/dlopen-experiment` only. `gh` is
authenticated with account-wide scope, so this is a policy rule you enforce
yourself.

**Allowed:** commits, branches, tags, push, issues, PRs and releases on **this
repo only**.

**Forbidden everywhere else:** creating or commenting on issues, PRs,
discussions or reviews; starring, forking, watching or editing any other
repository; pushing to any other remote; any non-`GET` `gh api` call outside
this repo; touching account settings, keys, gists or org membership.

Read-only elsewhere is fine. Upstream projects (`Anylinux-AppImages`, `solo`,
`detour`, `sharun`) are **read-only**: study them, never file anything on them.
If a task seems to need a write outside this repo, stop and ask.
