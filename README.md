# Cross-libc `dlopen`

Make an AppImage use the **host's** GPU drivers instead of bundling 100-200 MB
of Mesa and LLVM -- without patching host files, without a second libc in the
process, and without symbol collisions.

Two things stand in the way, and only the first is about libc.

1. The host's driver was built against a **different libc**: a newer glibc, or
   musl. [`src/foreign-dlopen.c`](src/foreign-dlopen.c) carries it across.
2. The host has the **capability** but ships nothing in the shape the bundled
   loader looks for -- which is the whole of OpenGL on a musl distro.
   [`src/gl-fwd.c`](src/gl-fwd.c) replaces the bundled loader instead.

The target is [Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages)'
`foreign-dlopen.c`.

```powershell
.\experiments\run.ps1        # ~3 min, needs podman or docker. 36/36 predictions hold.
.\experiments\appimage.ps1   # ~15 min, the end-to-end proof on a real AppImage
```

## The problem

The libc half of it, which is what the name of this repository is about:

| # | Process libc | Host library libc | Status |
|---|---|---|---|
| 1 | older glibc | newer glibc | **Solved.** Two mechanisms, see below |
| 2 | glibc | musl | **Solved.** Every library in the host's `/usr/lib` loads, and `vkcube`, `glxgears` and an EGL context all work on Alpine using Alpine's own musl-built Mesa |
| 3 | musl | glibc | **Out of scope.** Use [pg83/solo](https://github.com/pg83/solo) |
| 4 | musl | musl | Works natively |

And the half that is not about libc at all: a bundled **loader** whose plugin
the host does not ship in that shape. That is OpenGL and EGL on every classic-
Mesa host, and no row of the table above can express it, which is most of why
it went unnoticed for a session.

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

That split covers the libc gap and only the libc gap. The second gap -- a
bundled **dispatcher** whose plugin the host does not ship in that shape -- is
not a symbol problem at all and is not fixable by any amount of bridging;
"the gap that is not about libc at all" below is the design for it.

## Results

| Goal | Result | Proof |
|---|---|---|
| Evidence table still holds | **36/36**, up from 14/14 | [`experiments/run.ps1`](experiments/run.ps1), cases in [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| AppImage end to end, on a real host driver | **40/40 glibc, 35/35 musl** (five named skips) | [`experiments/appimage.ps1`](experiments/appimage.ps1), cases in [`40-appimage.sh`](experiments/40-appimage.sh) |
| **OpenGL on a host whose Mesa has no glvnd vendor library** | **`glxgears` renders on Alpine**, and a cleared pixel comes back `64 128 191 255` through 3470 forwarded entry points | E61-E64, [`src/gl-fwd.c`](src/gl-fwd.c), [`tests/glprobe.c`](tests/glprobe.c), [REPORT.md 9](REPORT.md) |
| **EGL on the same host** | `EGL_NO_DISPLAY` -> a working surfaceless context | E65, E66, [`tests/eglprobe.c`](tests/eglprobe.c) |
| Every bundled loader classified rather than assumed | **8 objects import `dlopen`, 0 unclassified** | E59, [`tools/plugin_boundaries.py`](tools/plugin_boundaries.py) |
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
| Library-path completeness fix for sharun | **Upstreamed**, with three residual gaps named | [`Anylinux-sharun@54208d2`](https://github.com/pkgforge-dev/Anylinux-sharun/commit/54208d2bc7d4c919ba46a6c234f6af7f8426b537), [`analysis/ground-truth.md`](analysis/ground-truth.md) |
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

None of the three names a missing library. All three have the same fix -- ask
the distribution which directories it considers library directories, instead of
guessing -- and **both** assemblers have it now, by different routes.
[`src/runtime-select.c`](src/runtime-select.c) reads `/etc/ld.so.conf`, which is
plain text and therefore safe to read from a process running under the patched
`ld-linux`. For the bundled runtime it is now **upstream**:
[Anylinux-sharun@`54208d2`](https://github.com/pkgforge-dev/Anylinux-sharun/commit/54208d2bc7d4c919ba46a6c234f6af7f8426b537) adds the `/usr/local/*` directories
and appends what it scrapes out of `/etc/ld.so.cache`, which is a safe read
there because sharun runs natively as a static-pie binary rather than under the
patched `ld-linux` whose cache handling is why the cache was inhibited. The
patch this repository used to carry is deleted; the three gaps that change does
not cover are named in [`analysis/ground-truth.md`](analysis/ground-truth.md).

## The gap that is not about libc at all

Everything above fixes **one** shape of problem: the host has the driver, it is
nameable, and it was built against a libc the bundle is not. That is the whole
of the Vulkan story, and Vulkan is easy for a structural reason -- the
loader/ICD boundary is thin and universal, every ICD exposes one entry point,
and every distribution that has Vulkan ships one.

OpenGL is not like that, and the difference cost a whole session. The AppImage
bundles libglvnd, which is a **dispatcher**: an application links `libGL.so.1`,
and behind it `libGLX.so.0` `dlopen`s a vendor library,
`libGLX_<vendor>.so.0`. A host whose Mesa was built without glvnd --
every musl distro, and every pre-glvnd glibc distro such as Ubuntu 14.04 --
ships no such file at all. There is nothing for `foreign-dlopen` to carry
across, however good it gets. The user sees:

```
Error: couldn't get an RGB, Double-buffered visual
```

which is about neither visuals nor libc.

So there are two gaps, and they need different repairs:

| | what is wrong | the repair |
|---|---|---|
| **the libc gap** | the host has the plugin and it was built against another libc | `foreign-dlopen.so` rewrites it so its version requirements stop mattering |
| **the interface gap** | the host has the capability but ships nothing in the shape the bundled loader looks for | **replace the bundled loader** |

[`src/gl-fwd.c`](src/gl-fwd.c) is the second repair. It is built with the SONAME
of the library it replaces, so preloading it makes `ld.so` bind the
application's `DT_NEEDED` to it and never load the bundled dispatcher; its
constructor picks a target -- the host's classic `libGL.so.1` on a classic host,
the **bundled** dispatcher on a glvnd host, where it works -- and every entry
point tail-jumps there. The same source file built with a different table and a
different vendor marker is `egl-fwd.so`.

Three things about it are worth stating because each one is a place where the
obvious version is wrong:

- **The table is generated, not written.** A shim that replaces a library must
  export everything that library exports; anything less is `undefined symbol`
  for the first application that links a name outside the list. The bundled
  `libGL.so.1` exports **3470** functions.
  [`tools/gen_gl_fwd.py`](tools/gen_gl_fwd.py) reads them out of the object
  being replaced and `make gl-syms-check` fails the build if the two ever
  disagree. A hand-written 33-symbol version renders `glxgears` perfectly and
  dies on `glGetIntegerv`, which is why [`tests/glprobe.c`](tests/glprobe.c)
  calls past that set **and reads the cleared pixel back**.
- **Each entry point is a two-instruction tail jump**, not a C wrapper. A tail
  jump preserves every argument register, the return value and the varargs count
  in `%al`, so it forwards any signature correctly -- including the ones nobody
  typed out. E58 pins that with eight integer registers, nine float registers, a
  varargs call and a struct return, through a jump that knows none of them.
- **`RTLD_GLOBAL` is asked for by the shim, for the one object it opens.** A
  plugin can import a symbol with no `DT_NEEDED` edge to whoever defines it and
  rely on its loader's closure being in the global scope, which is where
  `libGL` sits natively; a loader that opens `libGL` `RTLD_LOCAL` breaks that
  without touching either file (E54, E55). Making *every* foreign `dlopen`
  global would cover the same case and would also hand host definitions a win
  over bundled ones that they do not have natively. Worth knowing that no Mesa
  available here still needs it -- the `_glapi_*` case is against Mesa 10.1,
  reported from outside this repository, and REPORT.md 9.5 labels it that way.

The gap and the mechanism came from
[PR #2](https://github.com/Azathothas/dlopen-experiment/pull/2), opened from
outside against a repository that had written this off. What is here differs
from that patch in five measured ways (REPORT.md 9), but it exists because
somebody else did not accept the sentence below.

And that sentence is the part worth keeping. This gap was recorded in the
previous README as "not done", with a reason that was measured and a verdict --
*no loader shim can supply a file the distribution does not ship* -- that was
never tested and was wrong.
[`tools/plugin_boundaries.py`](tools/plugin_boundaries.py) is the answer to that
class of mistake: every bundled object that imports `dlopen` is a loader by
construction, so the set of them is a property of the bundle rather than
something anyone has to think of. E59 fails the suite on a loader nobody has
classified. It found `libdecor-0.so.0`, which turned out to be benign -- but
"benign, checked" and "never looked at" are different states.

[REPORT.md 9](REPORT.md) has the whole chain.

## What is not done

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
  (`RS_LDSO`, `RS_TRIPLET`, the `gl-fwd` trampolines, syscall-number fallbacks)
  and that is unverified. `make gl-fwd-asm-check` at least assembles the
  aarch64 trampolines, which is a weaker claim than running them and is
  labelled as such.
- **One boundary in this AppDir is identified and unmeasured, and six more are
  listed for AppDirs that have them.** `libX11.so.6` can load i18n modules from
  `/usr/lib/X11/locale` on a build that has them, and nothing here has run that.
  [`tools/plugin_boundaries.py`](tools/plugin_boundaries.py) reports it as
  `unmeasured` rather than folding it into `covered`, which is the distinction
  the OpenGL gap turned on. The same table classifies `libva`, `libvdpau`,
  `libasound`, `libpulse`, `libOpenCL` and `libgbm` on sight for an AppImage
  that bundles one -- each the shape the OpenGL boundary was, none of them a
  libc problem. Named, not fixed.
- **1097 of the 3470 GL entry points have no host implementation** on Alpine's
  Mesa 25.1: 1357 are exported, 1016 more come back from
  `glXGetProcAddressARB`, and the rest are extensions glvnd knows about and
  this Mesa does not implement. They forward to a stub that returns zero. That
  is the same answer an application would get natively on that host, but the
  count is a property of the host's Mesa and the shim reports it rather than
  hiding it (`ANYLINUX_LIB_DEBUG=1`).

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
src/gl-fwd.c                       stand in for a bundled dispatcher the host
                                   cannot serve; built twice, as gl-fwd.so and
                                   egl-fwd.so
src/gl-fwd-gl.h                    GENERATED: 3470 libGL.so.1 entry points
src/gl-fwd-egl.h                   GENERATED: 44 libEGL.so.1 entry points
src/Makefile                       build; `make shim` regenerates, `make traps`
                                   audits, `make gl-syms` re-reads the tables

tools/libc_inventory.py            symbol inventory and per-release diff
tools/gen_forward_shim.py          the shim generator
tools/version_traps.py             which symbols an unversioned reference gets wrong
tools/trap_users.py                and which of them a given object actually steps on
tools/gen_gl_fwd.py                read a dispatcher's export table into a shim table
tools/plugin_boundaries.py         every bundled object that loads a host plugin
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
tests/glprobe.c                    GL past the glxgears symbol set, with the frame
                                   read back so a stub cannot pass for a driver
tests/eglprobe.c                   the same question asked of EGL, with no X at all
tests/abi-abi.h                    the view both sides of the ABI boundary fill
tests/abi-guest.c                  the host driver's side; built by glibc and by musl
tests/abi-host.c                   the crossings: allocator, errno, FILE*, mutex, structs

experiments/run.ps1                one command, the whole evidence table
experiments/1x-3x.sh               the three container stages behind run.ps1
experiments/appimage.ps1           the end-to-end proof on a real AppImage
experiments/4x-*.sh                its stages: extract, build on the floor, the musl
                                   ABI guest, and the two hosts
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
| E59 | every bundled object that `dlopen`s something is classified | 8 loaders, 0 unclassified |
| E60 | the GL forwarding table still matches the bundled libglvnd | 3470 entry points, no drift |
| E61/E62 | `glxgears`, no shim vs shim | musl: `couldn't get an RGB` -> **renders**; glibc: renders either way |
| E63/E64 | `glprobe`: GL past the `glxgears` symbol set, with the frame read back | musl: `no RGB visual` -> **`64 128 191 255`** |
| E65/E66 | `eglprobe`, GL shim only vs GL+EGL shims | musl: `EGL_NO_DISPLAY` -> **`OK: EGL is complete`** |
| E67 | `vkcube` with both shims preloaded | **unaffected** |
| E68 | the shim pointed at itself | refuses by name; the app gets its own failure instead of a stack overflow |

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

Re-reading the GL and EGL forwarding tables out of a different bundled
libglvnd. Needed only when the bundle changes; `gl-syms-check` is what makes a
silent drift impossible, and the AppImage suite runs it against the real
extracted AppDir (E60).

```bash
make gl-syms GLVND=/path/to/AppDir/lib
make gl-syms-check GLVND=/path/to/AppDir/lib
```

The shims go in the AppDir's `.preload`. Their order relative to
`foreign-dlopen.so` does not matter -- `gl-fwd` asks for what it needs rather
than assuming it has already happened -- which is deliberate, because preload
constructors run in **reverse** of that file (E56, E57):

```
path-mapping.so
anylinux.so
foreign-dlopen.so
gl-fwd.so
egl-fwd.so
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
| `ANYLINUX_GL_FWD_TARGET=host\|bundled\|auto` | which library `gl-fwd.so` and `egl-fwd.so` forward to. `auto` picks the bundled dispatcher when the host has a vendor library for it and the host's own library otherwise; forcing either way is the A/B |
| `ANYLINUX_GL_HOST_DIR=<dir>[:<dir>...]` | directories to look in first for the one SONAME the shim is impersonating, ahead of the conventional list |

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
