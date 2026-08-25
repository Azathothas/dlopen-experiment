# Cross-libc `dlopen`

Load a host shared library that was built against a **different libc** (a newer
glibc, or musl) without patching host files, without a second libc in the
process, and without symbol collisions.

The target is [Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages)'
`foreign-dlopen.c`, which lets an AppImage use the host's GPU drivers instead
of bundling 100-200 MB of Mesa and LLVM.

```powershell
.\experiments\run.ps1        # ~3 min, needs podman or docker. 31/31 predictions hold.
.\experiments\appimage.ps1   # ~15 min, the end-to-end proof on a real AppImage
```

## The problem

| # | Process libc | Host library libc | Status |
|---|---|---|---|
| 1 | older glibc | newer glibc | **Solved.** Two mechanisms, see below |
| 2 | glibc | musl | **Solved.** Every library in the host's `/usr/lib` loads, and `vkcube` renders on Alpine using Alpine's own musl-built Mesa |
| 3 | musl | glibc | **Out of scope.** Use [pg83/solo](https://github.com/pg83/solo) |
| 4 | musl | musl | Works natively |

## The design

Split the gap by whether it is **enumerable**.

Symbols that exist today and the bundle lacks are enumerable, so a **generated
shim** covers them. It is generated from glibc's and musl's own symbol tables
against a declared floor, regenerated when the bundled glibc changes, with a
per-symbol classification recorded in a manifest and a loud abort naming
anything it cannot implement.

Symbols invented **after** the AppImage ships are not enumerable and never will
be. No shim can cover them. For those the answer is to stop predicting and use
the host's own libc, selected at `execve` time when it is newer and provably
self-consistent.

Neither half is sufficient alone. On a musl host only the first is available,
because a glibc-linked process cannot exec under musl's `ld.so`.

## Results

| Goal | Result | Proof |
|---|---|---|
| Evidence table still holds | **31/31**, up from 14/14 | [`experiments/run.ps1`](experiments/run.ps1), cases in [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| AppImage end to end, on a real host driver | **31/31 glibc, 25/25 musl** (six named skips) | [`experiments/appimage.ps1`](experiments/appimage.ps1), cases in [`40-appimage.sh`](experiments/40-appimage.sh) |
| A **closed-source** host driver, on real silicon | **4096 bytes round-tripped through an RTX 3050 Ti and verified**, from the AppImage's bundled glibc on Alpine | E41, [`tests/cudaprobe.c`](tests/cudaprobe.c), [REPORT.md 7.1](REPORT.md) |
| Rendering on an actual GPU | **`GL_RENDERER = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)`, 101-121 FPS**, through the AppImage with no file changed | E53, [REPORT.md 7.5](REPORT.md) |
| Cross-libc ABI microtests, T1.3-T1.7 | **26 crossings hold**; of six struct hazards, **two measured live, two measured benign, two argued** | E47-E50, [`tests/abi-host.c`](tests/abi-host.c), [REPORT.md 7.4](REPORT.md) |
| Design R with a driver on the end | **1 Vulkan device, and the CUDA round trip**, under the switched host runtime | E51, E52, [REPORT.md 7.6](REPORT.md) |
| Host driver built against newer glibc loads into older bundled glibc | **Achieved** | E5 (shim path), E12 (host runtime, no shim), [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| Host runtime selected correctly per distro | **8/8 distros** | [`src/runtime-select.c`](src/runtime-select.c), E17-E21, [REPORT.md](REPORT.md#4-design-r-host-runtime-selection) |
| Mixed runtime set refused (the configuration that segfaults) | **Refused**, with an accept-control | E20 refuses, E21 accepts, [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| musl host libraries loading into a glibc process | **2/177 to 177/177**, zero regressions | [`tests/corpus.c`](tests/corpus.c), E33/E34 |
| Host Vulkan ICD loads and is callable | **`vkCreateInstance` returns `VK_SUCCESS`** | [`tests/icd-harness.c`](tests/icd-harness.c), [`tests/vkprobe.c`](tests/vkprobe.c) |
| `vkcube` renders on Alpine using Alpine's Mesa | **`Selected GPU 0: llvmpipe (LLVM 20.1.8)`** | as shipped it reports zero devices (E37a), with this it renders (E37). [REPORT.md 6.2](REPORT.md) |
| It keeps rendering | **100 load/unload cycles and 60 s continuous, everything flat** | [`tests/soak.c`](tests/soak.c), E36 |
| Turning the feature on cannot break a host that already worked | **0 objects rewritten where none needs it**, down from 6 | E39, [REPORT.md 3.4](REPORT.md) |
| No host file modified | **Identical sha256** before and after | [`tests/invariants.c`](tests/invariants.c), REPORT.md T4.3 |
| Bundled libraries beat host libraries | **All collision-surface sonames resolve under `$APPDIR`** | [`tests/invariants.c`](tests/invariants.c) |
| Exactly one libc family in the process | **Yes, on both hosts** | [`tests/invariants.c`](tests/invariants.c), E35 |
| Generated shim is correct, not just compilable | **42 behavioural checks on real glibc 2.31** | [`tests/shim-selftest.c`](tests/shim-selftest.c), case E16 |
| ELF rewriting is safe on hostile input | **Truncations and bit flips refused or bounded** | [`tests/elf-selftest.c`](tests/elf-selftest.c), case E14 |
| Library-path completeness fix for sharun | **Patch written, compiled, run on 3 distros** | [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch) |
| Verdict on the two rejected designs | **Written, with evidence** | [`analysis/rejected-designs.md`](analysis/rejected-designs.md) |

## The blocker, and why it looked like an ABI problem

`vkcube` reported `vkEnumeratePhysicalDevices reported zero accessible devices`
on Alpine, and that was blamed on glibc-vs-musl ABI differences for as long as
it went unfixed. It is not that. Removing an object's symbol version
requirements is by itself enough to break it, on **one** libc, with no musl and
no Vulkan anywhere in the process:

```
versions stripped   pthread_cond_init(&c, &attr)  ->  22 (EINVAL)     E22
versions intact     pthread_cond_init(&c, &attr)  ->   0              E22b
```

glibc still exports `pthread_cond_init@GLIBC_2.2.5` for binaries from before
the 2003 condition-variable ABI change, and its whole body is
`if (cond_attr != NULL) return EINVAL;`. **An unversioned reference does not get
the default definition.** A stripped object has only unversioned references, and
so does every musl-built object, which never had any. Mesa's
`u_cnd_monotonic_init()` always passes an attribute, so WSI init fails,
`lvp_init_wsi()` reports `VK_ERROR_OUT_OF_HOST_MEMORY`, and the loader reports
no devices.

[`src/version-compat.c`](src/version-compat.c) defines the trapped names in the
preload, ahead of libc in the global scope, and forwards each to the default
definition. [`tools/version_traps.py`](tools/version_traps.py) computes which
names those are -- a name defined at two versions whose `st_value` differs -- and
`make traps` fails the build if a libc has one that is neither forwarded nor
explicitly declined. Full chain, one measurement per link, in
[REPORT.md 6.2](REPORT.md).

## Six defects found by measurement

Each was found by running something, not by reading the code.

**musl folds `libm` into `libc`; glibc splits it out.** A musl-built object
imports `fmod` and `fesetround` with no `DT_NEEDED` on anything, because on musl
its libc edge covered them, and that edge is exactly what `foreign-dlopen.c`
drops. `libm.so.6` was not in the process at all. This, not glibc's own 2.34
consolidation, is what blocked the musl case.

**Bundled libraries were losing to host libraries for musl guests.** The
dependency probe is skipped for them, correctly, because loading the host copy
unstripped would drag musl libc in. But the skip went straight to a search that
only covers the active load stack, so a bundled soname could never win. The
AppDir bundles `libstdc++.so.6.0.36` and the host's `.6.0.33` was loading
alongside it. Two libstdc++ and two unwinders in one process.

**`dlerror()` was being consumed.** The fallback path reads it unconditionally
and only prints it under debug. `dlerror()` is destructive, so with debug off
the caller's own `dlerror()` returned `NULL` and the error message the comment
promises to surface reached nobody.

**Everything was rewritten, whether or not it needed to be.** The provider scan
learned only libc's own version names, so every `GLIBCXX_*`, `CXXABI_*` and
`LLVM_*` requirement in a Mesa closure looked unsatisfiable. On a host whose
glibc is *older* than the bundled one, where nothing can be missing, it rewrote
6 objects anyway and broke a driver that worked. Asking per **file** instead --
a `DT_VERNEED` names the file its versions are wanted from -- takes that to 0.

**The failure report accused the wrong thing.** A `DT_NEEDED` that cannot be
opened makes every symbol it would have provided look unresolved, and the
report ended with "most likely the bundled glibc predates them" under 258 LLVM
entry points that no libc has ever exported.

**The failure report was itself destructive.** It probes with `dlsym`, and every
probe that misses replaces the pending `dlerror()`, so the caller was told this
preload had an undefined symbol rather than being told which library failed to
open. Same class as the defect above it, from the other side.

## Two corrections to the original design

**A flat `--library-path "$HOST_LIBDIR:$APPDIR/lib"` breaks the bundling
guarantee.** It hands the host `libstdc++`, `libX11` and every other soname the
win, not just libc. Replaced with a symlink farm under `$XDG_RUNTIME_DIR` that
holds the runtime set and nothing else, placed ahead of `$APPDIR/lib`.

**A `DT_VERNEED` completeness check cannot detect a mixed runtime set.** glibc
never retires a version name, so an old `libdl.so.2` asks libc only for
`GLIBC_2.2.5` and every later glibc still defines it. Version names alone
declare the mixed set healthy; it segfaults. What discriminates is the
`GLIBC_PRIVATE` symbol surface, which is not stable: old `libdl.so.2` imports
`_dl_sym`, `_dl_addr` and `__libc_dlopen_mode`, none of which glibc 2.41
exports. The implemented check is symbol-level, and is verified empirically by
re-execing under the candidate runtime before committing to it.

## One bug, found three times, that belongs somewhere else

The `dlopen` path is not what breaks a host driver most often here. Library
*discovery* is, and it is the same defect every time.

Anylinux patches `ld-linux.so` to skip `/etc/ld.so.cache` (E13b), so
`--library-path` becomes the only discovery mechanism there is, and whatever
assembles it decides what exists. Every assembler in the chain uses a hardcoded
list. WSL, meanwhile, publishes its GPU userspace by writing
`/etc/ld.so.conf.d/ld.wsl.conf` and nothing else. The intersection is empty, and
a driver that `dlopen`s the rest of its own stack by bare soname -- which
`foreign-dlopen` deliberately never intercepts -- cannot find it:

| Where | Symptom | Reads like |
|---|---|---|
| CUDA (E44) | `cuInit -> 100 CUDA_ERROR_NO_DEVICE` | there is no GPU |
| OpenGL (E53a) | `Error: glXCreateContext failed` | the display or the driver is broken |
| Design R (measured against the commit before the fix) | `cuInit -> 304 CUDA_ERROR_OPERATING_SYSTEM` | the operating system is at fault |

None of the three names a missing library. All three have the same fix -- read
`/etc/ld.so.conf`, which is plain text, so it gets the benefit of the cache
without touching the binary cache whose parsing is why the cache was inhibited --
and only one of the two assemblers has it today.
[`src/runtime-select.c`](src/runtime-select.c) now does that for the runtime it
switches to; [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch)
does it for the bundled one and still needs upstreaming by hand.

## What is not done

- **`glxgears` cannot run on Alpine**, for a reason that is not libc: Alpine's
  `mesa-gl` is classic Mesa, not libglvnd, so there is no
  `libGLX_<vendor>.so.0` for the AppImage's bundled libglvnd to `dlopen`. It
  passes on a glibc host with libglvnd, in software (E38) and on the GPU (E53).
  No loader shim can supply a file the distribution does not ship.
- **Two of the glibc-vs-musl struct hazards are real and cannot be fixed here.**
  Measured, not assumed: every field the probe checks sits at the same offset in
  both, so `rusage`, `sched_param` and `stat` cross harmlessly. What does not is
  a musl-built object reading back a struct glibc filled at its own stride --
  `regexec` returns a match ending at byte 7 and the caller reads 12884901888 --
  and the `FTW_*` constants, which are off by one, so an `nftw` walk over two
  directories counts none (E50). An offset compiled into an object is not
  something a preload can reach. Two of the six, `ucontext_t` and `O_LARGEFILE`,
  are argued rather than measured: nothing here crosses them, so there is no
  crossing to test. REPORT.md 7.4 keeps those two labelled.
- **Vulkan has never run on hardware.** OpenGL now has, through Mesa's d3d12
  Gallium driver, which reaches `/dev/dxg` without a DRM render node. Its Vulkan
  counterpart (`dzn`) is not packaged by Debian, so `vkcube` and every ICD result
  here is still lavapipe. `radv`, `anv` and `radeonsi` remain out for a reason
  that is not fixable from userspace: WSL2 publishes no `/dev/dri` at all.
- **No aarch64.** This machine is x86_64. The code is arch-parameterised
  (`RS_LDSO`, `RS_TRIPLET`, syscall-number fallbacks) and that is unverified.
- **The sharun patch is not upstreamed.** It is a different repository; the
  patch compiles and has been run on three distros, and handing it over is a
  human step.

Full per-test results, including every skipped test and the specific missing
capability, are in [REPORT.md](REPORT.md).

## Out of scope

Case 3, a glibc-built library loading into a **musl** process, is not addressed.
The packaging always bundles glibc deliberately, because musl would lose the
proprietary NVIDIA driver. [pg83/solo](https://github.com/pg83/solo) solves
case 3 properly with its own ELF loader and a ~6000-line glibc-to-musl ABI
bridge.

## Layout

```
README.md                          this file
REPORT.md                          full measured results, per test
CONTINUE.md                        state and next steps for a fresh start
analysis/                          Phase A measurements, and the C/D verdict

src/foreign-dlopen.c               the patched loader
src/version-compat.c               unversioned forwarders for the version traps
src/fgn-symver.h                   the one thing version-compat.c needs from it
src/runtime-select.c               host-runtime selection at exec time
src/forward-shim.c                 GENERATED, do not edit
src/forward-shim-manifest.json     per-symbol classification and the floor
src/Makefile                       build; `make shim` regenerates, `make traps` audits

tools/libc_inventory.py            symbol inventory and per-release diff
tools/gen_forward_shim.py          the shim generator
tools/version_traps.py             which symbols an unversioned reference gets wrong
tools/trap_users.py                and which of them a given object actually steps on
inventories/*.json                 inventories the generator reads

tests/elf-selftest.c               ELF rewriting, against the real implementation
tests/shim-selftest.c              42 behavioural checks on the generated shim
tests/corpus.c                     load every .so in a directory, before vs after
tests/icd-harness.c                foreign-load a real Vulkan ICD
tests/vkprobe.c                    bundled loader plus host ICD, no window system
tests/invariants.c                 one libc family, bundled wins
tests/allocprobe.c                 interpose the allocator; every counter has a total
tests/verprobe.c                   the version-binding trap, as a loadable probe
tests/vertrap.c                    the three properties version-compat.c rests on
tests/soak.c                       N load/unload cycles, with RSS, fds and copies
tests/cudaprobe.c                  a closed-source vendor driver, and a GPU round trip
tests/bindprobe.c                  which DEFINITION each loaded object actually bound
tests/abi-abi.h                    the view both sides of the ABI boundary fill
tests/abi-guest.c                  the host driver's side; built by glibc and by musl
tests/abi-host.c                   the crossings: allocator, errno, FILE*, mutex, structs

experiments/run.ps1                one command, the whole evidence table
experiments/1x-3x.sh               the three container stages behind run.ps1
experiments/appimage.ps1           the end-to-end proof on a real AppImage
experiments/4x-*.sh                its stages: extract, build on the floor, the musl
                                   ABI guest, and the two hosts
patches/                           the sharun fix, to be upstreamed by hand
scripts/wsl-ephemeral.ps1          throwaway WSL2 distros from any OCI image
elfsym.py, gap.py                  dependency-free ELF reader and gap driver
```

## Evidence table

Run by `experiments/run.ps1`. E1-E13 measure the problem, E14-E29 measure the
fix. E30-E53 are the separate AppImage suite, `experiments/appimage.ps1`. Every
case states a prediction and the harness reports MATCH or MISMATCH; a MISMATCH
is a finding, not a harness bug, and a case that cannot run on this machine is
SKIPPED with the missing capability named rather than dropped.

| ID | Experiment | Result |
|---|---|---|
| E1 | musl lib, musl `DT_NEEDED` dropped, into glibc | FAILS: `undefined symbol: atexit` |
| E2 | same, with a 3-line `atexit` shim | WORKS |
| E3 | glibc-2.41 lib into glibc 2.31 | FAILS: `version 'GLIBC_2.38' not found` |
| E4 | same, version-stripped | FAILS: `undefined symbol: arc4random` |
| E5 | same, stripped plus compat shim | WORKS |
| E6 | `pthread_create@GLIBC_2.34`, stripped, no `libpthread` loaded | FAILS |
| E7 | same, with `libpthread.so.0` in the process | WORKS |
| E8 | `dlopen` newer `libc.so.6` under older `ld.so` | FAILS: version lock |
| E9 | same via `dlmopen(LM_ID_NEWLM)` | FAILS: identical error |
| E10 | exec-time switch to the host's whole runtime | WORKS |
| E11 | exec-time switch with a mixed runtime | SIGSEGV |
| E12 | E3/E4's library under the host's complete runtime, no shim | WORKS |
| E13a | lib in `/usr/local/lib`, path omits it, cache allowed | WORKS via cache |
| E13b | same, `--inhibit-cache` (anylinux's patched loader) | FAILS |
| E13c | same, cache inhibited, directory added to the path | WORKS |
| E14 | ELF rewriter self-test against the real implementation | PASSES |
| E15 | generated shim compiles `-Wall -Werror` on its target floor | PASSES |
| E16 | 42 behavioural checks on the generated shim, glibc 2.31 | PASSES |
| E17 | host **older** than bundled, keep bundled | PASSES |
| E18 | host **equal** to bundled, keep bundled | PASSES |
| E19 | `ANYLINUX_RUNTIME=bundled` override honoured | PASSES |
| E20 | mixed runtime set **refused** | PASSES |
| E21 | control for E20: the same glibc unmixed is **accepted** | PASSES |
| E22 | version-stripped object: `pthread_cond_init` returns `EINVAL` | THE BUG |
| E22b | control: the same object unstripped returns 0 | PASSES |
| E23 | the same stripped object, preload merely present | RETURNS 0 |
| E24 | the obsolete definition rejects the attribute Mesa passes | PASSES |
| E25 | the `memcpy` exclusion is justified, 4096 combinations | PASSES |
| E26 | audit: no glibc trap is unforwarded and undeclined | PASSES |
| E27 | which resolution primitive returns the default definition | `dlsym(RTLD_NEXT)` does NOT, on glibc 2.31 |
| E28 | the report names the dependency that failed to open | PASSES |
| E29 | and the caller still gets ld.so's message | PASSES |
| E30 | AppImage as shipped, host ICD | no devices |
| E31 | control: feature off | host driver unusable |
| E32 | this repo, feature on | **1 device** |
| E39 | objects rewritten where none needs rewriting | **0**, down from 6 |
| E33/E34 | host `/usr/lib` loadable, off vs on | 2/177 -> 177/177 on musl |
| E35 | exactly one libc family mapped | PASSES |
| E36 | 100 load/unload cycles | no growth |
| E37a/E37 | `vkcube`, as shipped vs this repo | zero devices -> **renders** |
| E40 | **replace one file in the AppDir, set no variables at all, run it** | as shipped: `Do you have a compatible Vulkan ICD installed?` / with this: **`Selected GPU 0: llvmpipe`** |
| E38 | `glxgears` | PASSES on glibc, SKIPPED on Alpine with the reason |

E41-E53 are this repository's last pass: a **closed-source** driver, the
cross-libc ABI, and an actual GPU. Each is SKIPPED by name on a machine that
lacks the capability, so the suite still passes without one. E41-E50 run on both
hosts; E51-E53 need a host glibc runtime and a hardware GL driver, so on Alpine
they are skipped with those reasons.

| ID | Experiment | Result |
|---|---|---|
| E41 | foreign-`dlopen` NVIDIA's `libcuda.so.1`, then push 4 KiB to the GPU and read it back | **verified round trip** on an RTX 3050 Ti |
| E41b | its control, feature off | **also passes** -- a `GLIBC_2.2.5` vendor floor never needed the fix |
| E41c | the same with **no preload in the process at all** | also passes; the strongest form of that finding |
| E42 | objects rewritten to get there | **0**, from a real vendor binary |
| E43a | which `pthread_cond_*` definition each object bound, AppImage as shipped | **MIXED**: `libdxcore.so` gets `@GLIBC_2.2.5`, `libcuda.so.1.1` gets `@GLIBC_2.3.2`, 5 of 6 symbols |
| E43 | the same with this repo | **UNIFORM**, 0 of 6 |
| E44 | the vendor driver with only the AppImage's own library path | `cuInit -> 100 CUDA_ERROR_NO_DEVICE` |
| E45 | the same, plus the directories `/etc/ld.so.conf` names | round trip completes |
| E46 | the vendor's own `nvidia-smi` under the bundled loader, on Alpine | names the GPU |
| E46a | its control: the same binary with no AppImage runtime, on musl | **cannot execute at all** |
| E47 | ABI crossings, same-libc control | 27 checks pass |
| E48 | musl guest, feature off | `libc.musl-x86_64.so.1: cannot open shared object file` |
| E49 | musl guest, feature on: allocator, `errno`, `FILE*`, mutex, condvar | 26 checks pass |
| E50 | guest reading back a struct glibc filled | **2 live hazards** named, every field offset agrees |
| E51 | Design R switches runtime, then enumerates a Vulkan device | 1 device, no shim involved |
| E52 | Design R, then the CUDA round trip | passes on the GPU |
| E53a | the AppImage on the host's **hardware** GL driver, as it stands | `Error: glXCreateContext failed` |
| E53 | the same, plus the directories `/etc/ld.so.conf` names | **`GL_RENDERER = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)`** |
| E53b | its control, feature off | also renders; the host GL stack is older glibc, so the shim has nothing to do |

## Building

Build on the **oldest** glibc you intend to support, not the newest. The
products only need symbols from that floor, so an old build runs everywhere and
a new one does not.

```bash
cd src && make
```

Regenerating the shim for a different bundled glibc. `--musl` is not optional:
without it the generator emits 2 definitions instead of 35 and silently disarms
the entire musl bridge, so `MUSL` is a default in the Makefile rather than
something the caller has to remember.

```bash
make shim FLOOR=../inventories/glibc-2.31.json TARGET=../inventories/glibc-2.44.json
```

Auditing the version traps against a libc. Run it against the **newest** glibc
you care about; the set only grows.

```bash
make traps AUDIT_LIBC=/lib/x86_64-linux-gnu/libc.so.6
```

## Runtime switches

| Variable | Effect |
|---|---|
| `ANYLINUX_LIB_FOREIGN_DLOPEN=1` / `=0` | force the feature on or off; `=0` is the A/B control |
| `ANYLINUX_RUNTIME=host\|bundled\|auto` | force or auto-select the libc runtime |
| `ANYLINUX_LIB_DEBUG=1` | trace to stderr, prefixed ` [foreign-dlopen.so] >> ` |
| `ANYLINUX_LIB_FOREIGN_DRYRUN=1` | report what would be rewritten and what would not resolve, load nothing |
| `ANYLINUX_LIB_FOREIGN_NORENAME=1` | disable symbol renaming, for bisecting a misbehaving driver |
| `ANYLINUX_LIB_FOREIGN_NOSTRIP=1` | keep the version tags but still load from the private copy, which separates "the rewrite broke it" from "the path broke it" in one A/B |

## Prior art

- [pg83/solo](https://github.com/pg83/solo), the reverse direction: static musl
  loading host glibc drivers, with its own ELF loader and a glibc-to-musl ABI
  bridge. Production grade, CI across Alpine/Fedora/NixOS/Termux plus a
  2100-object corpus test per commit.
- [graphitemaster/detour](https://github.com/graphitemaster/detour) and
  [pfalcon/foreign-dlopen](https://github.com/pfalcon/foreign-dlopen), driving a
  foreign `ld.so` in-process. Requires a libc-free process, so not applicable to
  an AppImage.
- [Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages), the
  implementation this work targets.
- [VHSgunzo/sharun](https://github.com/VHSgunzo/sharun), the launcher that
  assembles `--library-path`, where the discovery fix belongs.
- [QaidVoid/onelf](https://github.com/QaidVoid/onelf), whose `bundle/gpu.rs`
  enumerates DRI/GBM/Vulkan ICD search paths.
