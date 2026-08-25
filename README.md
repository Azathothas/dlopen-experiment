# Cross-libc `dlopen`

Load a host shared library that was built against a **different libc** (a newer
glibc, or musl) without patching host files, without a second libc in the
process, and without symbol collisions.

The target is [Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages)'
`foreign-dlopen.c`, which lets an AppImage use the host's GPU drivers instead
of bundling 100-200 MB of Mesa and LLVM.

```powershell
.\experiments\run.ps1        # ~3 min, needs podman or docker. 22/22 predictions hold.
```

## The problem

| # | Process libc | Host library libc | Status |
|---|---|---|---|
| 1 | older glibc | newer glibc | **Solved.** Two mechanisms, see below |
| 2 | glibc | musl | **Loading solved, rendering not.** 247/247 libraries load; `vkcube` still does not render |
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
| Evidence table still holds | **22/22**, up from 14/14 | [`experiments/run.ps1`](experiments/run.ps1), cases in [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| Host driver built against newer glibc loads into older bundled glibc | **Achieved** | E5 (shim path), E12 (host runtime, no shim), [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| Host runtime selected correctly per distro | **8/8 distros** | [`src/runtime-select.c`](src/runtime-select.c), E17-E21, [REPORT.md](REPORT.md#4-design-r-host-runtime-selection) |
| Mixed runtime set refused (the configuration that segfaults) | **Refused**, with an accept-control | E20 refuses, E21 accepts, [`30-run-tests.sh`](experiments/30-run-tests.sh) |
| musl host libraries loading into a glibc process | **2/247 to 247/247**, zero regressions | [`tests/corpus.c`](tests/corpus.c), [REPORT.md](REPORT.md#6-goal-2-what-works-and-exactly-where-it-stops) |
| Host Vulkan ICD loads and is callable | **`vkCreateInstance` returns `VK_SUCCESS`** | [`tests/icd-harness.c`](tests/icd-harness.c), [`tests/vkprobe.c`](tests/vkprobe.c) |
| `vkcube` renders on Alpine using Alpine's Mesa | **Not achieved** | [REPORT.md](REPORT.md#6-goal-2-what-works-and-exactly-where-it-stops), continuation in [CONTINUE.md](CONTINUE.md) |
| No host file modified | **Identical sha256** before and after | [`tests/invariants.c`](tests/invariants.c), REPORT.md T4.3 |
| Bundled libraries beat host libraries | **All collision-surface sonames resolve under `$APPDIR`** | [`tests/invariants.c`](tests/invariants.c) |
| Exactly one libc family in the process | **glibc yes, musl no** | [`tests/invariants.c`](tests/invariants.c) |
| Generated shim is correct, not just compilable | **42 behavioural checks on real glibc 2.31** | [`tests/shim-selftest.c`](tests/shim-selftest.c), case E16 |
| ELF rewriting is safe on hostile input | **Truncations and bit flips refused or bounded** | [`tests/elf-selftest.c`](tests/elf-selftest.c), case E14 |
| Library-path completeness fix for sharun | **Patch written, compiled, run on 3 distros** | [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch) |
| Verdict on the two rejected designs | **Written, with evidence** | [`analysis/rejected-designs.md`](analysis/rejected-designs.md) |

## Three defects found by measurement

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

## What is not done

- **`vkcube` does not render on Alpine.** `vkEnumeratePhysicalDevices` fails
  inside lavapipe, past symbol resolution. Ruled out by measurement: missing
  symbols, real allocation failure, the symbol rename, duplicate `libstdc++`,
  and `issetugid`. See [CONTINUE.md](CONTINUE.md).
- **The glibc-vs-musl struct-size hazards are unexercised.** `regmatch_t`,
  `rusage`, `sched_param`, `ucontext_t`, the `FTW_*` constants and
  `O_LARGEFILE` all differ, and nothing here proves the loaded closure avoids
  them. This is the most probable cause of the item above.
- **Design R has never run a GPU workload.** It selects correctly on eight
  distros and passes its self-test, but the end-to-end path is unverified,
  because the only end-to-end target available is the musl case, where Design R
  correctly declines to switch.
- **No hardware validation.** No discrete GPU, no NVIDIA, no aarch64.
  Everything runs on Mesa lavapipe and llvmpipe, which exercise the identical
  `dlopen` path.

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
src/runtime-select.c               host-runtime selection at exec time
src/forward-shim.c                 GENERATED, do not edit
src/forward-shim-manifest.json     per-symbol classification and the floor
src/Makefile                       build; `make shim` regenerates for a new floor

tools/libc_inventory.py            symbol inventory and per-release diff
tools/gen_forward_shim.py          the shim generator
inventories/*.json                 inventories the generator reads

tests/elf-selftest.c               ELF rewriting, against the real implementation
tests/shim-selftest.c              42 behavioural checks on the generated shim
tests/corpus.c                     load every .so in a directory, before vs after
tests/icd-harness.c                foreign-load a real Vulkan ICD
tests/vkprobe.c                    bundled loader plus host ICD, no window system
tests/invariants.c                 one libc family, bundled wins
tests/allocprobe.c                 interpose the allocator to catch NULL returns

experiments/run.ps1                one command, the whole evidence table
experiments/*.sh                   the three container stages
patches/                           the sharun fix, to be upstreamed by hand
scripts/wsl-ephemeral.ps1          throwaway WSL2 distros from any OCI image
elfsym.py, gap.py                  dependency-free ELF reader and gap driver
```

## Evidence table

Run by `experiments/run.ps1`. E1-E13 measure the problem, E14-E21 measure the
fix. Every case states a prediction and the harness reports MATCH or MISMATCH.

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

## Building

Build on the **oldest** glibc you intend to support, not the newest. The
products only need symbols from that floor, so an old build runs everywhere and
a new one does not.

```bash
cd src && make
```

```bash
make shim FLOOR=../inventories/glibc-2.31.json TARGET=../inventories/glibc-2.44.json
```

## Runtime switches

| Variable | Effect |
|---|---|
| `ANYLINUX_LIB_FOREIGN_DLOPEN=1` / `=0` | force the feature on or off; `=0` is the A/B control |
| `ANYLINUX_RUNTIME=host\|bundled\|auto` | force or auto-select the libc runtime |
| `ANYLINUX_LIB_DEBUG=1` | trace to stderr, prefixed ` [foreign-dlopen.so] >> ` |
| `ANYLINUX_LIB_FOREIGN_DRYRUN=1` | report what would be rewritten and what would not resolve, load nothing |
| `ANYLINUX_LIB_FOREIGN_NORENAME=1` | disable symbol renaming, for bisecting a misbehaving driver |

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
