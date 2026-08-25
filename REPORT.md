# REPORT

What was built, what was measured, and what is still broken.

Every claim is either backed by a command whose output is quoted, or labelled
**UNVERIFIED**. Nothing is estimated.

---

## 1. Summary

| Goal | Status |
|---|---|
| A host GPU driver built against a **newer glibc** loads into a process carrying an older bundled glibc | **Achieved.** Two mechanisms: the generated shim (E5) and the host-runtime switch (E12, no shim at all). The selector picks correctly on 8 of 8 distros |
| A **musl-built** host driver loads into that same glibc process | **Loading achieved, rendering not.** All 247 libraries in Alpine's `/usr/lib`, including `libvulkan_lvp.so` and its full closure, now load, and `vkCreateInstance` against the host ICD returns `VK_SUCCESS`. `vkEnumeratePhysicalDevices` then fails inside lavapipe, past symbol resolution. `vkcube` does not render. See section 6 |

| Completion criterion | Status |
|---|---|
| Both goals demonstrated by a test that fails before and passes after | **Partial.** Goal 1 yes. Goal 2 yes for loading (T2.2, T2.4), no for rendering (T3.2) |
| The evidence harness still reports all predictions held | **Yes, 22/22**, up from 14/14, with 8 new cases for the fix |
| No host file modified, verified by checksum | **Yes.** T4.3, identical sha256 before and after |
| Bundled libraries still win, verified via `dladdr` | **Yes.** T4.2, all resolved under `$APPDIR` |
| A forward-compatibility story that does not depend on foresight | **Yes.** Host-runtime selection for the unenumerable gap, a generated shim for the enumerable one |
| A report separating measured from assumed | this document |

---

## 2. Environment reached

**Highest tier reached: Tier 3**, end-to-end AppImage under `xvfb` with
software Vulkan. All Tier 4 invariants run. Tier 5 is skipped: no discrete GPU.

```
$ uname -srm                     # inside every test container
Linux 7.2.0-WSL2-STABLE x86_64

podman version 5.8.6             (WSL2 Fedora 44 machine)
Python 3.13                      (host, for the Tier-0 tooling)
```

| Image | libc | Role |
|---|---|---|
| `alpine:3.22` | musl 1.2.5 | the musl host, software Vulkan |
| `debian:bullseye-slim` | glibc 2.31 | "an AppImage bundling an older glibc", and the build host |
| `debian:trixie-slim` | glibc 2.41 | the newer-glibc build host |
| `ubuntu:20.04` | glibc 2.31 | no-regression check |
| `rockylinux:9` | glibc 2.35 | selector matrix |
| `fedora:44` | glibc 2.43 | selector matrix |
| `opensuse/tumbleweed` | glibc 2.43 | selector matrix |
| `archlinux:latest` | glibc 2.44 | selector matrix, newest released glibc |

Software rendering was used throughout and is named in every result: Mesa
**lavapipe** and **llvmpipe** (LLVM 20.1.8, 256 bits), pinned with
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` so a half-working
host GPU could not silently take over.

The host driver is sane natively, so every downstream result is interpretable:

```
$ vulkaninfo --summary        # Alpine 3.22, native musl
        deviceName         = llvmpipe (LLVM 20.1.8, 256 bits)
        driverName         = llvmpipe
```

---

## 3. Three defects found by measurement

None of these were in the problem statement. Each was found by running
something, and each is fixed.

### 3.1 musl folds `libm` into `libc`; glibc splits it out

The known hazard was glibc's own 2.34 consolidation, where `libpthread`,
`libdl`, `librt`, `libutil` and `libanl` merged into `libc.so.6`, so a modern
build emits `pthread_create@GLIBC_2.34` with no `DT_NEEDED` on `libpthread`.
That is E6 and E7.

The mirror image is what actually blocked the musl case. musl keeps the maths,
threading and dynamic-linking functions **inside** `libc.musl-x86_64.so.1`. A
musl-built object therefore imports `fmod`, `fesetround`, `log10` and `pow`
with no `DT_NEEDED` on anything, because on musl its libc edge covered them,
and that edge is exactly what `foreign-dlopen.c` drops:

```
foreign: rewritten load failed: .../libxml2.so.2.13.9: undefined symbol: fmod
foreign: rewritten load failed: .../libstdc++.so.6.0.33: undefined symbol: fesetround
foreign: rewritten load failed: .../libLLVM.so.20.1: libc.musl-x86_64.so.1: cannot open...
```

The last line is the cascade. `libLLVM` needed `libxml2` and `libstdc++`, which
had just failed, so `ld.so` fell back to loading the unrewritten originals,
which still carry the musl `DT_NEEDED`.

**Fix:** load every glibc library that can hold a re-homed name into the
**global** scope at startup: `libm.so.6`, `libresolv.so.2`, `libcrypt.so.1`,
plus glibc's own pre-2.34 split libraries. `fgn_global_scope_libs[]` in
`src/foreign-dlopen.c`.

### 3.2 Bundled libraries were losing to host libraries

`foreign-dlopen.c` skips the dependency probe entirely for musl guests. That
part is correct: loading the host copy unstripped would drag musl libc into the
process. But the skip went straight to `fgn_find_candidate()`, which only
searches directories on the active load stack. For a host object that is
`/usr/lib`, so **a bundled soname could never win**.

Measured on Alpine: the AppDir bundles `libstdc++.so.6.0.36` and
`libgcc_s.so.1`, and the host's `libstdc++.so.6.0.33` and `libgcc_s.so.1` were
loading alongside them. Two libstdc++ and two unwinders in one process is the
classic "every symbol resolves and nothing works" configuration.

**Fix:** check `$APPDIR/lib/<soname>` **before** hunting the host, for musl
guests too. Loading the bundled copy is always safe, because it is a glibc
object built against the runtime already running. After the fix:

```
T4.2 -- provenance of collision-surface sonames
    libstdc++.so.6     /w/AppDir/lib/libstdc++.so.6      BUNDLED (correct)
    libgcc_s.so.1      /w/AppDir/lib/libgcc_s.so.1       BUNDLED (correct)
    libxcb.so.1        /w/AppDir/lib/libxcb.so.1         BUNDLED (correct)
```

### 3.3 `dlerror()` was being consumed

The fallback path reads `dlerror()` unconditionally and only prints it under
debug. `dlerror()` is destructive, so with debug off, which is the default, the
caller's own `dlerror()` returns `NULL`:

```
FAILED: dlopen: (null)
```

The comment above that code says it "surfaces the classic error message users
know how to read". It does the opposite.

**Fix:** read `dlerror()` only when tracing is on, so the message survives for
the caller in the normal case.

---

## 4. Design R: host-runtime selection

`src/runtime-select.c`. The forward-compatible half. If the host glibc is newer
and the set is complete, re-exec under the **host's** runtime, so a symbol
invented after the AppImage shipped resolves because the process is using the
future libc itself.

### 4.1 Two things the obvious implementation gets wrong

**A flat `--library-path "$HOST_LIBDIR:$APPDIR/lib"` breaks the bundling
guarantee.** It hands the host `libstdc++`, `libX11` and every other soname the
win too, in the same way section 3.2 did. Instead a **symlink farm** under
`$XDG_RUNTIME_DIR` holds the runtime set and nothing else:

```
--library-path  $FARM : $APPDIR/lib : $HOST_LIBDIRS
                ^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^^^
                libc    everything    fallback for
                only    bundled       what we lack
```

Symlinks, so no host file is touched and every write lands under
`XDG_RUNTIME_DIR`.

**A `DT_VERNEED` completeness check cannot detect a mixed runtime set.** This is
the more important correction. The obvious check, whether each member's
`DT_VERNEED` falls inside what its peers define, catches the direction where a
*new* object needs a version an *old* peer lacks. It provably cannot catch the
reverse, because **glibc never retires a version name**: an old `libdl.so.2`
asks libc only for `GLIBC_2.2.5`, and every later glibc still defines it.
Version names alone declare the mixed set healthy. It segfaults.

What discriminates is the `GLIBC_PRIVATE` symbol surface, which is not stable
at all. Measured, glibc 2.31 to 2.41:

```
old libdl.so.2       imports _dl_sym, _dl_addr, _dl_catch_error, _dl_vsym,
                     __libc_dlopen_mode        -> 2.41 exports NONE of them
old libpthread.so.0  13 imports absent from 2.41, incl. __libc_pthread_init,
                     _dl_make_stack_executable
old librt.so.1       9 absent, incl. __pthread_barrier_init, __shm_directory
```

So the implemented check is a **symbol** check. Every strong undefined symbol of
every member must be defined by the libc and `ld.so` it will be paired with.
Weak imports (`_ITM_registerTMCloneTable`, `__gmon_start__`) are skipped: they
are absent from every libc ever built and resolve to 0 by design, so counting
them would make every set look mixed.

The static check is then **verified empirically** before being committed to.
`runtime-select` forks and re-execs itself under the candidate runtime,
exercising malloc, TLS, stdio and `dlopen`, which is where a mixed set actually
dies.

Two traps in that self-test, both measured:

- It must re-exec **this binary**, not `/bin/true`. Rocky 9's `/bin/true` is a
  51-byte shell script, and `ld.so` answers `file too short`, which looks
  exactly like a mixed set and is not. Re-execing our own binary is also the
  stronger question, since it was linked against the bundled glibc.
- `/proc/self/exe` is the wrong way to find ourselves. Inside an AppImage this
  program starts as `$APPDIR/lib/ld-linux... runtime-select`, and when a loader
  is invoked explicitly the kernel exec'd the **loader**, so `/proc/self/exe`
  names `ld-linux`. Re-execing that asks one dynamic linker to run another as a
  program; it exits 127, indistinguishable from a broken runtime. Every newer
  host reported a false `SELF-TEST FAILED` until this was fixed.

### 4.2 Measured decision on all eight distros

Run against a fake AppDir bundling glibc 2.31, so the newer hosts really are
newer. The real AppImage bundles 2.44 and picks `bundled` everywhere, which is
correct, and is why the probe is run both ways.

| Host | Host glibc | Decision | Reason logged |
|---|---|---|---|
| debian bullseye | 2.31 | **bundled** | not newer than bundled |
| ubuntu 20.04 | 2.31 | **bundled** | not newer than bundled |
| rocky 9 | 2.35 | **host** | newer, set internally consistent |
| debian trixie | 2.41 | **host** | newer, set internally consistent |
| fedora 44 | 2.43 | **host** | newer, set internally consistent |
| opensuse tumbleweed | 2.43 | **host** | newer, set internally consistent |
| arch | 2.44 | **host** | newer, set internally consistent |
| alpine 3.22 | musl | **bundled** | no host glibc, bundled plus shim is the only option |

Every `host` decision also passed the empirical self-test. `host` on every
newer glibc, `bundled` on older, equal and musl, never a mixed set, always with
a logged reason.

**E20 and E21 are the guard and its control.** A deliberately mixed set (2.41
`ld.so` and `libc`, 2.31 `libdl`, `libpthread`, `librt`, `libutil`, every member
present so "incomplete" cannot be the reason) is **refused**, while the same
glibc unmixed is **accepted**. Without the control, a selector that refused
everything would pass.

### 4.3 The trade

Switching to the host runtime **gives up the bundle-everything guarantee**. The
app then runs against an unaudited host libc. That is a real cost and it is the
user's call, which is why `ANYLINUX_RUNTIME=host|bundled|auto` exists and why
the decision and its reason are logged under `ANYLINUX_LIB_DEBUG=1`.

---

## 5. Design B: the generated shim

`tools/gen_forward_shim.py`. The **selection** is generated; the
**implementations** come from an audited table. A generator that invented
semantics would be worse than the treadmill it replaces, not better. Solo splits
it the same way.

### 5.1 The floor, and what it means for this AppImage

The shipped `src/forward-shim.c` targets the demo AppImage's own bundled
runtime, **glibc 2.44**.

That is the headline finding of the ground-truth phase. The demo AppImage
bundles the newest released glibc, so **no distro in the matrix is newer**, the
selector correctly picks `bundled` on all of them, and the version gap is empty:

```
floor  : appdir-bundled glibc 2.44 (4287 symbols)
target : glibc-2.44             (4288 symbols)
musl   : 46 symbols musl exports and the floor does not
gap    : 47 symbols the floor lacks
   implementable    13
   stub-only        22
   irrelevant       12
```

The single non-musl gap symbol is `__libanl_version_placeholder`, an empty ABI
placeholder. **Case 1 is already solved for this artifact by bundling a
new-enough glibc.** It is not solved in general: any AppImage built on an older
distro has the gap, and this one acquires it the day glibc 2.45 ships.

So the generator is demonstrated at a realistic older floor as well. Floor 2.31,
target 2.44:

```
gap    : 628 symbols the floor lacks
   implementable   107
   stub-only       296
   irrelevant      225
```

Compiled with `-Wall -Wextra -Werror` on real glibc 2.31, with **42 documented
behaviours checked** (`tests/shim-selftest.c`, case E16), not just "it links":

```
  ok   strlcpy trunc          ok   stat matches __xstat     ok   bit_ceil(0)==1
  ok   strlcat trunc          ok   arc4random_uniform covers range
  ok   clz(0)==32             ok   _dl_find_object==-1      ok   sigabbrev_np(SIGKILL)
  ... 42 checks ...
SHIM TEST PASSED (0 failures)
```

### 5.2 What happens when an uncovered symbol appears

It fails loudly, naming the symbol, at the earliest point it can.

**At load time**, the dry-run and report path enumerates every strong undefined
symbol that neither the process nor the object's own dependency closure can
supply, and prints all of them, not just `ld.so`'s first.

**At call time**, a stub-only symbol aborts with its own name:

```
[foreign-dlopen] FATAL: sinpi: not implementable over this glibc
[foreign-dlopen] the bundled glibc 2.44 does not provide this symbol
[foreign-dlopen] and no implementation exists for it. Set ANYLINUX_RUNTIME=host
[foreign-dlopen] to run against the host's own libc, which will have it.
```

**Why emit stubs at all.** Every Mesa object is `DF_BIND_NOW`, so `ld.so`
resolves the whole symbol table at load. One undefined symbol makes the library
unloadable even if that code path is never taken. A stub converts "cannot load
at all" into "works unless it genuinely needs this".

**Why C23 maths is stub-only, deliberately.** `sinpi`, `fmaximum_num`,
`roundeven` and the other 186 have exacting NaN, signed-zero and rounding-mode
semantics. An approximation that is subtly wrong is worse than a loud abort, and
no GPU driver calls them. This is a recorded decision, not an oversight: the
manifest carries a per-symbol reason for all 47.

### 5.3 The musl-only surface is larger than the Mesa closure suggested

`gap.py` measures the union over the Mesa and LLVM closure as exactly
`['___environ', 'atexit']`, and that reproduces. But over the **whole** Alpine
`/usr/lib`, one more musl-only symbol is load-bearing:

```
foreign: rewritten load failed: .../libX11.so.6.4.0: undefined symbol: issetugid
```

`issetugid` alone was blocking `libX11.so.6` and `libdbus-1.so.3`. It is
implementable exactly as musl implements it, over `getauxval(AT_SECURE)`.

The generator now takes `--musl <inventory>` and folds musl's 46 floor-absent
exports into the same enumerable gap, rather than relying on a hand-maintained
list. That is what took the corpus from 243/247 to **247/247**.

### 5.4 The `___environ` rename

Applied, and confirmed firing on the real `libLLVM.so.20.1`:

```
foreign: ___environ -> __environ (st_name +1, no .dynstr write)
```

musl spells the environ pointer with three underscores, glibc with two. The
reference is a **weak** import, so it does not stop the load: it silently
resolves to 0 and the driver reads a NULL environment. Latent, and exactly the
class of bug that works until it does not.

The fix costs no string edits. `"___environ" + 1` **is** `"__environ"`, so
advancing `st_name` by one byte renames the reference. Two properties make this
total rather than merely likely, and both are checked:

- the symbol is **undefined**, so `DT_GNU_HASH`, which indexes only *defined*
  symbols from `symoffset` onward, does not cover it. No hash fixup.
- nothing is written to `.dynstr`, so tail-merging cannot bite. Tail-merging is
  real: 16 of 647 names in `libvulkan_lvp.so` are suffixes of another.

The general case, renaming to something that is not a suffix, needs an in-place
`.dynstr` write. `fgn_dynstr_range_occupied()` refuses unless it can prove that
no other referenced offset (symbol name, `DT_NEEDED`, `SONAME`, `RPATH`,
`RUNPATH`, or a version-table name) falls inside the clobbered range. T0.7 tests
that it does refuse.

---

## 6. Goal 2: what works, and exactly where it stops

### 6.1 What works

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

T2.4 is the result that separates a fix from a demo. Every one of the 247 musl
libraries in Alpine's `/usr/lib` loads into the glibc process, up from 2, with
zero regressions. Through the bundled Vulkan loader, `vkCreateInstance` against
the host ICD returns `VK_SUCCESS`.

### 6.2 Where it stops: T3.2 fails

```
[Vulkan Loader] ERROR: setup_loader_term_phys_devs: Call to
  'vkEnumeratePhysicalDevices' in ICD /tmp/xdg/.anylinux-fgn-dbdb70ee.so
  failed with error code VK_ERROR_OUT_OF_HOST_MEMORY
vkEnumeratePhysicalDevices reported zero accessible devices.
```

`vkcube` does not render on Alpine. This is the "loads but misbehaves" rung:
symbol availability is necessary but not sufficient.

Ruled out by measurement, not by reasoning:

| Hypothesis | Test | Result |
|---|---|---|
| Missing symbols | dry-run over the whole ICD closure | **zero** unresolvable strong imports |
| A real allocation failure | interposed `malloc`, `calloc`, `realloc`, `posix_memalign`, `aligned_alloc` | **0 NULL returns**, so the error code is a stand-in |
| The `___environ` rename | A/B with `ANYLINUX_LIB_FOREIGN_NORENAME=1` | byte-identical failure, exonerated |
| Duplicate `libstdc++` and `libgcc_s` | provenance check | was real, fixed in 3.2, failure persists |
| `issetugid` | added to the shim | corpus 243 to 247, failure persists |
| Display or WSI, not libc | run under `xvfb-run -a`, the error is a device error | not the cause |
| Host driver broken | `vulkaninfo --summary` natively on Alpine | lavapipe healthy |

Narrowed to this: both paths are **byte-identical in `strace` up to and
including the two `sysinfo` calls** in lavapipe's device init. The working musl
path then continues to `/proc/meminfo`; the glibc path makes **no further
syscalls at all** and returns the error. The divergence is a pure userspace
decision inside Mesa, after the memory queries and before any allocation.

Progress is real and measurable: with the **stock upstream** preload the same
probe **segfaults**. With this work it reaches `VK_SUCCESS` on instance creation
and returns a clean, diagnosable error. That is a strict improvement, but it is
not the goal, and it is reported as a failure.

`glxgears` (T3.3) was not separately diagnosed. It shares the ICD and DRI
loading path and the same blocker.

Next steps are in [CONTINUE.md](CONTINUE.md) section 4.

---

## 7. Test results

Tests are grouped by what they need to run. Tier 0 is static analysis on any
OS. Tier 1 is the evidence table. Tier 2 needs a real driver but no GPU. Tier 3
is end to end. Tier 4 checks invariants. Tier 5 needs hardware.

### Tier 0, static

| ID | Test | Result |
|---|---|---|
| T0.1 | musl gap is exactly two symbols | **PASS.** `['___environ', 'atexit']` |
| T0.2 | binding-mode audit | **PASS.** Every closure member `BIND_NOW` |
| T0.3 | TLS audit | **PASS.** No `DF_STATIC_TLS`, every `PT_TLS memsz` under 4 KiB (max 56 bytes) |
| T0.4 | rewriter round-trip | **PASS.** All four version tags gone together, size unchanged, re-parses |
| T0.5 | idempotence | **PASS.** Second strip byte-identical, content-hash name stable |
| T0.6 | `atexit` interposition safety | **PASS.** `nm -D` shows exactly one exported definition, checked by the Makefile |
| T0.7 | tail-merge guard | **PASS.** Refuses a range containing another live reference, allows a clear one |
| T0.8 | malformed-input fuzz | **PASS.** Every truncation and bit flip refused or bounded |

T0.4, T0.5, T0.7 and T0.8 run against the **real implementation**:
`tests/elf-selftest.c` includes `foreign-dlopen.c` rather than modelling it,
because a Tier-0 test that models the C can pass while the shipped code is
wrong.

### Tier 1, the evidence table

`experiments/run.ps1` reports **22/22 predictions held**. E1 through E13 are
unchanged. E14 through E21 are new, one per fix: the ELF self-test, the
generated-shim compile, the generated-shim behaviour, and five selector
decisions including the mixed-set guard and its control.

### Tier 2, 3 and 4

| ID | Test | Result |
|---|---|---|
| T2.1 | Alpine native lavapipe baseline | **PASS**, named by `vulkaninfo` |
| T2.2 | foreign-load the real ICD | **PASS** |
| T2.3 | rewritten once, cached, no musl libc load | **PASS** |
| T2.4 | corpus, zero regressions | **PASS.** 2/247 to 247/247, 0 regressions |
| T2.5 | selector across the distro matrix | **PASS** |
| T2.6 | forced `ANYLINUX_RUNTIME=bundled` on a newer host | **PASS** (E19) |
| T2.7 | cache-only library found via `--library-path` | **PASS** (E13c) |
| T3.1 | Alpine baseline fails before the fix | **PASS with a caveat**, below |
| T3.2 | `vkcube` with the host driver | **FAIL**, section 6 |
| T3.3 | `glxgears` | **FAIL**, same blocker |
| T3.4 | driver provenance is the host's | **PASS**, below |
| T4.1 | exactly one libc family | **PASS.** glibc yes, musl no |
| T4.2 | bundled wins, via `dladdr` | **PASS** |
| T4.3 | no host file modified | **PASS.** Identical sha256 over `/usr/lib`, `/lib`, `/etc/ld.so.conf.d` |
| T4.4 | no regression on glibc hosts | **PASS**, below |

**T3.1 caveat.** Its condition is "fails with a *symbol-resolution* error, not a
display error". At the AppImage level, under `xvfb-run -a`, the message is
`vkEnumeratePhysicalDevices reported zero accessible devices`, a device error,
because the Vulkan loader swallows an ICD that fails to load and reports only
the absence. The symbol-resolution error is real but one layer down, visible
directly at T2.2 and in the trace:

```
FAILED: dlopen: libc.musl-x86_64.so.1: cannot open shared object file
```

The baseline does fail for the right reason, but the criterion as written is
only satisfied by looking below the loader. Counting it as a clean pass on the
AppImage message alone would be wrong.

**T3.4 detail.** The mapped driver is
`$XDG_RUNTIME_DIR/.anylinux-fgn-dbdb70ee.so`, not a path under `$APPDIR`, so the
bundled-software-rendering trap is avoided. That file is the rewritten copy of
the host's driver, which the Vulkan loader itself confirms:

```
[Vulkan Loader] DEBUG | DRIVER: Searching for ICD drivers named /usr/lib/libvulkan_lvp.so
[Vulkan Loader] WARNING | LAYER: Path to given binary /usr/lib/libvulkan_lvp.so
                was found to differ from OS loaded path /tmp/xdg/.anylinux-fgn-dbdb70ee.so
```

The indirection is inherent: the whole mechanism is loading a *rewritten* copy,
so provenance has to be established through the rewrite, not by the mapped path.

**T4.4 detail.** The AppImage run on three glibc hosts with the **stock
upstream** preload and with the patched one, in both modes. The outcome is
identical in all twelve combinations, which is what "unchanged" means:

```
### Arch Linux (glibc 2.44)      ### Ubuntu 20.04 LTS      ### Debian trixie (2.41)
    stock    mode=0  rc=1            stock    mode=0  rc=1     stock    mode=0  rc=1
    stock    mode=1  rc=1            stock    mode=1  rc=1     stock    mode=1  rc=1
    patched  mode=0  rc=1            patched  mode=0  rc=1     patched  mode=0  rc=1
    patched  mode=1  rc=1            patched  mode=1  rc=1     patched  mode=1  rc=1
```

`rc=1` everywhere because these containers have no GPU, no display and no Vulkan
driver installed. The AppImage fails the same way before and after. The point of
the test is the equality, not the exit code.

### Skipped, with the specific missing capability

```
T1.3  SKIPPED - allocator-crossing microtest not written. Nearest evidence:
      the interposed-allocator probe in section 6 recorded 0 NULL returns
      across a full ICD load, and T2.4 loads 247 objects without a crash.
      Cross-libc malloc/free ownership transfer remains UNVERIFIED.

T1.4  SKIPPED - errno-coherence microtest not written. UNVERIFIED.

T1.5  SKIPPED - FILE* crossing microtest not written. This one matters:
      glibc's FILE is 216 bytes, musl's is opaque, and it is on the hazard
      list. UNVERIFIED.

T1.6  SKIPPED - pthread_mutex_t and cond_t sharing under TSan not written.
      UNVERIFIED.

T1.7  SKIPPED - Solo's dev/abi_probe.c was not ported, so the glibc-vs-musl
      struct-size divergences (regmatch_t 8 vs 16, rusage 144 vs 272,
      sched_param 4 vs 48, ucontext_t 968 vs 936, the FTW_* constants,
      O_LARGEFILE) were NOT proven unused by the closure. This is the single
      most likely home of the T3.2 failure and is the first thing to do next.
      See CONTINUE.md section 4.

T4.5  SKIPPED - 100 load/unload cycles and a 60 s vkcube run were not
      performed, because vkcube does not render (T3.2). RSS stability, fd
      leaks and .anylinux-fgn-* accumulation are UNVERIFIED. Partial
      evidence: T2.3 confirms each object is rewritten exactly once and
      cached, and T4.3 confirms the runtime dir is cleaned.

T5.1  SKIPPED - no discrete GPU in the test environment.
      Nearest evidence: T2.2 and T2.4 pass with lavapipe (software Vulkan),
      exercising the identical dlopen path. Hardware-specific failures
      such as libdrm ioctl ABI remain unverified.

T5.2  SKIPPED - no NVIDIA hardware.

T5.3  SKIPPED - no aarch64 hardware. The code is arch-parameterised
      (RS_LDSO, RS_TRIPLET, the syscall number fallbacks) but this is
      UNVERIFIED outside x86-64.
```

---

## 8. Measured versus assumed

**Measured:** every table and quoted output above, plus `experiments/run.ps1`
(22/22), `gap.py --fetch`, the eight-distro inventory, the AppImage inventory,
the corpus test, and the compiled-and-run sharun patch.

**Assumed or UNVERIFIED:**

- Everything in the skipped list above, most importantly T1.7.
- **The generated shim's stub-only symbols have never been called.** Their abort
  path is exercised by construction, not by a driver reaching it.
- **The `--host-dir` override and the symlink farm are tested in containers,
  not on a real desktop** where `XDG_RUNTIME_DIR` is a user-owned tmpfs. The
  permission model there is UNVERIFIED.
- **Design R has never run a GPU workload.** It selects correctly on eight
  distros and passes its self-test, but the end-to-end path where a newer host
  driver renders under the host runtime is UNVERIFIED, because the only
  end-to-end target available is the musl case, where Design R correctly
  declines to switch.
- A 32-bit or aarch64 build is UNVERIFIED.

---

## 9. Known unfixed and out of scope

**Case 3, a glibc-built host library loading into a musl process, is out of
scope and not addressed.** The packaging always bundles glibc deliberately,
because musl would lose the proprietary NVIDIA driver. Anyone who needs case 3
should use [pg83/solo](https://github.com/pg83/solo), which solves it with its
own ELF loader and a ~6000-line glibc-to-musl ABI bridge, and has CI across
Alpine, Fedora, NixOS and Termux plus a 2100-object corpus test per commit.

Also not delivered: NVIDIA's glibc-only userspace on a musl process, static musl
binaries with GPU access, bridging manylinux wheels into Alpine, and distroless
containers reaching host NSS or PAM.

Two designs were evaluated on paper and both rejected, with evidence, in
[`analysis/rejected-designs.md`](analysis/rejected-designs.md).
`dlmopen` into a private namespace is impossible (E9 measures it failing
identically to plain `dlopen`). A private ELF loader costs about 2700 lines,
still needs a shim, and buys isolation for a collision surface measured at three
sonames.

---

## 10. Residual risk

1. **T3.2 is unexplained.** A load that succeeds can still be semantically
   wrong, and here it is. The failure is bounded (section 6) but not diagnosed.
2. **The glibc-vs-musl hazard list is unexercised** (T1.7). `regmatch_t`,
   `rusage`, `sched_param`, `ucontext_t`, the `FTW_*` constants and
   `O_LARGEFILE` all differ, and nothing here proves the loaded closure avoids
   them. Given T3.2 fails inside a musl-built driver with no missing symbols,
   this is the most probable cause.
3. **Switching to the host runtime abandons the bundle-everything guarantee.**
   Real, deliberate, surfaced and overridable, but real.
4. **The generated shim is bounded by construction.** It covers what existed
   when it was generated. A symbol invented afterwards is the host-runtime
   switch's job, and on a musl host there is no host-runtime switch, which is
   why the musl row of the decision matrix has no escape hatch. Its
   forward-compatibility risk is small, because musl's exported surface grows
   slowly and glibc is very nearly a superset, but it is not zero.
5. **`at_quick_exit` returns failure rather than registering a handler.**
   glibc's real one runs handlers on `quick_exit()` only; approximating it with
   `__cxa_atexit` would run them on normal exit too, which is worse. Callers
   that ignore the return value will silently not get their handler.
6. **The stale-shim hazard.** If the bundled glibc is upgraded without
   regenerating `forward-shim.c`, the shim would interpose over symbols libc now
   provides. The manifest records the floor and `make shim` regenerates, but
   nothing enforces regeneration at build time.
