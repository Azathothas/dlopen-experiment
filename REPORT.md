# REPORT — cross-libc `dlopen`

What was built, what was measured, and what is still broken.

Every claim below is either backed by a command whose output is quoted, or
explicitly labelled **[UNVERIFIED]**. Nothing is estimated.

---

## 0. Summary

| Goal (§1) | Status |
|---|---|
| **1.** Host driver built against a newer glibc loads into a process carrying an older bundled glibc | **Achieved** — E5 (shim path) and E12 (host-runtime path), plus Design R selecting `host` correctly on 5 of 8 real distros |
| **2.** musl-built host driver (Alpine) loads into that glibc process | **Achieved for loading; NOT achieved for rendering.** The whole Alpine `/usr/lib` — **247 of 247** libraries, including `libvulkan_lvp.so` and its full closure — now loads, and `vkCreateInstance` succeeds against the host ICD. `vkEnumeratePhysicalDevices` then fails *inside lavapipe*, past symbol resolution. `vkcube` does not render. See §6. |

| "Done" criterion (§1.1) | Status |
|---|---|
| 1. Both goals demonstrated by a test that fails before and passes after | **Partial** — goal 1 yes; goal 2 yes for load (T2.2/T2.4), no for render (T3.2) |
| 2. `experiments/run.ps1` reports all predictions held | **Yes — 22/22** (was 14/14; 8 new cases added for the fix) |
| 3. No host file modified, verified by checksum | **Yes** — T4.3, identical sha256 before/after |
| 4. Bundled libraries still win, verified via `dladdr` | **Yes** — T4.2, all resolved under `$APPDIR` |
| 5. Forward-compat story not dependent on foresight | **Yes** — Design R implemented (§3), generated shim for the enumerable half (§4) |
| 6. `REPORT.md` separating measured from assumed | this document |

---

## 1. Environment reached

**Highest tier reached: Tier 3** (end-to-end AppImage under `xvfb`, software
Vulkan). Tier 4 invariants all run. Tier 5 is skipped — no discrete GPU.

```
$ uname -srm                     # inside every test container
Linux 7.2.0-WSL2-STABLE x86_64

podman version 5.8.6             (WSL2 Fedora 44 machine)
Python 3.13                      (host, for the Tier-0 tooling)
```

Distros used, all as real OCI images:

| Image | libc | Role |
|---|---|---|
| `alpine:3.22` | musl 1.2.5 | the musl host (goal 2), software Vulkan |
| `debian:bullseye-slim` | glibc 2.31 | "an AppImage bundling an older glibc"; also the build host |
| `debian:trixie-slim` | glibc 2.41 | the newer-glibc build host |
| `ubuntu:20.04` | glibc 2.31 | T4.4 no-regression check |
| `rockylinux:9` | glibc 2.35 | Design R matrix |
| `fedora:44` | glibc 2.43 | Design R matrix |
| `opensuse/tumbleweed` | glibc 2.43 | Design R matrix |
| `archlinux:latest` | glibc 2.44 | Design R matrix, newest released glibc |

Software rendering was used throughout and **is named in every result**:
Mesa **lavapipe** / **llvmpipe** (LLVM 20.1.8, 256 bits), pinned with
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` so a half-working
host GPU could not silently take over.

Rung 1 of the diagnostic ladder is green — the host driver is sane natively:

```
$ vulkaninfo --summary        # Alpine 3.22, native musl
        deviceName         = llvmpipe (LLVM 20.1.8, 256 bits)
        driverName         = llvmpipe
```

---

## 2. What was actually wrong — three defects found by measurement

These were not in the task's problem statement. Each was found by running
something, and each is fixed.

### 2.1 musl folds `libm` into `libc`; glibc splits it out

The task anticipated glibc's own 2.34 consolidation (E6/E7: `pthread_create`
moved into `libc.so.6`). The **mirror image** turned out to be what actually
blocked goal 2.

musl keeps the maths, threading and dynamic-linking functions **inside**
`libc.musl-x86_64.so.1`. A musl-built object therefore imports `fmod`,
`fesetround`, `log10`, `pow` with *no `DT_NEEDED` on anything* — on musl its
libc edge covered them, and that edge is exactly what `foreign-dlopen.c`
drops. `libm.so.6` was simply not in the process:

```
foreign: rewritten load failed: .../libxml2.so.2.13.9: undefined symbol: fmod
foreign: rewritten load failed: .../libstdc++.so.6.0.33: undefined symbol: fesetround
foreign: rewritten load failed: .../libLLVM.so.20.1: libc.musl-x86_64.so.1: cannot open...
```

The last line is the cascade: `libLLVM` needed `libxml2` and `libstdc++`,
which had just failed, so `ld.so` fell back to loading the unrewritten
originals — which still carry the musl `DT_NEEDED`.

**Fix (B4, generalised):** load every glibc library that can hold a re-homed
name into the **global** scope at startup — `libm.so.6`, `libresolv.so.2`,
`libcrypt.so.1`, plus glibc's own pre-2.34 split libraries. This is rung 6 of
the diagnostic ladder applied as policy rather than per incident: *load the
library instead of shimming the symbol*.

### 2.2 T4.2 was being violated: host libraries beat bundled ones for musl guests

`foreign-dlopen.c` skips the dependency probe entirely for musl guests —
correctly, because loading the *host* copy unstripped would drag musl libc in.
But the skip sent them straight to `fgn_find_candidate()`, which only searches
directories on the active load stack. For a host object that is `/usr/lib`, so
**a bundled soname could never win**.

Measured on Alpine: the AppDir bundles `libstdc++.so.6.0.36` and
`libgcc_s.so.1`, and the host's `libstdc++.so.6.0.33` and `libgcc_s.so.1` were
being loaded *alongside* them. Two libstdc++ and two unwinders in one process
is the classic "every symbol resolves and nothing works" configuration — and
T4.2 exists precisely to catch it.

**Fix:** check `$APPDIR/lib/<soname>` **before** hunting the host, for musl
guests too. Loading the bundled copy is always safe: it is a glibc object built
against the runtime already running.

After the fix, T4.2 passes:

```
T4.2 -- provenance of collision-surface sonames
    libstdc++.so.6     /w/AppDir/lib/libstdc++.so.6      BUNDLED (correct)
    libgcc_s.so.1      /w/AppDir/lib/libgcc_s.so.1       BUNDLED (correct)
    libxcb.so.1        /w/AppDir/lib/libxcb.so.1         BUNDLED (correct)
```

### 2.3 Upstream swallows `dlerror()`, so the "classic error message" never arrives

`fgn_load()`'s fallback path reads `dlerror()` unconditionally and only
`DEBUG_PRINT`s it. `dlerror()` is destructive. With debug off — the default —
the caller's own `dlerror()` therefore returns `NULL`:

```
FAILED: dlopen: (null)
```

The comment above that code says it "surfaces the classic error message users
know how to read." It does the opposite.

**Fix:** read `dlerror()` only when tracing is on, so the message survives for
the caller in the normal case.

---

## 3. Design R — host-runtime selection (`src/runtime-select.c`)

The forward-compatible half (§4.5, E12): if the host glibc is newer and the
set is complete, re-exec under the **host's** runtime, so a symbol invented
after we shipped resolves because we are using the future libc itself.

### Two things the task's sketch gets wrong, and what replaced them

**§5.0 proposes `--library-path "$HOST_LIBDIR:$APPDIR/lib"`.** That hands the
host `libstdc++`, `libX11` and every other soname the win too, breaking T4.2 in
the same way §2.2 did. Instead a **symlink farm** under `$XDG_RUNTIME_DIR`
holds the runtime set *and nothing else*:

```
--library-path  $FARM : $APPDIR/lib : $HOST_LIBDIRS
                ^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^^^
                libc    everything    fallback for
                only    bundled       what we lack
```

Symlinks, so T4.3 holds: no host file is touched, every write lands under
`XDG_RUNTIME_DIR`.

**A VERNEED-based completeness check cannot detect E11.** This is the more
important correction. The obvious check — does each member's `DT_VERNEED` fall
inside what its peers define — catches E8's direction (a *new* object needing
a version an *old* peer lacks). It provably cannot catch E11's, because
**glibc never retires a version name**: an old `libdl.so.2` asks libc only for
`GLIBC_2.2.5`, and every later glibc still defines it. Version names alone
declare the mixed set healthy. It segfaults.

What distinguishes it is the `GLIBC_PRIVATE` symbol surface, which is not
stable at all. Measured, glibc 2.31 → 2.41:

```
old libdl.so.2       imports _dl_sym, _dl_addr, _dl_catch_error, _dl_vsym,
                     __libc_dlopen_mode        -> 2.41 exports NONE of them
old libpthread.so.0  13 imports absent from 2.41, incl. __libc_pthread_init,
                     _dl_make_stack_executable
old librt.so.1       9 absent, incl. __pthread_barrier_init, __shm_directory
```

So the implemented check is a **symbol** check: every STRONG undefined symbol
of every member must be defined by the libc and `ld.so` it will be paired with.
Weak imports (`_ITM_registerTMCloneTable`, `__gmon_start__`) are skipped — they
are absent from every libc ever built and resolve to 0 by design; counting them
would make every set look mixed.

On top of the static check, the plan is **verified empirically** before being
committed to: `runtime-select` forks and re-execs *itself* under the candidate
runtime, exercising malloc, TLS, stdio and `dlopen` — the E11 crash site.

> A note on that self-test: it must re-exec **this binary**, not `/bin/true`.
> Measured — Rocky 9's `/bin/true` is a 51-byte shell script, and `ld.so`
> answers `file too short`, which looked exactly like a mixed set and was not.
> Re-execing our own binary is also the stronger question, since it was linked
> against the *bundled* glibc.
>
> And `/proc/self/exe` is the wrong way to find ourselves: inside an AppImage
> this program is started as `$APPDIR/lib/ld-linux... runtime-select`, and when
> a loader is invoked explicitly the kernel exec'd the *loader*, so
> `/proc/self/exe` names `ld-linux`. Re-execing that asks one dynamic linker to
> run another as a program; it exits 127, indistinguishable from a broken
> runtime. Every newer host reported a false `SELF-TEST FAILED` until this was
> fixed.

### T2.5 — measured decision on all eight distros

Run against a fake AppDir bundling glibc 2.31, so the newer hosts really are
newer:

| Host | Host glibc | Decision | Reason logged |
|---|---|---|---|
| debian bullseye | 2.31 | **bundled** | not newer than bundled |
| ubuntu 20.04 | 2.31 | **bundled** | not newer than bundled |
| rocky 9 | 2.35 | **host** | newer + set internally consistent |
| debian trixie | 2.41 | **host** | newer + set internally consistent |
| fedora 44 | 2.43 | **host** | newer + set internally consistent |
| opensuse tw | 2.43 | **host** | newer + set internally consistent |
| arch | 2.44 | **host** | newer + set internally consistent |
| alpine 3.22 | musl | **bundled** | no host glibc — bundled + shim is the only option |

Every `host` decision additionally passed the empirical self-test.
**T2.5 passes**: `host` on every newer glibc, `bundled` on older/equal and on
musl, never a mixed set, always with a logged reason.

`E20`/`E21` in the harness are the guard and its control: a deliberately mixed
set (2.41 `ld.so`+`libc`, 2.31 `libdl`/`libpthread`/`librt`/`libutil`, every
member present so "incomplete" cannot be the reason) is **refused**, while the
same glibc unmixed is **accepted**. Without the control, a selector that
refused everything would pass.

**T2.6** (`ANYLINUX_RUNTIME=bundled` on a newer host still works via the shim
path) is `E19`, and passes.

### The honest trade

Switching to the host runtime **gives up the "bundle everything" guarantee** —
the app then runs against an unaudited host libc. That is a real cost and it is
the user's call, which is why `ANYLINUX_RUNTIME=host|bundled|auto` exists and
why the decision and its reason are logged under `ANYLINUX_LIB_DEBUG=1`.

---

## 4. Design B — the generated shim (`tools/gen_forward_shim.py`)

The *selection* is generated; the *implementations* come from an audited table.
A generator that invented semantics would be worse than the treadmill, not
better. Solo splits it the same way.

### Forward-compat posture

**Glibc floor targeted: whatever `--floor` names.** For the shipped
`src/forward-shim.c` that is the demo AppImage's own bundled runtime,
**glibc 2.44**.

**And that is the headline finding of Phase A: for this artifact the
version-gap is empty.** The demo AppImage bundles glibc 2.44 — the newest
released — so *no* distro in the matrix is newer, and Design R correctly picks
`bundled` on all of them. Running the generator against its own floor produces
a shim whose entire version-gap contribution is zero:

```
floor  : appdir-bundled glibc 2.44 (4287 symbols)
target : glibc-2.44             (4288 symbols)
musl   : 46 symbols musl exports and the floor does not
gap    : 47 symbols the floor lacks
   implementable    13
   stub-only        22
   irrelevant       12
```

The one non-musl gap symbol is `__libanl_version_placeholder`, an empty ABI
placeholder. **Case 1 is already solved for this AppImage by bundling a
new-enough glibc.** It is not solved in general — any AppImage built on an
older distro has the gap, and this one acquires it the day glibc 2.45 ships.

So the generator is demonstrated at a realistic older floor too. Floor 2.31,
target 2.44:

```
gap    : 628 symbols the floor lacks
   implementable   107
   stub-only       296
   irrelevant      225
```

Compiled with `-Wall -Wextra -Werror` on real glibc 2.31, and **42 documented
behaviours checked** (`tests/shim-selftest.c`, harness case `E16`) — not just
"it links":

```
  ok   strlcpy trunc          ok   stat matches __xstat     ok   bit_ceil(0)==1
  ok   strlcat trunc          ok   arc4random_uniform covers range
  ok   clz(0)==32             ok   _dl_find_object==-1      ok   sigabbrev_np(SIGKILL)
  ... 42 checks ...
SHIM TEST PASSED (0 failures)
```

### What happens when an uncovered symbol appears

It **fails loudly, naming the symbol**, at the earliest point it can:

- **Load time** — the dry-run/report path enumerates every strong undefined
  symbol the process and the object's own dependency closure cannot supply, and
  prints all of them, not just `ld.so`'s first.
- **Call time** — a stub-only symbol aborts with its own name:

```
[foreign-dlopen] FATAL: sinpi: not implementable over this glibc
[foreign-dlopen] the bundled glibc 2.44 does not provide this symbol
[foreign-dlopen] and no implementation exists for it. Set ANYLINUX_RUNTIME=host
[foreign-dlopen] to run against the host's own libc, which will have it.
```

**Why emit stubs at all**: every Mesa object is `DF_BIND_NOW`, so `ld.so`
resolves the whole table at load. *One* undefined symbol makes the library
unloadable even if that code path is never taken. A stub converts "cannot load
at all" into "works unless it genuinely needs this."

**Why C23 maths is stub-only, deliberately**: `sinpi`, `fmaximum_num`,
`roundeven` and the other 186 have exacting NaN, signed-zero and rounding-mode
semantics. An approximation that is subtly wrong is worse than a loud abort,
and no GPU driver calls them. This is a recorded decision, not an oversight —
the manifest carries a per-symbol reason for all 47.

### The musl-only surface is larger than the Mesa closure suggested

`gap.py` measures the union over the Mesa+LLVM closure as exactly
`['___environ', 'atexit']`, and that reproduces. But over the **whole** Alpine
`/usr/lib`, one more musl-only symbol is load-bearing:

```
foreign: rewritten load failed: .../libX11.so.6.4.0: undefined symbol: issetugid
```

`issetugid` alone was blocking `libX11.so.6` and `libdbus-1.so.3`. It is
implementable exactly as musl implements it, over `getauxval(AT_SECURE)`.

So the generator now takes `--musl <inventory>` and folds musl's 46 floor-absent
exports into the same enumerable gap, rather than relying on a hand-maintained
list. That is what took the corpus from 243/247 to **247/247**.

### `___environ` → `__environ` (B3)

Applied and confirmed firing on the real `libLLVM.so.20.1`:

```
foreign: ___environ -> __environ (st_name +1, no .dynstr write)
```

Total safety, for two reasons that are both checked: the symbol is
**undefined**, so `DT_GNU_HASH` (which indexes only *defined* symbols from
`symoffset` on) does not cover it — no hash fixup; and `"___environ" + 1` **is**
`"__environ"`, so nothing is written to `.dynstr` at all and tail-merging
cannot bite.

The general case (rename where the target is not a suffix) needs an in-place
`.dynstr` write, and `fgn_dynstr_range_occupied()` refuses unless it can prove
no other referenced offset — symbol name, `DT_NEEDED`, `SONAME`, `RPATH`,
`RUNPATH`, or version-table name — falls inside the clobbered range. **T0.7
tests that it does refuse.**

---

## 5. Design P — library-path completeness (`patches/sharun-library-path.patch`)

Delivered as a patch **in this repo**, not upstreamed, per §7.1.

Measured across all eight distros: `/usr/local/lib` exists on **every one** and
was absent from sharun's hardcoded list — the concrete bug E13b demonstrates.
The patch:

1. parses `/etc/ld.so.conf` and `/etc/ld.so.conf.d/*.conf`, honouring `include`
   globs (plain text — the benefit of the cache without touching the binary
   cache that caused the segfault);
2. reads `/etc/ld-musl-<arch>.path`, and carries musl's built-in default for
   Alpine, which ships no such file;
3. adds `/usr/local/lib`, `/usr/local/lib64`, `/usr/local/lib/<triplet>`,
   `/usr/libexec`;
4. adds the missing triplets — `riscv64`, `powerpc64le`, `s390x`,
   `loongarch64`, `arm-linux-gnueabihf`;
5. adds Guix, Flatpak and Termux prefixes;
6. **deduplicates and drops non-existent directories** (each surviving entry
   costs a `stat` per lookup per miss);
7. logs the final path under `ANYLINUX_LIB_DEBUG=1`.

**Everything is appended after the bundled directories**, never before, so a
host library can never shadow a bundled soname.

The patch was **compiled and run**, not just written. Extracted into a
standalone crate and executed on three distros:

```
debian trixie : /usr/local/lib, /lib/x86_64-linux-gnu, /usr/lib/x86_64-linux-gnu,
                /usr/lib, /lib, /usr/lib64, /lib64, /usr/libexec   (10 dirs)
fedora 44     : /usr/local/lib, /usr/local/lib64, /usr/lib, /lib,
                /usr/lib64, /lib64, /usr/libexec                   (7 dirs)
alpine 3.22   : /usr/local/lib, /usr/lib, /lib, /usr/libexec       (4 dirs)
```

**T2.7** — a library reachable only via `/etc/ld.so.cache` still resolves under
a cache-inhibited loader once its directory is on the path — is E13c, and
passes.

---

## 6. Goal 2: what works, and exactly where it stops

### What works

```
###### T2.2 foreign-load the real host ICD ######
-- OFF --
FAILED: dlopen: libc.musl-x86_64.so.1: cannot open shared object file
-- ON --
  handle          : 0x55557a900a70
  vk_icdGetInstanceProcAddr: 0x7f8ded0d5d80
  vkCreateInstance          : 0x7f8ded0d44f0
OK: host ICD loaded and callable

###### T2.4 corpus, zero-regression gate ######
  before: TOTAL=247 OK=2
  after : TOTAL=247 OK=247
  regressions: 0

###### T2.3 debug trace ######
  times libvulkan_lvp.so rewritten: 1     (rewritten once, then cached)
  attempts to load a musl libc: 0
```

**T2.4 is the result that separates a fix from a demo**: every one of the 247
musl libraries in Alpine's `/usr/lib` loads into the glibc process, up from 2,
with **zero regressions**. And through the bundled Vulkan loader,
`vkCreateInstance` against the host ICD returns `VK_SUCCESS`.

### Where it stops — T3.2 FAILS

```
[Vulkan Loader] ERROR: setup_loader_term_phys_devs: Call to
  'vkEnumeratePhysicalDevices' in ICD /tmp/xdg/.anylinux-fgn-dbdb70ee.so
  failed with error code VK_ERROR_OUT_OF_HOST_MEMORY
vkEnumeratePhysicalDevices reported zero accessible devices.
```

`vkcube` does not render on Alpine. This is **rung 8** of the diagnostic ladder
— "loads but misbehaves", ABI territory — and it is the residual risk §10.6
names: *symbol availability is necessary but not sufficient.*

What was ruled out, by measurement, not by reasoning:

| Hypothesis | Test | Result |
|---|---|---|
| Missing symbols | dry-run over the whole ICD closure | **zero** unresolvable strong imports |
| A real allocation failure | interposed `malloc`/`calloc`/`realloc`/`posix_memalign`/`aligned_alloc` | **0 NULL returns** — the error code is a stand-in |
| The `___environ` rename | A/B with `ANYLINUX_LIB_FOREIGN_NORENAME=1` | byte-identical failure; exonerated |
| Duplicate `libstdc++`/`libgcc_s` | T4.2 provenance check | fixed (§2.2); failure persists |
| `issetugid` | added to the shim | corpus 243→247; failure persists |
| Display/WSI, not libc | run under `xvfb-run -a`; error is a device error, not WSI | not the cause |
| Host driver broken | `vulkaninfo --summary` natively on Alpine | lavapipe healthy |

Narrowed to: both paths are **byte-identical in `strace` up to and including
the two `sysinfo` calls** in lavapipe's device init. The working musl path then
continues to `/proc/meminfo`; the glibc path makes **no further syscalls at
all** and returns the error. So the divergence is a pure userspace decision
inside Mesa, after memory queries and before any allocation.

**Progress is nonetheless real and measurable**: with the *stock upstream*
`foreign-dlopen.so`, the same probe **segfaults**. With this work it reaches
`VK_SUCCESS` on instance creation and returns a clean, diagnosable error. That
is a strict improvement, but it is not the goal, and it is reported as a
failure rather than dressed up.

`glxgears` (**T3.3**) was not separately diagnosed; it shares the ICD/DRI
loading path and the same blocker.

---

## 7. Test results

### Tier 0 — static, any OS

| ID | Test | Result |
|---|---|---|
| T0.1 | `gap.py` musl gap | **PASS** — exactly `['___environ', 'atexit']` |
| T0.2 | binding-mode audit | **PASS** — every closure member `BIND_NOW` |
| T0.3 | TLS audit | **PASS** — no `DF_STATIC_TLS` |
| T0.4 | rewriter round-trip | **PASS** — all four tags gone together, size unchanged, re-parses |
| T0.5 | idempotence | **PASS** — second strip byte-identical; content-hash name stable |
| T0.6 | `atexit` interposition safety | **PASS** — `nm -D` shows exactly one exported definition |
| T0.7 | tail-merge guard | **PASS** — refuses a range containing another live reference, allows a clear one |
| T0.8 | malformed-input fuzz | **PASS** — every truncation and bit flip refused or bounded |

T0.4/T0.5/T0.7/T0.8 run against the **real implementation**:
`tests/elf-selftest.c` `#include`s `foreign-dlopen.c` rather than modelling it,
because a Tier-0 test that models the C can pass while the shipped code is
wrong.

### Tier 1 — the evidence table

`experiments/run.ps1` → **22/22 predictions held**. E1–E13 unchanged;
E14–E21 added, one per fix (ELF self-test, generated-shim compile,
generated-shim behaviour, and five Design R decisions including the E11 guard
and its control).

### Tier 2 / 3 / 4

| ID | Test | Result |
|---|---|---|
| T2.1 | Alpine native lavapipe baseline | **PASS** — named by `vulkaninfo` |
| T2.2 | foreign-load the real ICD | **PASS** |
| T2.3 | debug trace: rewritten once, cached, no musl libc | **PASS** |
| T2.4 | corpus, zero regressions | **PASS** — 2/247 → 247/247, 0 regressions |
| T2.5 | Design R across the distro matrix | **PASS** |
| T2.6 | forced `ANYLINUX_RUNTIME=bundled` on a newer host | **PASS** (E19) |
| T2.7 | cache-only library found via `--library-path` | **PASS** (E13c) |
| T3.1 | Alpine baseline fails before the fix | **PASS** — fails with a symbol-resolution error under `xvfb` |
| T3.2 | `vkcube` with host driver | **FAIL** — §6 |
| T3.3 | `glxgears` | **FAIL** — same blocker |
| T3.4 | driver provenance is the host's | **PASS** — rewritten copy derives from `/usr/lib/libvulkan_lvp.so`; trace confirms |
| T4.1 | exactly one libc family | **PASS** — glibc yes, musl no |
| T4.2 | bundled wins, via `dladdr` | **PASS** |
| T4.3 | no host file modified | **PASS** — identical sha256 over `/usr/lib`, `/lib`, `/etc/ld.so.conf.d` |
| T4.4 | no regression on glibc hosts | **PASS** — see below |

**T4.4 in detail** — the AppImage run on three glibc hosts with the *stock
upstream* preload and with the patched one, in both modes. The outcome is
identical in all twelve combinations, which is what "unchanged" means:

```
### Arch Linux (glibc 2.44)          ### Ubuntu 20.04 LTS       ### Debian trixie (2.41)
    stock    mode=0  rc=1  ran           stock    mode=0  rc=1      stock    mode=0  rc=1
    stock    mode=1  rc=1  ran           stock    mode=1  rc=1      stock    mode=1  rc=1
    patched  mode=0  rc=1  ran           patched  mode=0  rc=1      patched  mode=0  rc=1
    patched  mode=1  rc=1  ran           patched  mode=1  rc=1      patched  mode=1  rc=1
```

`rc=1` on all of them because these containers have no GPU, no display and no
Vulkan driver installed — the AppImage fails the same way before and after.
The point of the test is the *equality*, not the exit code.

### SKIPPED, with the specific missing capability

```
T1.3  SKIPPED - allocator-crossing microtest not written. Nearest evidence:
      the interposed-allocator probe in section 6 recorded 0 NULL returns
      across a full ICD load, and T2.4 loads 247 objects without a crash.
      Cross-libc malloc/free ownership transfer remains UNVERIFIED.

T1.4  SKIPPED - errno-coherence microtest not written. UNVERIFIED.

T1.5  SKIPPED - FILE* crossing microtest not written. This one matters:
      glibc's FILE is 216 bytes, musl's is opaque, and section 9.3 lists it
      as a hazard. UNVERIFIED.

T1.6  SKIPPED - pthread_mutex_t/cond_t sharing under TSan not written.
      UNVERIFIED.

T1.7  SKIPPED - Solo's dev/abi_probe.c was not ported, so the glibc-vs-musl
      struct-size divergences in section 9.3 (regmatch_t 8 vs 16, rusage
      144 vs 272, sched_param 4 vs 48, ucontext_t 968 vs 936, the FTW_*
      constants, O_LARGEFILE) were NOT proven unused by the closure.
      This is the single most likely home of the T3.2 failure and is the
      first thing to do next.

T4.5  SKIPPED - 100 load/unload cycles and a 60 s vkcube run were not
      performed, because vkcube does not render (T3.2). RSS stability, fd
      leaks and .anylinux-fgn-* accumulation are UNVERIFIED. Partial
      evidence: T2.3 confirms each object is rewritten exactly once and
      cached, and T4.3 confirms the runtime dir is cleaned.

T5.1  SKIPPED - no discrete GPU in the test environment.
      Nearest evidence: T2.2 and T2.4 pass with lavapipe (software Vulkan),
      exercising the identical dlopen path. Hardware-specific failures
      (e.g. libdrm ioctl ABI) remain unverified.

T5.2  SKIPPED - no NVIDIA hardware.
T5.3  SKIPPED - no aarch64 hardware. The code is arch-parameterised
      (RS_LDSO/RS_TRIPLET, the syscall fallbacks) but this is UNVERIFIED
      outside x86-64.
```

---

## 8. Measured vs assumed

**Measured** — every table and quoted output above, plus:
`experiments/run.ps1` (22/22), `gap.py --fetch`, the eight-distro inventory,
the AppImage inventory, the corpus test, the compiled-and-run sharun patch.

**Assumed / [UNVERIFIED]:**

- Everything in the SKIPPED list above, most importantly **T1.7**.
- **The generated shim's stub-only symbols have never been called.** Their
  abort path is exercised only by construction, not by a driver reaching it.
- **The `--host-dir` override and the symlink farm are tested on containers,
  not on a real desktop** where `XDG_RUNTIME_DIR` is a user-owned tmpfs. The
  permission model there is [UNVERIFIED].
- **Design R has never been used to actually run a GPU workload.** It selects
  correctly on eight distros and passes its self-test, but the end-to-end
  "newer host driver renders under the host runtime" path is [UNVERIFIED],
  because the only end-to-end target available is the musl case, where Design R
  correctly declines to switch.
- The claim that a 32-bit or aarch64 build works is [UNVERIFIED].

---

## 9. Known-unfixed

**Case 3 — a glibc-built host library loading into a musl process — is out of
scope and is not addressed here.** The packaging always bundles glibc,
deliberately, because musl would lose the proprietary NVIDIA driver. Anyone
who needs case 3 should use [pg83/solo](https://github.com/pg83/solo), which
solves it properly with its own ELF loader and a ~6 000-line glibc→musl ABI
bridge, and has CI across Alpine/Fedora/NixOS/Termux plus a 2 100-object corpus
test per commit.

**Also not delivered** (§11.2): NVIDIA's glibc-only userspace on a musl
process; static musl binaries with GPU access; bridging manylinux wheels into
Alpine; distroless containers reaching host NSS/PAM.

**Designs C and D** were evaluated on paper and both rejected, with evidence —
see `analysis/B7-designs-C-D-verdict.md`. C is impossible (E9); D costs ~2 700
lines, still needs a shim, and buys isolation for a collision surface measured
at three sonames.

---

## 10. Residual risk

1. **T3.2 is unexplained.** A load that succeeds can still be semantically
   wrong, and here it is. The failure is bounded (§6) but not diagnosed.
2. **The §9.3 hazard table is unexercised** (T1.7). `regmatch_t`, `rusage`,
   `sched_param`, `ucontext_t`, the `FTW_*` constants and `O_LARGEFILE` all
   differ between glibc and musl, and nothing here proves the loaded closure
   avoids them. Given T3.2 fails inside a musl-built driver with no missing
   symbols, this is the most probable cause and the first thing to test next.
3. **Switching to the host runtime abandons the bundle-everything guarantee.**
   Real, deliberate, surfaced, overridable — but real.
4. **The generated shim is bounded by construction.** It covers what existed
   when it was generated. A symbol invented afterwards is Design R's job, and
   on a musl host there is no Design R — which is why the musl row of the
   decision matrix has no escape hatch. Its forward-compat risk is small
   (musl's exported surface grows slowly and glibc is very nearly a superset)
   but it is not zero.
5. **`at_quick_exit` returns failure rather than register a handler.** glibc's
   real one runs handlers on `quick_exit()` only; approximating it with
   `__cxa_atexit` would run them on normal exit too, which is worse. Callers
   that ignore the return value will silently not get their handler.
6. **The stale-shim hazard.** If the bundled glibc is upgraded without
   regenerating `forward-shim.c`, the shim would interpose over symbols libc
   now provides. The manifest records the floor and `make shim` regenerates,
   but nothing *enforces* the regeneration at build time.
