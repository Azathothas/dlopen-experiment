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

**The state as of this handover: it works, and the experiment is closed.** On
Alpine, the demo AppImage's bundled glibc 2.44 drives Alpine's own musl-built
Mesa, `vkcube` renders, and exactly one libc family is in the process. On a
glibc host it drives NVIDIA's closed-source CUDA userspace on a real RTX 3050 Ti,
and renders OpenGL on that GPU at over 100 FPS. Section 3.3 reproduces all of it
in one command.

Section 4 is what is left, and every item on that list is now blocked by
hardware, by distribution packaging, or by the repository-permission rule in
section 8 -- not by anything unwritten. If you are here to add work rather than
to verify it, read section 4 first and be honest with yourself about which of
those you can actually unblock.

## 2. Environment

Everything runs in throwaway containers. No GPU is needed for any mandatory
test; Mesa's software rasterisers (**lavapipe** for Vulkan, **llvmpipe** for GL)
exercise the identical `dlopen` path, and the cases that do need hardware are
SKIPPED by name without it.

```
podman 5.8.6 at %LOCALAPPDATA%\Programs\Podman\podman.exe   (NOT on PATH)
podman machine: podman-machine-default, a running WSL2 Fedora 44 VM
Python 3.13 on PATH as `py -3`
```

**There is a GPU, and reaching it is not obvious.** This machine has a discrete
NVIDIA RTX 3050 Ti Laptop (driver 580.97) and an Intel Iris Xe. WSL2 exposes
both through `/dev/dxg` paravirtualisation and bind-mounts the driver userspace
from Windows at `/usr/lib/wsl`; it publishes **no `/dev/dri` at all**, so `radv`,
`anv` and `radeonsi` are out regardless. Two flags put a container on the
silicon:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm \
  --device /dev/dxg -v /usr/lib/wsl:/usr/lib/wsl:ro \
  debian:trixie-slim sh -c 'LD_LIBRARY_PATH=/usr/lib/wsl/lib \
      /usr/lib/wsl/lib/nvidia-smi -L'
# GPU 0: NVIDIA GeForce RTX 3050 Ti Laptop GPU (UUID: GPU-df849629-...)
```

For **graphics**, the route is Mesa's `d3d12` Gallium driver, which needs no DRM
node and which Debian packages as `dri/d3d12_dri.so`:

```bash
GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
  xvfb-run -a -s '-screen 0 1024x768x24 +extension GLX +render' glxgears -info
# GL_RENDERER = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)   115 FPS
```

`MESA_D3D12_DEFAULT_ADAPTER_NAME` is the only way to choose between the two
GPUs; without it you get the Intel one, which is still hardware and still a
valid result, just not the one you probably meant.

`scripts/wsl-ephemeral.ps1` creates throwaway WSL2 distros from any OCI image
if you need a real init-less VM rather than a container.

**Bind mounts from Git Bash need `MSYS_NO_PATHCONV=1`**, otherwise Git Bash
mangles the container-side path:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm -v "$PWD:/repo:ro" alpine:3.22 sh -c '...'
```

`.tmp/` is gitignored scratch. `appimage.ps1` caches the demo AppImage and the
extracted AppDir there, so the second run is much faster than the first.

## 3. Reproduce the current state

### 3.1 The evidence table (3 minutes, the regression gate)

```powershell
.\experiments\run.ps1
```

Expect **31/31 predictions held**. Run this before every commit. A MISMATCH is
a finding, not a harness bug: investigate before coding.

Three container stages over one shared volume: `alpine:3.22` builds a faithful
musl probe, `debian:trixie-slim` (glibc 2.41) builds libraries needing new
symbols, `debian:bullseye-slim` (glibc 2.31) plays "an AppImage bundling an
older glibc" and runs the tests. The repo is mounted at `/repo`, so cases
E14-E29 build and test `src/` as it actually ships.

### 3.2 The musl symbol gap (Tier 0, no Linux needed)

```bash
mkdir -p /tmp/gap && cd /tmp/gap
PYTHONPATH=<repo> py -3 <repo>/gap.py --fetch
```

Expect the union over the Mesa+LLVM closure to be exactly
`['___environ', 'atexit']`.

### 3.3 The end-to-end proof (15 minutes the first time)

```powershell
.\experiments\appimage.ps1
```

Expect **31/31 on the glibc host** and **25/25 with six named skips on musl**.
It downloads the demo AppImage once into `.tmp` (sha256 verified), extracts it
inside a container because the payload is DwarFS, builds `src/` on the glibc
2.31 **floor**, builds the musl half of the ABI probe on Alpine, and then runs
the same suite on `alpine:3.22` and `debian:trixie-slim`.

**On a machine with no GPU the count is lower and that is correct.** The driver
probes for `/dev/dxg` plus a bind-mountable `/usr/lib/wsl` by running a
throwaway container, and when either is absent E41-E53 are SKIPPED with the
capability named. Six skips on Alpine here are the same mechanism: no libglvnd
vendor library, no host glibc runtime to switch to, and no d3d12 driver.

Every case runs the feature **off and on**, and against **both** the
`foreign-dlopen.so` shipped inside the AppImage and the one built from `src/`.
That is not thoroughness for its own sake: see section 6, where "it rendered
with the feature off" turns out not to mean what it looks like.

The headline rows, on Alpine:

```
E30  AppImage as shipped, feature on   NO-DEVICES  (segfault)
E31  control, feature off              NO-DEVICES  (VK_ERROR_INCOMPATIBLE_DRIVER)
E32  this repo, feature on             DEVICES     (llvmpipe)
E37a AppImage as shipped, vkcube       reported zero accessible devices
E37  this repo, vkcube                 Selected GPU 0: llvmpipe (LLVM 20.1.8)
E40  one file replaced, no variables    Selected GPU 0: llvmpipe (LLVM 20.1.8)
E41  NVIDIA's closed-source libcuda     4096 bytes round-tripped through the GPU
E43a bindings, AppImage as shipped     MIXED: 5 of 6 condvar entry points
E43  bindings, this repo               UNIFORM: 0 of 6
E44  the same, without the wsl dir     cuInit -> 100 CUDA_ERROR_NO_DEVICE
E46  the vendor's own nvidia-smi        GPU 0: NVIDIA GeForce RTX 3050 Ti Laptop
E46a the same binary, no AppImage       env: can't execute .../nvidia-smi
E49  a musl guest, 26 ABI crossings     ABI CROSSING PASSED
```

and three more on Debian only, which is where there is a host glibc runtime to
switch to and a hardware GL driver to drive:

```
E51  Design R, then a Vulkan device    OK: 1 physical device
E52  Design R, then the CUDA round trip on the GPU
E53a the AppImage on the d3d12 driver  Error: glXCreateContext failed
E53  the same, plus the conf dirs      GL_RENDERER = D3D12 (NVIDIA RTX 3050 Ti)
```

E40 is the one to look at first. Every other case forces something -- the
feature, the ICD, the loader. E40 replaces `lib/foreign-dlopen.so` inside the
AppDir and runs it with no `ANYLINUX_*` and no `VK_DRIVER_FILES` at all, which
is the only form of the claim that matches what was asked.

### 3.4 Driving it by hand

The suite is the reproducible form, but when you are debugging you want the
pieces. Build the preload on **bullseye** so it only needs old symbols, drop it
into the AppDir, and run against Alpine:

```bash
# in debian:bullseye-slim, with the repo mounted
cd src && make                      # foreign-dlopen.so + runtime-select

# in alpine:3.22, with AppDir and the built .so mounted
apk add --no-cache mesa-vulkan-swrast vulkan-tools vulkan-loader
export APPDIR=/w/AppDir XDG_RUNTIME_DIR=/tmp/xdg
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
mkdir -p $XDG_RUNTIME_DIR

# A/B: the same command, feature off then on. Never trust a single-sided run.
ANYLINUX_LIB_FOREIGN_DLOPEN=0 \
  "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib" ./vkprobe

ANYLINUX_LIB_FOREIGN_DLOPEN=1 \
  "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib" \
  --preload "$APPDIR/lib/foreign-dlopen.so" ./vkprobe
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
| [`tests/soak.c`](tests/soak.c) | N load/unload cycles with RSS, fd and rewritten-copy counts |
| [`tests/verprobe.c`](tests/verprobe.c) | the version-binding trap as a loadable probe: returns 0 or 22 |
| [`tests/vertrap.c`](tests/vertrap.c) | the three libc properties `version-compat.c` rests on |
| [`tests/allocprobe.c`](tests/allocprobe.c) | interpose the allocator family; every counter carries its total |
| [`tests/cudaprobe.c`](tests/cudaprobe.c) | foreign-load a closed-source vendor driver, then push bytes to the GPU and read them back |
| [`tests/bindprobe.c`](tests/bindprobe.c) | walk every loaded object's relocations and report which DEFINITION each one bound |
| [`tests/abi-host.c`](tests/abi-host.c) | the cross-libc crossings, against [`abi-guest.c`](tests/abi-guest.c) built by the other libc |

Reaching the GPU by hand needs two flags on the container and one directory on
the library path, none of which is guessable:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm \
  --device /dev/dxg -v /usr/lib/wsl:/usr/lib/wsl:ro \
  -v "$PWD:/repo:ro" -v "$PWD/.tmp:/w" alpine:3.22 sh -c '
    APPDIR=/w/AppDir; LP=$APPDIR/lib
    ANYLINUX_LIB_FOREIGN_DLOPEN=1 APPDIR=$APPDIR \
      "$LP/ld-linux-x86-64.so.2" --library-path "$LP:/usr/lib/wsl/lib" \
      --preload "$LP/foreign-dlopen.so" \
      /w/build/cudaprobe /usr/lib/wsl/lib/libcuda.so.1'
```

Drop `:/usr/lib/wsl/lib` and it still loads, still resolves every entry point,
and then reports `cuInit -> 100 CUDA_ERROR_NO_DEVICE`. That is E44, and it is
the single most misleading failure in this repository.

## 4. What was blocking it, and what is left

### 4.1 The blocker, now fixed

`vkEnumeratePhysicalDevices` returned `VK_ERROR_OUT_OF_HOST_MEMORY` with zero
devices. This was attributed to glibc-vs-musl ABI differences. **It was not
that.**

The measurement that broke it open was reproducing the failure on
`debian:trixie-slim` with **one libc**: Debian's own glibc-built
`libvulkan_lvp.so`, glibc 2.41 on both sides, no musl anywhere. Then the chain
fell out in an afternoon, because Debian ships Mesa's `__FILE__` strings and
`mesa-vulkan-drivers-dbgsym` exists:

```
lvp_device.c:1315            lvp_init_wsi() failed
wsi_display_init_wsi()       -> VK_ERROR_OUT_OF_HOST_MEMORY
wsi_common_display.c:2323    u_cnd_monotonic_init() -> thrd_error
                             pthread_cond_init() -> 22 (EINVAL)
gdb, info symbol $pc         libc+0x909f0 = pthread_cond_init@GLIBC_2.2.5
                             (the working run: libc+0x91b00 = @@GLIBC_2.3.2)
```

`pthread_cond_init@GLIBC_2.2.5` is the pre-2003 compat definition and its whole
body is `if (cond_attr != NULL) return EINVAL;`.

**An unversioned reference does not get the default definition of a symbol.**
A version-stripped object has only unversioned references. So does every
musl-built object, which never had version information at all. That is why the
same failure showed up on Alpine, on Gentoo with a glibc `radv`, and on Debian
once the ICD manifest named an absolute path.

The fix is [`src/version-compat.c`](src/version-compat.c) plus
[`tools/version_traps.py`](tools/version_traps.py); REPORT.md 6.2 has the whole
chain with the commands.

### 4.2 What is genuinely left

Everything on this list is blocked by hardware, by what a distribution ships,
or by the permission rule in section 8. Nothing on it is merely unwritten. If
you find yourself with time and a machine that unblocks a row, that row is the
work; if not, the honest thing is to verify what is here rather than add to it.

| Item | Why it is not done | What would unblock it |
|---|---|---|
| **Vulkan on hardware** | Mesa's Vulkan-on-D3D12 driver (`dzn`) is `microsoft-experimental` and Debian does not package it, so every ICD result here is lavapipe. OpenGL *is* on hardware now (E53), through the `d3d12` **Gallium** driver, which Debian does package as `dri/d3d12_dri.so` | build Mesa with `-Dvulkan-drivers=microsoft-experimental`, then re-run the suite against that ICD. Watch for `libd3d12.so` being glibc-built: on Alpine the chain becomes musl-Mesa on a glibc D3D12 layer |
| DRM-native drivers (`radv`, `anv`, `radeonsi`) | there is no `/dev/dri` anywhere on this machine. WSL2 publishes no DRM render nodes, so these three cannot initialise however much silicon is present | a non-WSL Linux host |
| `glxgears` on Alpine | Alpine's `mesa-gl` is classic Mesa, not libglvnd, so there is no `libGLX_<vendor>.so.0` for the AppImage's bundled libglvnd to `dlopen`. Host packaging, not libc. Passes on a glibc host in software (E38) and on the GPU (E53) | a musl distro that ships libglvnd, or a probe that bypasses glvnd |
| aarch64 | no aarch64 hardware; this machine is x86_64 (i7-12700H). The code is arch-parameterised (`RS_LDSO`, `RS_TRIPLET`, syscall number fallbacks) and that is unverified | hardware |
| Upstreaming the sharun patch | a different repository, and section 8 forbids writing there | hand [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch) to the maintainer |
| The two live ABI hazards | `regoff_t` is 4 bytes on glibc and 8 on musl, and the `FTW_*` values are off by one, so a musl-built object reads a glibc-filled `regmatch_t[]` or classifies an `nftw` entry wrongly (E50). An offset compiled into an object is not reachable from a preload | nothing in this repository. It is a property of the two libcs, and the useful output is the list of two, which E50 keeps honest |

### 4.3 What the last session closed, so you do not redo it

The previous handover named the proprietary-driver test as the next task. It is
done, and the result was not what the task predicted, which is the part worth
carrying forward:

- **A proprietary driver is the least likely host library to need this fix.**
  NVIDIA ships `libcuda.so.1` against a `GLIBC_2.2.5` floor on purpose, so
  nothing in it can be missing from a bundled glibc 2.44. E41 works, and so do
  E41b with the feature off, E41c with no preload in the process at all, and
  E43a with upstream's shim. E42 rewrites zero objects. What those cases
  support is the *regression* claim, and REPORT.md 7.1 says so rather than
  dressing it up.
- **What the vendor stack did need was uniform version binding.** Microsoft's
  `libdxcore.so`, which `libcuda.so.1` loads to reach `/dev/dxg`, carries no
  symbol versioning at all, so as shipped the CUDA stack runs two different
  `pthread_cond_*` implementations in one process (E43a). This repo's preload
  makes them one (E43). `libd3d12.so` is the same shape in the graphics stack.
  Latent, not currently fatal, and REPORT.md 7.2 states the limit of the claim.
- **The `/etc/ld.so.cache` blindness has a symptom now, three times over**, and
  none of the three names a missing library: `CUDA_ERROR_NO_DEVICE` (E44),
  `glXCreateContext failed` (E53a), and `CUDA_ERROR_OPERATING_SYSTEM` from a
  switched host runtime, measured against the commit before the fix.
  `runtime-select.c` now derives its directories from `/etc/ld.so.conf`; sharun
  still needs the patch.
- **T1.3-T1.7 are written and passing**, and the size-versus-offset distinction
  is what made them useful rather than alarming. Two of the six struct hazards
  are measured live, two measured benign, and two (`ucontext_t`, `O_LARGEFILE`)
  argued rather than measured, because nothing in the closure crosses them.
  REPORT.md 7.4 keeps that labelled; do not let it quietly become "six checked".
- **Design R has run a real GPU workload** (E51, E52), with the switch forced,
  because auto correctly declines on a host older than the bundle.

## 5. Things that will waste your time

Each of these cost real time here. They are recorded so they cost you none.

### About symbol versions

- **An unversioned reference does NOT get the default definition.** This is the
  whole bug. `pthread_cond_init`, `realpath`, `regexec`, `glob`, `nftw`,
  `sched_getaffinity` and about 27 others have an obsolete definition glibc
  still exports, and a stripped or musl-built object lands on it silently.
  `tools/version_traps.py <libc>` prints the current list for any libc.
- **`dlsym` is not a way to find the default definition.** Measured in E27:
  `dlsym(RTLD_NEXT, "pthread_cond_init")` returns the **obsolete** definition on
  glibc 2.31 and the default one on 2.41. `dlsym(RTLD_DEFAULT, ...)` gets it
  right on both, but from inside the preload it finds the preload's own
  forwarder and recurses. Read the version name out of the ELF and use `dlvsym`.
- **Multi-versioned does not mean dangerous.** glibc 2.34's libpthread merge
  re-versioned 191 symbols in place: same address, two labels, either is
  correct. Only a name whose versions have **different `st_value`** is a trap.
  A list built from "has two versions" is 191 false positives long.

### About the test environment

- **Debian's Vulkan ICD manifest names a bare soname, not a path.**
  `foreign-dlopen` only ever intercepts absolute paths, so on Debian the whole
  feature is a no-op and every A/B looks identical and healthy. Alpine and
  Gentoo use absolute paths. Write your own manifest if you want the code path.
- **`xvfb-run -a` alone gives you an X server with no GLX.** `glxgears` then
  says `couldn't get an RGB, Double-buffered visual`, which reads exactly like a
  driver failure. You need
  `-s '-screen 0 1024x768x24 +extension GLX +extension RANDR +render'`.
- **Alpine's `mesa-gl` is not libglvnd.** There is no `libGLX_mesa.so.0` on
  Alpine, so anything bundling libglvnd has no vendor library to load. That is
  not fixable from a loader shim.
- **`ANYLINUX_LIB_FOREIGN_DLOPEN=0` is not always a clean control.** Under the
  demo AppImage's own AppRun on Alpine, with a search path that reaches `/lib`,
  the bundled glibc `ld.so` finds `libc.musl-x86_64.so.1` and loads it: the
  process ends up with **two libc families initialised** and renders anyway.
  `LD_DEBUG=libs` and `grep 'calling init:'` is what distinguishes an object
  that was *searched for* from one that was *loaded*.
- **`/bin/true` is not always an ELF binary.** On Rocky 9 it is a 51-byte shell
  script, and `ld.so` answers `file too short`, which looks exactly like a
  broken runtime and is not. `runtime-select` re-execs itself instead.
- **`/proc/self/exe` is the loader, not you**, when a program is started as
  `ld-linux.so --library-path ... ./prog`. The kernel exec'd the loader. Use
  `argv[0]`. This produced a false `SELF-TEST FAILED` on every newer host.

### About writing the tests themselves

- **`tests/vkprobe.c` used to smash its own stack on SUCCESS.** Its `struct
  Props` was ~800 bytes shorter than `VkPhysicalDeviceProperties`, so the driver
  wrote past it every time enumeration *worked*. A segfault that only happens
  when things go right is the most misleading result available. It now has the
  tail, a guard band, and a check.
- **stdout is block-buffered when it is a pipe.** A probe that crashes loses
  every line it printed, and you conclude it crashed at the start. `setvbuf(...,
  _IONBF, ...)`.
- **A verdict computed inside `$( )` increments the counter in a subshell.** The
  per-line verdicts and the totals then disagree, and the suite reports success
  while showing a MISMATCH.
- **`ls a b` fails as a whole when either glob misses.** Used as a capability
  probe, that silently skips a case on a host that could have run it.
- **Predicting `FAIL` scores a segfault as a MISMATCH.** "It did not work"
  arrives as an error code, a refusal to load, or a crash. Normalise first, then
  predict.
- **`dlerror()` is destructive, and so is anything that calls `dlsym`.** Reading
  it to log it consumes it. Less obviously: a diagnostic that probes with
  `dlsym` replaces the pending message with its own last miss, so the caller is
  told the wrong object failed. Both were live bugs here.
- **`mkstemp` rewrites its template in place.** Reusing a spent template makes
  the second loop a silent no-op. This made a fuzz test "pass" nothing.
- **glibc serves a 16 KB `malloc` from its arena; musl `mmap`s it.** So an
  absent `mmap` in an `strace` comparison proves nothing about whether the
  allocation happened. Compare on syscalls that must appear in both, such as
  `openat` of a specific file.
- **`RTLD_DEFAULT` does not see an object's own dependencies.** A "missing
  symbol" report that only consults the global scope accuses a library of
  missing 446 symbols its own `DT_NEEDED` closure supplies.
- **glibc puts version *names* in `.dynsym`** as zero-sized `SHN_ABS` entries
  (`GLIBC_2.32`, `GLIBC_ABI_DT_RELR`). They are ABI markers, not API. A
  generator that treats them as symbols emits C identifiers containing a dot.
- **Run every runtime test twice**, `ANYLINUX_LIB_FOREIGN_DLOPEN=0` then `=1`. A
  single-sided result cannot distinguish "the fix worked" from "it was already
  falling back to something else".
- **Shell scripts must be LF.** A CR becomes `$'...\r'` and yields baffling
  "not found" errors. `.gitattributes` enforces it and `run.ps1` verifies rather
  than trusts.
- **`podman machine ssh` drops a file called `NUL` in your working directory.**
  It writes its known_hosts entry to the Windows null device, and from Git Bash
  that resolves to a real file. `git add` then fails the whole commit with
  `short read while indexing NUL`, which reads like repository corruption and is
  a stray 99-byte SSH host key. `rm -f ./NUL` clears it. Check for it after any
  `podman machine ssh`.
- **PowerShell corrupts a string piped to a native process.** Mount scripts into
  the container instead. A PowerShell function that leaves native output on the
  success stream returns an array, not your exit code.
- **`& $exe @array 'x' 'a','b','c'` passes the comma list as ONE argument.**
  PowerShell parses `a, b, c` in a command position as an array expression and
  stringifies it. The GPU-capability probe did this and reported "no GPU" on a
  machine that has two, and the whole suite then SKIPPED nine cases while
  looking perfectly healthy. Build one flat array and splat it once.

### About reaching the GPU

- **A missing library directory does not announce itself as a missing library.**
  This is the single most misleading failure here. `libcuda.so.1` loads,
  resolves every entry point, and then `cuInit` returns 100
  `CUDA_ERROR_NO_DEVICE` -- because it `dlopen`ed `libdxcore.so` by bare soname
  and `ld.so` could not find it. Mesa's `d3d12_dri.so` does the same with
  `libd3d12.so` and the user sees `Error: glXCreateContext failed`. Both read as
  hardware faults. `LD_DEBUG=libs` plus `grep 'find library='` is the one
  command that distinguishes them.
- **`sharun` re-execs and replaces your `--library-path`.** Running
  `$APPDIR/bin/<prog>` under `ld.so --library-path ...` does not do what it
  looks like: everything in `bin/` is sharun, which re-execs the real binary
  with a path it assembles itself. The trace shows two pids and only the second
  one matters. `SHARUN_FALLBACK_LIBRARY_PATH` is the supported way to add to it
  without editing anything.
- **`MESA_D3D12_DEFAULT_ADAPTER_NAME` is the only adapter selector.** Without it
  a machine with two GPUs quietly gives you the integrated one. `GALLIUM_DRIVER=d3d12`
  is what selects the driver; `MESA_LOADER_DRIVER_OVERRIDE=d3d12` alone is not
  enough and falls back to llvmpipe without saying so.
- **`glxgears -info` prints the whole `GL_EXTENSIONS` string**, which is several
  kilobytes on one line and will bury whatever you were reading. Grep for
  `GL_RENDERER` with `-m1`.

### About measuring what the loader did

- **`LD_DEBUG=bindings` prints the version a reference ASKED for, not the one it
  got.** For the version-binding trap that is exactly the wrong half: an
  unversioned reference asks for nothing, and the line is silent about which of
  the two definitions it landed on. `tests/bindprobe.c` reads the slot instead.
- **A lazily-bound GOT slot still holds the PLT resolver stub.** Reading it
  without `LD_BIND_NOW=1` measures the stub, not the definition. Eager binding
  changes *when* the choice is made, never which definition is chosen.
- **A `d_ptr` in a mapped `PT_DYNAMIC` may be absolute or link-time**, depending
  on the port, and dereferencing the wrong guess is a segfault. `dladdr` decides
  it safely: it searches the loaded objects for an address and never
  dereferences it, so the candidate that lands inside that object is the right
  one.
- **Taking `&func` in an EXECUTABLE gives you its own PLT entry**, not libc's
  address, so comparing that against a shared object's `&func` can differ for a
  linking reason rather than a libc one. Compare the FILE each address lands in.

### About what a control is allowed to do

- **Some controls do not flip, and that is the result.** The CUDA cases were
  written expecting the feature-off control to fail. It passes, because NVIDIA
  ships against a `GLIBC_2.2.5` floor and nothing in the blob can be missing.
  The correct response was to state that as the finding (REPORT.md 7.1), not to
  keep forcing the test until it broke. A control that has to be engineered into
  failing is not evidence of anything.
- **`$?` after a pipeline is the LAST command's status.** `probe | sed` then
  `echo $?` reports `sed`. Same family as the subshell-counter bug above, and it
  made three scratch runs look like they all succeeded.

## 6. Diagnostic ladder

When something fails, report **which rung caught it**.

1. **Host driver sane?** `vulkaninfo --summary` natively. If this fails, stop.
2. **Display, not libc?** Re-run under
   `xvfb-run -a -s '-screen 0 1024x768x24 +extension GLX +render'`. WSI errors
   are not this project's bug.
3. **Feature on?** `ANYLINUX_LIB_DEBUG=1`. No ` [foreign-dlopen.so] >> ` lines
   means the marker, the env switch, or the `.preload` order is wrong. No
   `foreign: rewriting` line and no `needs no rewrite` line means the path was
   not absolute and nothing was ever intercepted.
4. **Which object, which symbol?** `ANYLINUX_LIB_FOREIGN_DRYRUN=1`, or
   `LD_DEBUG=libs,bindings`. An `undefined symbol: X` names the next candidate.
5. **Is the library findable at all?** The failure most likely to send you the
   wrong way. A driver that `dlopen`s the rest of its own stack by BARE SONAME
   -- `libdxcore.so` from CUDA, `libd3d12.so` from Mesa's d3d12 -- is not
   intercepted, so `ld.so` searches `--library-path` and nothing else, because
   the cache is inhibited. What you see when it misses is
   `CUDA_ERROR_NO_DEVICE` or `glXCreateContext failed`, neither of which
   mentions a library. `LD_DEBUG=libs LD_DEBUG_OUTPUT=/tmp/ld` then
   `grep 'find library=' /tmp/ld.*` is what settles it in one command.
7. **Is `X` really absent?** Check with `elfsym.py` against the **bundled**
   `libc.so.6`. If present, this is a scope or visibility problem, not
   availability, and needs a different fix.
6. **Is `X` merely re-homed?** musl folds `libm`, `libpthread`, `libdl`, `librt`
   and the resolver into `libc`; glibc splits them out, and glibc 2.34 merged its
   own split libraries back in. Load the library instead of shimming the symbol.
   `fgn_global_scope_libs[]` in `src/foreign-dlopen.c` is the list.
8. **Did the rewrite corrupt the image?** Re-parse
   `$XDG_RUNTIME_DIR/.anylinux-fgn-*.so` with `elfsym.py`.
9. **Did it need rewriting at all?** `ANYLINUX_LIB_DEBUG=1` prints
   `provider <file> -> ...` for each `DT_VERNEED` file and says which version it
   could not vouch for. On a host older than the bundle the answer should be
   "nothing was rewritten" (E39).
10. **Loads, but the wrong definition?** `ANYLINUX_LIB_FOREIGN_NOSTRIP=1` keeps
    the version tags while still loading from the private copy, which separates
    "the rewrite broke it" from "the path broke it". If NOSTRIP fixes it, you
    are looking at a version-binding trap: run `tools/version_traps.py` against
    the libc and check the symbol is covered by `version-compat.c`. To read the
    answer rather than infer it, `LD_BIND_NOW=1 tests/bindprobe <lib> <symbol>`
    walks each loaded object's relocations and names the file and version behind
    the address the loader actually stored. `LD_DEBUG=bindings` cannot do this:
    it prints the version a reference ASKED for, and for this trap the whole
    point is that the reference asks for nothing.
11. **Loads and still misbehaves?** ABI territory. `tests/abi-host.c` against a
    guest built by the other libc covers the allocator, `errno`, `FILE*`,
    mutexes, condition variables and the divergent structs (E47-E50); if a
    crossing there is red, that is your answer. If they all pass, the remaining
    shapes are the two live hazards in 4.2 -- a musl-built object reading back a
    glibc-filled struct at its own stride, or comparing against its own `FTW_*`.
    Otherwise bisect with gdb: breakpoint the failing library call, `finish`,
    read the return value, and `info symbol $pc` at entry — that last step is
    what found the version trap.

## 7. Rules that must not be broken

- **Never modify a host file.** Every write goes under `$XDG_RUNTIME_DIR` or
  `$TMPDIR`. `tests/invariants.c` and the checksum comparison in REPORT.md guard
  this.
- **Bundled libraries always beat host libraries**, for everything except the
  libc runtime set when Design R deliberately switches it.
- **Exactly one libc family per process.** Never `dlopen` a second libc. E8 and
  E9 measure why for glibc. musl's libc *can* be mapped by a glibc `ld.so`,
  which is worse, not better: it succeeds quietly. See section 5.
- **Never strip symbol versions partially.** `DT_VERSYM`, `DT_VERNEED`,
  `DT_VERDEF` and `DT_VERDEFNUM` go together. A verdef without its versym
  segfaults `ld.so`.
- **Strip only when the object actually needs it.** A rewritten object is a
  private copy loaded from a path the application did not ask for, and the
  Vulkan loader says so. On a host that can satisfy every requirement, the right
  number of rewrites is zero.
- **Never touch `ld-linux*`, `libc.so.*`, `ld-musl*`.** `fgn_never_touch[]`
  exists for this. `ld-linux` has no `SONAME`, so `RTLD_NOLOAD` cannot catch it.
- **Do not add library searching to `foreign-dlopen.c`.** Finding libraries is
  `ld.so`'s job, driven by `--library-path`. Two search implementations would
  diverge and the C one would be the buggy one.
  **Assembling that path is a different job, and it belongs to whatever launches
  the process.** There are two such launchers and they need the same fix
  independently: sharun assembles the path for the bundled runtime, which is
  what [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch)
  is for, and `rs_library_path()` in `src/runtime-select.c` assembles it for the
  switched one, which is done. Adding a directory to a path is not searching it;
  the shim still never opens a library it was not handed by name.
- **Anything appended to a library path goes at the END.** Bundled directories
  first, then the host runtime dir, then the conventional host dirs, then
  whatever `/etc/ld.so.conf` names. Inserting anywhere else hands a host library
  a win it should not have, which is the defect two corrections in README.md are
  already about.
- **Regenerate the shim when the bundled glibc changes.**
  `make shim FLOOR=... TARGET=... MUSL=...`. A stale shim interposes over symbols
  libc now provides. `src/forward-shim-manifest.json` records the floor it
  targets. `MUSL` has a default in the Makefile because omitting it drops 33 of
  35 definitions and silently disarms the musl bridge.
- **Re-audit the version traps when the bundled glibc changes.** `make traps`.
  The set only grows with newer glibc.
- **A test you cannot run is SKIPPED with the specific missing capability
  named.** Never silently omitted, never guessed.

## 8. Repository permissions

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
