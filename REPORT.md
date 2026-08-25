# REPORT

What was built, what was measured, and what is still broken.

Every claim is either backed by a command whose output is quoted, or labelled
**UNVERIFIED**. Nothing is estimated.

---

## 1. Summary

| Goal | Status |
|---|---|
| A host GPU driver built against a **newer glibc** loads into a process carrying an older bundled glibc | **Achieved.** Two mechanisms: the generated shim (E5) and the host-runtime switch (E12, no shim at all). The selector picks correctly on 8 of 8 distros |
| A **musl-built** host driver loads into that same glibc process **and renders** | **Achieved.** On Alpine 3.22, the demo AppImage's bundled glibc 2.44 drives Alpine's musl-built lavapipe: `vkEnumeratePhysicalDevices` returns one device and `vkcube` renders (E32, E37). Exactly one libc family is mapped (E35). 60 s of continuous rendering with RSS, fds and threads flat. See section 6 |
| A **closed-source** host driver does the same, on real silicon | **Achieved, and it never needed the fix.** NVIDIA's `libcuda.so.1` loads under the bundled glibc on Alpine and round-trips 4096 bytes through an RTX 3050 Ti (E41) -- and so does the control, because a vendor ships against a `GLIBC_2.2.5` floor on purpose. What the vendor stack DID need is uniform version binding (E43a/E43). Section 7.1 |
| Rendering on an actual GPU rather than a software rasteriser | **Achieved for OpenGL.** `GL_RENDERER = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)` at 101-121 FPS through the AppImage with no file changed (E53), via Mesa's d3d12 Gallium driver, which needs no DRM render node. Vulkan is still lavapipe: `dzn` is not packaged. Section 7.5 |
| The cross-libc ABI microtests, T1.3-T1.7 | **Written and passing.** 26 crossings hold with a musl guest; of the six struct hazards, two are live and named and four are benign, measured rather than assumed. Section 7.4 |

| Completion criterion | Status |
|---|---|
| Both goals demonstrated by a test that fails before and passes after | **Yes.** Goal 1: E5, E12. Goal 2: E22/E23 for the mechanism, E30/E32 and E37a/E37 for the end-to-end |
| The evidence harness still reports all predictions held | **Yes, 31/31**, up from 22/22, with 9 new cases. The AppImage suite adds 31 on a glibc host and 25 on a musl host, with every unrunnable case SKIPPED by the capability it lacks |
| No host file modified, verified by checksum | **Yes.** T4.3, identical sha256 before and after |
| Bundled libraries still win, verified via `dladdr` | **Yes.** T4.2, all resolved under `$APPDIR` |
| A forward-compatibility story that does not depend on foresight | **Yes.** Host-runtime selection for the unenumerable gap, a generated shim for the enumerable one, and a build-time audit (E26) for the version traps |
| A report separating measured from assumed | this document |

The one thing this report previously got wrong is worth stating plainly, because
it was the central claim: **the rendering failure was blamed on glibc-vs-musl
ABI differences, and it was not that.** Removing an object's symbol version
requirements is by itself enough to break it, on one libc, with no musl and no
Vulkan anywhere in the process. Section 6.2 is the measurement.

---

## 2. Environment reached

**Highest tier reached: Tier 5**, on hardware, for OpenGL and for compute. All
Tier 4 invariants run. Tier 3 end-to-end runs under `xvfb` with software Vulkan,
and Vulkan is the one path with no hardware result: Mesa's Vulkan-on-D3D12
driver is not packaged (section 7.5).

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

Software rendering is used for every Vulkan result and is named in each one:
Mesa **lavapipe** and **llvmpipe** (LLVM 20.1.8, 256 bits), pinned with
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` so a half-working
host GPU could not silently take over.

Two GPUs are reachable and are used where a case says so: an NVIDIA GeForce
RTX 3050 Ti Laptop and an Intel Iris Xe, both through `/dev/dxg`
paravirtualisation with the vendor userspace bind-mounted from
`/usr/lib/wsl`. There is no `/dev/dri` on this machine at all, so `radv`, `anv`
and `radeonsi` cannot initialise; `d3d12` and CUDA do not need one. Cases that
require the device are SKIPPED with that capability named on a machine without
it, and the suite still passes.

The host driver is sane natively, so every downstream result is interpretable:

```
$ vulkaninfo --summary        # Alpine 3.22, native musl
        deviceName         = llvmpipe (LLVM 20.1.8, 256 bits)
        driverName         = llvmpipe
```

---

## 3. Six defects found by measurement

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

### 3.4 Everything was being rewritten, whether or not it needed to be

`fgn_scan_providers()` built its idea of "versions we can satisfy" from
`dlsym("malloc")` -> `dladdr` -> parse that one file. So it only ever learned
**libc's** version names. Every `GLIBCXX_*`, `CXXABI_*` and `LLVM_*` requirement
in a Mesa closure was therefore unvouchable, `fgn_requirements_satisfied()`
returned 0 for all of them, and objects that needed nothing were rewritten
anyway. Reported independently in issue #1, from a Gentoo host whose glibc is
*older* than the bundled one, where the debug line says it outright:

```
foreign: our libc provides 46 known versions
```

A `DT_VERNEED` record names a **file** and the versions wanted **from it**, so
that is the question to ask: resolve the file (bundled copy first, then whatever
is already loaded under that soname) and look in *its* `DT_VERDEF`.

The check also had to move. It ran before the dependency closure was walked, and
half the files a `DT_VERNEED` names are the object's own dependencies, none of
them loaded yet — so the precise version of the question would have answered
"absent" for every one and stripped everything regardless.

Measured on `debian:trixie-slim`, host glibc 2.41 under a bundled 2.44:

| | objects rewritten | `/tmp` copies | result |
|---|---|---|---|
| as shipped | 6 | 6 | `enumerate -> -1` |
| after 3.4 | **0** | **0** | 1 device, llvmpipe |

Zero is the right answer there, and it also silences the Vulkan loader's
"path to given binary differs from OS loaded path" warning, because there is no
longer a rewritten copy for it to notice. On Alpine 5 objects are still
rewritten, which is unavoidable: they are musl-built. **E39** pins the count,
because a fix that merely stopped mattering would pass every other case.

### 3.5 The failure report accused the wrong thing

When a `DT_NEEDED` cannot be opened, every symbol it would have provided looks
unresolved. The report listed them and ended with:

```
Most likely the bundled glibc predates them. ANYLINUX_RUNTIME=host
runs against the host's own libc, which will have them.
```

under 258 LLVM entry points. No libc has ever exported any of them, and
`ANYLINUX_RUNTIME=host` cannot help. Found in issue #1 on a host that keeps
LLVM in `/usr/lib/llvm/22/lib64`, reachable only through `/etc/ld.so.cache`,
which a bundled `ld.so` patched to a private cache path does not read.

**Fix:** record which dependencies could not be opened and name them; offer the
glibc guess only when at least one unresolved symbol is shaped like something a
libc could own — not `_Z`-mangled, not `LLVM*`. **E28**.

### 3.6 The failure report was itself destructive

Found while testing 3.5, and the same class of bug as 3.3 reached from the other
side. `fgn_report_unresolved()` probes with `dlsym`, and **every probe that
misses replaces the pending `dlerror()` message**. The caller, about to ask for
it, was handed

```
/work/foreign-dlopen.so: undefined symbol: _ZN4llvm9Attribute16getWithAlignmentEv
```

— this object blamed for a failure in a different one — instead of ld.so's
actual `libvendor.so.1: cannot open shared object file`. The code carries a
comment saying it makes no `dlerror()` call, which was true and not enough.

**Fix:** re-run the load after the report, which puts the real message back.
One extra failed `dlopen`, only in a trace run. **E29**.

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

## 6. Goal 2: what works, and how the last blocker fell

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

### 6.2 T3.2, solved: an unversioned reference does not get the default definition

This was the open failure:

```
[Vulkan Loader] ERROR: setup_loader_term_phys_devs: Call to
  'vkEnumeratePhysicalDevices' in ICD /tmp/xdg/.anylinux-fgn-dbdb70ee.so
  failed with error code VK_ERROR_OUT_OF_HOST_MEMORY
vkEnumeratePhysicalDevices reported zero accessible devices.
```

It was attributed to glibc-vs-musl ABI differences. **It is not that**, and the
first measurement that mattered was reproducing it with no musl in sight.

#### The reproduction that broke it open

`debian:trixie-slim`, one libc, glibc 2.41 throughout. Debian's own
`libvulkan_lvp.so` is a glibc object. The only change from the working case is
that foreign-dlopen intercepts the load, and the only reason it intercepts is
that the ICD manifest was given an absolute `library_path` (Debian ships a bare
soname, which foreign-dlopen deliberately never touches, which is why nobody had
seen this on Debian):

```
=== feature OFF ===  deviceName = llvmpipe (LLVM 19.1.7, 256 bits)
=== feature ON  ===  WARNING: [lvp_device.c:1315] Code 0 : VK_ERROR_OUT_OF_HOST_MEMORY
                     ERROR: setup_loader_term_phys_devs ... VK_ERROR_OUT_OF_HOST_MEMORY
```

Same libc on both sides. So the ABI hypothesis is dead, and the mechanism is
the rewriting itself.

#### The chain, one measurement per link

Debian ships Mesa's `__FILE__` strings, so the failure names its own line.

| Link | How | Result |
|---|---|---|
| which Mesa call fails | the message itself | `lvp_device.c:1315` = `lvp_init_wsi()` |
| which WSI backend | gdb `FinishBreakpoint` on each `wsi_*_init_wsi` | `wsi_display_init_wsi` -> `VK_ERROR_OUT_OF_HOST_MEMORY`; x11 and wayland both `VK_SUCCESS` |
| which line inside it | gdb, stepping by line, both runs side by side | diverges at `wsi_common_display.c:2323`, `u_cnd_monotonic_init()` returns `thrd_error` where the working run returns 0 |
| which libc call | breakpoints on the three calls that inlines to | `pthread_condattr_init` 0, `pthread_condattr_setclock` 0, **`pthread_cond_init` 22 = `EINVAL`** |
| *which* `pthread_cond_init` | `info symbol $pc` at the breakpoint | failing run enters libc+`0x909f0`, working run libc+`0x91b00` |

```
$ nm -D /lib/x86_64-linux-gnu/libc.so.6 | grep -w pthread_cond_init
00000000000909f0 T pthread_cond_init@GLIBC_2.2.5     <- the failing run lands here
0000000000091b00 T pthread_cond_init@@GLIBC_2.3.2    <- the working run lands here
```

`pthread_cond_init@GLIBC_2.2.5` is `__pthread_cond_init_2_0`, kept for binaries
from before the 2003 condition-variable ABI change. Its entire body is
`if (cond_attr != NULL) return EINVAL;`. Mesa always passes an attribute,
because a monotonic clock is the only reason to build one.

#### Stated as a property of libc alone

E22 and E22b, no Vulkan, no musl, no AppImage — one small object, built once,
then version-stripped exactly the way a host driver is:

```
  E22    versions stripped   probe_cond_init() = 22
  E22b   versions intact     probe_cond_init() = 0
```

**An unversioned reference does not get the default definition of a symbol.**
A stripped object has only unversioned references. So does every musl-built
object, which never had version information to begin with — which is why the
same failure appeared on Alpine and on Gentoo with a glibc driver, and why it
looked like an ABI problem for as long as it did.

#### The fix

[`src/version-compat.c`](src/version-compat.c) defines the trapped names itself.
The preload is ahead of libc in the global lookup scope, so every unversioned
reference in the process lands there, and each definition forwards to the
default one.

Finding "the default one" is the part that needed care. `dlsym` is not an
answer: measured in **E27**, `dlsym(RTLD_NEXT, "pthread_cond_init")` returns the
**obsolete** definition on glibc 2.31 and the default one on 2.41. So the
version name is read out of the defining object's own `.gnu.version_d` — the
entry whose versym lacks the hidden bit — and handed to `dlvsym`, which is
correct on both. No version string is hardcoded anywhere.

Which names to cover is not a judgement call either.
[`tools/version_traps.py`](tools/version_traps.py) computes the set from a libc:
a name defined at two or more versions whose `st_value` **differs**. Same
address at several versions is re-versioning, not an ABI change — the glibc 2.34
libpthread merge does that to 191 symbols and none of them can matter.

```
glibc 2.42  multi-version, same address (harmless): 191    different address (traps): 38
glibc 2.41  multi-version, same address (harmless): 191    different address (traps): 33
glibc 2.31  multi-version, same address (harmless):  10    different address (traps): 21
```

`make traps` fails the build if a libc has a trap `version-compat.c` neither
forwards nor explicitly declines, so a future glibc cannot introduce one
silently (**E26**).

That is not a hypothetical. Run against glibc **2.42** (Arch, Fedora 44) rather
than the 2.31 and 2.41 this was developed on, the audit failed with five
uncovered names:

```
cfgetispeed  cfgetospeed  cfsetispeed  cfsetospeed  cfsetspeed
                                       default=GLIBC_2.42  others=GLIBC_2.2.5
```

glibc 2.42 added arbitrary terminal baud rates, so the `GLIBC_2.2.5`
definitions speak the old `Bnnn`-encoded `speed_t` and the new ones take a real
number of bits per second. Nothing in a graphics driver closure calls them,
which is the point: the set grew under a glibc newer than any this was tested
on, and an audit that enumerates rather than reasons is what noticed. Now
covered; audited clean on glibc 2.31, 2.41, 2.42 (Arch) and 2.42 (Fedora 44). Three are declined on purpose, with reasons: `memcpy`
(both definitions satisfy the memcpy contract, checked byte-for-byte over 4096
size and alignment combinations in **E25**; interposing every memcpy in a
rendering process to fix nothing is not a trade worth making) and
`sys_nerr`/`_sys_nerr` (data objects, which a forwarder cannot alias, and from
glibc 2.32 neither has a default version at all, so an unversioned reference
fails loudly instead of quietly).

#### End to end

`alpine:3.22`, musl host, the demo AppImage bundling glibc 2.44, forced onto
Alpine's own musl-built lavapipe:

| | as shipped | feature off | **this repo, feature on** |
|---|---|---|---|
| `vkprobe` | segfault | `VK_ERROR_INCOMPATIBLE_DRIVER` | **1 device, llvmpipe** |
| host `/usr/lib` loadable | — | 2 / 177 | **177 / 177** |
| libc families mapped | — | — | **one** |
| `vkcube` | `reported zero accessible devices` | — | **`Selected GPU 0: llvmpipe (LLVM 20.1.8)`** |
| 100 load/unload cycles | — | — | **rss +68 kB, fds +0, copies +0** |
| 60 s continuous render | — | — | **rss/fds/threads flat at 6 s, 33 s, 60 s** |

The `feature off` column is why the rest of the table means anything: the same
command with the same binaries cannot use the host driver at all.

And the same thing with nothing forced at all -- **E40**, one file replaced
inside the AppDir, no `ANYLINUX_*`, no `VK_DRIVER_FILES`, the marker the AppDir
already carries turning the feature on by itself:

```
as shipped   Do you have a compatible Vulkan installable client driver (ICD) installed?
with this    Selected GPU 0: llvmpipe (LLVM 20.1.8, 256 bits)
```

#### What this also fixed

The same defect ran the other way on a **glibc** host. Reported independently in
[issue #1](https://github.com/Azathothas/dlopen-experiment/issues/1) on Gentoo
with a real `radv`, and reproduced here on `debian:trixie-slim`, whose glibc
2.41 is **older** than the bundled 2.44 — so by construction nothing can be
missing and nothing needs rewriting:

| | vkcube |
|---|---|
| as shipped, feature on | `vkEnumeratePhysicalDevices reported zero accessible devices` |
| feature off | renders |
| this repo, feature on | renders |

Turning the feature on used to destroy a working configuration. See 3.4 for the
second half of that, which is that it should not have been rewriting anything
there in the first place.

#### One claim retracted

While reviewing this I asserted, in the issue thread, that lavapipe "has no
libdrm on the path at all". That is false and is corrected there. Alpine's
`libvulkan_lvp.so` links `libdrm.so.2` directly and references 35 drm symbols.
What is true, and measured:

```
$ readelf -d /usr/lib/libvulkan_lvp.so | grep -c libdrm_amdgpu
0
LD_DEBUG=libs, filtered to `calling init:`   ->   /w/AppDir/lib/libdrm.so.2
```

`libdrm` is on the path and the **bundled** copy is the one loaded, which is 3.2
working rather than libdrm being absent. `libdrm_amdgpu` -- the one that reads
`amdgpu.ids` through `AMDGPU_ASIC_ID_TABLE_PATHS` -- genuinely is not involved,
because lavapipe never references it. The conclusion held; the reason given for
it did not.

#### `glxgears`, the OpenGL path

Runs on a glibc host (**E38**, `GL_RENDERER = llvmpipe`). **SKIPPED on Alpine**,
with the specific missing capability: Alpine's `mesa-gl` is classic Mesa, not
libglvnd, so there is no `libGLX_<vendor>.so.0` for the AppImage's bundled
libglvnd to `dlopen`. That is host packaging, not libc, and no loader shim can
supply a file the distribution does not ship.

---

## 7. The closed-source driver, the ABI, and real silicon

Three things this report carried as UNVERIFIED for its whole life are measured
here: a **proprietary** host driver, the **cross-libc ABI** microtests T1.3-T1.7,
and rendering on an actual **GPU**. Cases E41-E53 in
[`experiments/40-appimage.sh`](experiments/40-appimage.sh); the suite reports
them on both hosts and SKIPS them by name where the capability is absent.

The headline is not the one the task predicted, so it goes first.

### 7.1 A proprietary driver is the least likely library to need this fix

The target is NVIDIA's WSL CUDA userspace, reachable through `/dev/dxg`. It is
the one class of host library nothing else here covers: a vendor binary that
cannot be inspected, cannot be rebuilt, and was linked against a libc nobody in
this project chose.

It loads, and it works:

```
E41   handle          : 0x55557363f2d0
      provenance      : /usr/lib/wsl/lib/libcuda.so.1
      cuInit          : ok
      driver version  : 13.0
      devices         : 1
      device[0]       : NVIDIA GeForce RTX 3050 Ti Laptop GPU
      device memory   : 4095 MiB
      OK: 1 CUDA device(s), 4096 bytes round-tripped through the GPU and verified
```

The round trip is deliberate. A handle proves only that `ld.so` was satisfied;
`cuMemcpyHtoD` and `cuMemcpyDtoH` with a byte-for-byte compare exercise ioctls
on the device node, the vendor's own threading and its allocator, under a libc
runtime the vendor never saw, and none of them fails.

**And every control passes too.** E41b runs the identical command with
`ANYLINUX_LIB_FOREIGN_DLOPEN=0`; E41c runs it with **no preload in the process
at all**, so neither this repository's shim nor upstream's nor the version-trap
forwarders are present; E43a runs the shipped one. All four get the same result.
That is not a defect in the test, it is the answer:

```
$ objdump -T /usr/lib/wsl/lib/libcuda.so.1 | grep -o 'GLIBC_[0-9.]*' | sort -uV
GLIBC_2.2.5
```

A vendor ships against the oldest floor it can, precisely so its blob runs on
everything. Nothing in it can be missing from a bundled glibc 2.44, so the shim
has nothing to do, and E42 measures that directly: **0 objects rewritten, 3
examined and left unchanged** -- the E39 rule arriving from a real vendor binary
instead of a synthetic probe. The claim E41/E41b support is therefore the
*regression* claim: turning the feature on does not break a driver that already
worked.

E46 puts the vendor's own binary on the end of it. `nvidia-smi` is NVIDIA's, not
ours; it `dlopen`s `libnvidia-ml.so.1` itself, and under the AppImage's bundled
glibc on **Alpine** it reports the GPU. E46a is its control, and on a musl host
it is unambiguous: the same binary run without the AppImage's runtime does not
execute at all. The precise reason is worth stating, because the obvious phrasing
is wrong -- musl's `ld.so` is never asked. The binary's `PT_INTERP` names
`/lib64/ld-linux-x86-64.so.2`, Alpine has no such file, and the kernel fails the
`execve` with ENOENT before any loader runs. E46a requires that message NOT to be
a shared-library one, so the case cannot pass on E44's failure by mistake.

```
E46    GPU 0: NVIDIA GeForce RTX 3050 Ti Laptop GPU (UUID: GPU-df849629-...)
E46a   env: can't execute '/usr/lib/wsl/lib/nvidia-smi': No such file or directory
```

### 7.2 What the vendor stack did need: two condvar ABIs in one process

Section 6.2 established the version-binding trap from libc alone. The CUDA stack
is the first place it has been caught in **shipping third-party software**, and
it is caught by reading the answer out of the running process rather than
inferring it. [`tests/bindprobe.c`](tests/bindprobe.c) walks each loaded object's
relocations, reads the address the loader put in the slot, and names the file and
symbol version behind it. `LD_DEBUG=bindings` cannot do this: it prints the
version a reference *asked for*, and for this trap the whole point is that the
reference asks for nothing.

Microsoft's `libdxcore.so`, which `libcuda.so.1` loads to reach `/dev/dxg`,
carries no symbol versioning at all:

```
$ readelf -V /usr/lib/wsl/lib/libdxcore.so | grep -c 'Version needs'
0
$ readelf -V /usr/lib/wsl/lib/libd3d12.so  | grep -c 'Version needs'
0
$ readelf -V /usr/lib/wsl/lib/libcuda.so.1 | grep -c 'Version needs'
1
```

So it imports every libc symbol unversioned -- structurally identical to a
musl-built object, and to an object this project's own rewriter has stripped.
`libd3d12.so` is the same shape and is in the *graphics* stack rather than this
one (section 7.5); it is shown here only because two independent vendor blobs
being built this way is the point.

[`tools/trap_users.py`](tools/trap_users.py) intersects an object's imports with
the traps of the libc it will resolve against:

```
$ python3 tools/trap_users.py $APPDIR/lib/libc.so.6 /usr/lib/wsl/lib/libdxcore.so
libc .../libc.so.6: 38 trap(s), 191 benign re-versioning(s)

== libdxcore.so
   imports              : 140
   trapped names among them: 6
   symbol versioning    : ABSENT, so every one of those references is unversioned
                          and binds the OBSOLETE definition
     memcpy                     default=GLIBC_2.14   obsolete=GLIBC_2.2.5
     pthread_cond_broadcast     default=GLIBC_2.3.2  obsolete=GLIBC_2.2.5
     pthread_cond_destroy       default=GLIBC_2.3.2  obsolete=GLIBC_2.2.5
     pthread_cond_signal        default=GLIBC_2.3.2  obsolete=GLIBC_2.2.5
     pthread_cond_timedwait     default=GLIBC_2.3.2  obsolete=GLIBC_2.2.5
     pthread_cond_wait          default=GLIBC_2.3.2  obsolete=GLIBC_2.2.5
```

NVIDIA's own `libnvidia-ml.so.1` names ten trapped symbols and is versioned, so
every one of them binds correctly. Which object is at risk is decided by how it
was linked, not by who wrote it.

The consequence, measured with the AppImage exactly as it ships (E43a):

```
  pthread_cond_wait
      libdxcore.so                 -> libc.so.6 @GLIBC_2.2.5
      libcuda.so.1.1               -> libc.so.6 @GLIBC_2.3.2
      VERDICT: MIXED (2 implementations)

  BINDINGS MIXED: 6 symbol(s) measured, 5 MIXED
```

One process, one driver stack, two different implementations of five condition
variable entry points -- and the two differ in how they read the first eight
bytes of a `pthread_cond_t`, which is the 2003 ABI change section 6.2 is about.
With this repository's preload the same measurement is (E43):

```
      libdxcore.so                 -> foreign-dlopen.so+0x6c80
      libcuda.so.1.1               -> foreign-dlopen.so+0x6c80
      VERDICT: uniform

  BINDINGS UNIFORM: 6 symbol(s) measured, 0 MIXED
```

**What this does not claim.** The mixed binding is latent, not currently fatal:
`cuInit` returns 0 and the GPU round trip succeeds in both states. Whether a
`pthread_cond_t` ever crosses the `libdxcore`/`libcuda` boundary is not visible
from outside two closed-source blobs. If one ever does, the two sides read it
two different ways. The fix removes the question rather than answering it.

### 7.3 One blind spot, three sightings

The `/etc/ld.so.cache` item has been an open design question since the first
pass. It now has a symptom, from a real driver, three times over.

WSL makes its GPU userspace reachable by writing a file:

```
$ cat //wsl$/podman-machine-default/etc/ld.so.conf.d/ld.wsl.conf   # a real WSL distro
# This file was automatically generated by WSL. To stop automatic generation of this file, add the following entry to /etc/wsl.conf:
# [automount]
# ldconfig = false
/usr/lib/wsl/lib
```

Nothing else names that directory. Anylinux patches `ld-linux.so` to skip the
cache (E13b), so `--library-path` is the only discovery mechanism left, and
whatever assembles it decides what exists.

**Sighting one, compute (E44).** `libcuda.so.1` opens by absolute path and loads
fine. Inside `cuInit` it `dlopen`s `libdxcore.so` by **bare soname**, which
`foreign-dlopen` deliberately never intercepts, so it reaches `ld.so` and is not
found. The error the user sees is not "cannot open shared object file":

```
FAILED: cuInit -> 100          # CUDA_ERROR_NO_DEVICE
```

E45 appends the directories `/etc/ld.so.conf` names and the same command
completes the GPU round trip. The conf file is plain text, so reading it gets the
benefit of the cache without touching the binary cache whose parsing is why the
cache was inhibited.

**Sighting two, Design R.** `runtime-select` assembles its own
`--library-path` from a hardcoded `rs_host_libdirs[]`, which has the same blind
spot. This one was found by reading the list rather than by a failure, so it was
measured afterwards, building the file from the commit before the change and
after it against the same driver:

```
=== runtime-select, before the conf-dirs change ===
   directories on the path: 8
   /usr/lib/wsl/lib present: NO
   FAILED: cuInit -> 304 CUDA_ERROR_OPERATING_SYSTEM
=== runtime-select, after the conf-dirs change ===
   directories on the path: 10
   /usr/lib/wsl/lib present: yes
   OK: 1 CUDA device(s), 4096 bytes round-tripped through the GPU and verified
```

A **third** distinct symptom for one cause, and again not a missing library: 304
rather than E44's 100, because the process is running the host's glibc 2.41
rather than the bundled 2.44 and `libcuda` gives up at a different point. The fix
is `rs_conf_dirs()`, which reads `/etc/ld.so.conf`, follows its `include` globs
with recursion bounded by both depth and a total file budget, sorts each
directory's entries so the path is reproducible, and appends what it finds
**after** the hardcoded list, so bundled and host-runtime directories keep their
existing precedence. E52 is the after-state as a standing case; the before-state
above is a one-off, because keeping it would mean shipping a switch that exists
only to turn a bug back on.

**Sighting three, graphics (E53a).** The strongest one, because the symptom
implicates the wrong subsystem entirely. Mesa's `d3d12_dri.so` `dlopen`s
`libd3d12.so` by bare soname; sharun assembles the path; sharun's host-GPU
directory list is hardcoded and contains `/run/opengl-driver/lib` and
`/run/current-system/sw/lib` but not `/usr/lib/wsl/lib`. What the user sees:

```
Error: glXCreateContext failed
```

That reads as a display or driver fault. It is a missing directory. `LD_DEBUG=libs`
is what settles it:

```
897:  find library=libd3d12.so [0]; searching
897:    trying file=/w/AppDir/lib/libd3d12.so
897:    trying file=/usr/lib/x86_64-linux-gnu/libd3d12.so
897:    trying file=/run/opengl-driver/lib/libd3d12.so
897:    trying file=/run/current-system/sw/lib/libd3d12.so
                                               ... and never /usr/lib/wsl/lib
```

E53 hands sharun the conf-derived directories through its own
`SHARUN_FALLBACK_LIBRARY_PATH` -- no file edited, nothing patched -- and the
AppImage renders on the GPU. All three sightings are one computation, and it is
the one [`patches/sharun-library-path.patch`](patches/sharun-library-path.patch)
already implements.

### 7.4 The cross-libc ABI, measured

T1.3 through T1.7 were SKIPPED and UNVERIFIED for the whole project. They are
written now: [`tests/abi-guest.c`](tests/abi-guest.c) is one source file built
twice, by glibc on the floor and by musl on Alpine, and
[`tests/abi-host.c`](tests/abi-host.c) drives the crossings. The size table both
sides fill comes from one inline function in
[`tests/abi-abi.h`](tests/abi-abi.h) compiled into both, so the two columns can
differ only because the headers do.

The musl build is loaded **through `foreign-dlopen` itself**, which is what drops
its libc edge -- no `patchelf`, no stand-in. E48 is the control that fails:

```
E48   FAILED: dlopen: libc.musl-x86_64.so.1: cannot open shared object file
E49   ABI CROSSING PASSED: 26 checks, 0 failed
E47   ABI CROSSING PASSED: 27 checks, 0 failed        (same-libc control)
```

Every crossing holds. Memory allocated in the musl object is freed by the
process and the reverse; an `errno` set inside it is read outside it in the same
thread; a `FILE*` opened by the process is written from inside it and read back;
a mutex made on either side is locked from the other; and a condition variable
the process waits on is signalled from inside it. Both sides reach one `malloc`,
one `free`, one `__errno_location`, one `pthread_mutex_lock` and one `stdout`
FILE object.

**The size divergences are real and mostly harmless, and the report can finally
say which.** A size table alone cannot tell those apart, so the probe measures
offsets too:

```
    regmatch_t             guest=16       host=8        <-- DIVERGES
    struct rusage          guest=272      host=144      <-- DIVERGES
    struct sched_param     guest=48       host=4        <-- DIVERGES
    ucontext_t             guest=936      host=968      <-- DIVERGES
    FTW_D                  guest=2        host=1        <-- DIVERGES   (all seven)
    O_LARGEFILE            guest=32768    host=0        <-- DIVERGES
    sizeof regoff_t        guest=8        host=4        <-- DIVERGES
    off rusage.ru_maxrss   guest=32       host=32
    off rusage.ru_nivcsw   guest=136      host=136
    off stat.st_mode       guest=24       host=24
    off stat.st_size       guest=48       host=48
    off dirent.d_name      guest=19       host=19
    off sched.priority     guest=0        host=0
```

Every field the probe measures is at the same offset in both. `struct rusage`
differs by 128 bytes of trailing reserved space and `struct sched_param` by 44;
neither moves a field anybody reads.

Which leaves the direction that does break, and it is not the one the hazard list
implied. Handing the guest storage the host allocated is safe, because the guest
calls **glibc's** implementation, which writes glibc's layout into glibc-sized
memory: all four guard bands survive (T1.7b). The hazard is one step further on,
where the guest reads a glibc-filled struct back at its **own** offsets:

```
  T1.7c -- the guest reading back a struct glibc filled
    DIFF regexec, read back at own stride     host=7 guest=12884901888
         LIVE HAZARD: regoff_t is 4 bytes here and 8 there
    same getrusage, read back at own offset   host=6084 guest=6084
    DIFF nftw, dirs counted with own FTW_D    host=2 guest=0
         LIVE HAZARD: FTW_D is 1 here and 2 there
```

Nothing crashes. `regexec` reports a match ending at byte 7 and the musl-built
caller reads 12884901888 out of the same array; `nftw` walks a tree with two
directories in it and the musl-built caller counts none.

Six hazards were listed. They do not all end in the same place, and the
difference between measured and argued is worth keeping:

| Hazard | Verdict | On what basis |
|---|---|---|
| `regmatch_t` / `regoff_t` stride | **LIVE** | measured: host reads 7, guest reads 12884901888 from the same array |
| the seven `FTW_*` values | **LIVE** | measured: host counts 2 directories, guest counts 0 |
| `struct rusage` | benign | measured: sizes differ by 128 bytes of trailing reserve, `ru_maxrss` and `ru_nivcsw` are at the same offsets, and the guest reads the same value the host does |
| `struct sched_param` | benign | measured: `sched_priority` is at offset 0 in both, and the guard band around a host-allocated one survives the guest filling it |
| `ucontext_t` | **argued, not measured** | 936 vs 968 bytes, and nothing here calls `getcontext`/`swapcontext` across the boundary, so no crossing exists to measure. It would matter to a guest that made or swapped a context the process also touched |
| `O_LARGEFILE` | **argued, not measured** | 0 on glibc x86-64, `0100000` on musl. A guest passing musl's value to glibc's `open` sets the kernel flag glibc considers implied on 64-bit, which is a no-op there; that is a reading of the two headers, not a test |

E50 asserts the count of LIVE rows, so it fails if a future libc moves one.

No loader shim can fix the live two: an offset compiled into an object is not
something a preload can reach. They are a property of loading musl-built code
into a glibc process, and the honest statement is now four measured verdicts and
two arguments rather than a worry about six.

### 7.5 Hardware, at last, and the caveat that was wrong twice

"No GPU" was the standing caveat of this whole project. It was wrong the first
time (there are two GPUs, and the NVIDIA one is live from Linux) and wrong again
in its correction (`/dev/dri` is absent, so radv/anv/radeonsi are out -- but they
are not the only way to reach a GPU).

Mesa's **d3d12 Gallium driver** does not need a DRM render node. It talks to
`/dev/dxg` through Microsoft's `libdxcore`, and Debian packages it:
`/usr/lib/x86_64-linux-gnu/dri/d3d12_dri.so`. That makes the host's own OpenGL
driver a hardware driver, and the AppImage's bundled libglvnd finally has a real
vendor library to drive.

Rung 1 of the diagnostic ladder first, natively, with no AppImage involved:

```
$ GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
  xvfb-run -a -s '-screen 0 1024x768x24 +extension GLX +render' glxgears -info
GL_RENDERER   = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)
GL_VERSION    = 4.6 (Compatibility Profile) Mesa 25.0.7-2+deb13u1
GL_VENDOR     = Microsoft Corporation
579 frames in 5.0 seconds = 115.707 FPS
```

Then through the AppImage, which is 7.3's third sighting and its resolution:

```
E53a  Error: glXCreateContext failed                                (as it stands)
E53   GL_RENDERER   = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)  (+ conf dirs)
      606 frames in 5.0 seconds = 120.965 FPS
      507 frames in 5.0 seconds = 101.138 FPS      (the next interval)
```

**No file is modified and nothing is patched, but four environment variables
are set**: `SHARUN_FALLBACK_LIBRARY_PATH` with the directories `/etc/ld.so.conf`
names, and `GALLIUM_DRIVER`, `MESA_D3D12_DEFAULT_ADAPTER_NAME` and
`LIBGL_ALWAYS_SOFTWARE=0` to choose the hardware driver and which of the two
GPUs it drives. E40 remains the case that forces nothing; this one is not that
case and does not claim to be. The first of those four is the only one this
project is responsible for, and it is the one the sharun patch removes the need
for.

**E53b does not flip.** With the feature off the same command still renders, and
saying otherwise would be claiming a control that did not happen. The host GL
stack here is glibc-built against glibc 2.41, older than the bundled 2.44, so
there is nothing for the shim to do -- the same reason as E41b and E42. What E53
measures is that the path works on hardware, not that the shim made it work.

Vulkan stays on lavapipe. Mesa's Vulkan-on-D3D12 driver (`dzn`) is not packaged
by Debian, and building it is the one remaining route to a hardware-backed
**Vulkan** ICD.

### 7.6 Design R, with a device on the end

Design R selected correctly on eight distros and passed its self-test, and had
never had a driver on the end of the choice. E51 and E52 put one there. Read the
first three rows together: they run the **same** host Vulkan ICD and differ only
in how the process got a libc that can satisfy it.

| Case | Runtime | Feature | Driver | Result |
|---|---|---|---|---|
| E31 | bundled | off | host lavapipe | no devices |
| E32 | bundled | on | host lavapipe | 1 device (the shim half) |
| E51 | **host** | none at all | host lavapipe | 1 device (the Design R half) |
| E52 | **host** | none at all | NVIDIA `libcuda.so.1` | the round trip, on the RTX 3050 Ti |

The switch is forced with `ANYLINUX_RUNTIME=host`. Auto declines on this host
and is right to: the bundled glibc 2.44 is newer than the host's 2.41, so a
switch could only lose, and that is the rule E17 measures. What E51 and E52
measure is whether the switched runtime can drive a real device, not whether it
should have been chosen.

The two halves of the design are now each demonstrated end to end, on the same
host, against the same driver, and they remain independent: on a musl host only
the shim half exists, which is why the musl row of the matrix has no escape
hatch (risk 4).

---

## 8. Test results

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

`experiments/run.ps1` reports **31/31 predictions held**. E1 through E13
measure the problem. E14 through E21 are one per fix from the first pass: the
ELF self-test, the generated-shim compile and behaviour, and five selector
decisions including the mixed-set guard and its control. E22 through E29 are
the version-binding trap and the reporting defects:

| ID | What it pins |
|---|---|
| E22 | the bug, stated in libc alone: version-stripped object, `pthread_cond_init` returns `EINVAL` |
| E22b | its control: the same object unstripped returns 0, so the probe and the container are exonerated |
| E23 | the fix: the same stripped object with the preload merely present returns 0 |
| E24 | the obsolete definition really does reject the attribute Mesa passes |
| E25 | the `memcpy` exclusion is justified: 4096 size/alignment combinations, byte-identical |
| E27 | which resolution primitive may be trusted; `dlsym(RTLD_NEXT)` is not one |
| E26 | the audit: no glibc may add a trap `version-compat.c` neither forwards nor declines |
| E28 | the report names the dependency that failed to open, instead of accusing the libc |
| E29 | and the caller still gets ld.so's message, not one of the report's own `dlsym` misses |

### Tier 1b, the AppImage end-to-end suite

`experiments/appimage.ps1` runs a real AppImage against a real host driver on
two hosts and reports **31/31 on glibc** and **25/25 with six named skips on
musl**. It fetches the demo AppImage once (sha256 verified), extracts it in a
container because the payload is DwarFS, builds `src/` on the glibc 2.31 floor,
builds the musl half of the ABI probe on Alpine, and then measures E30 through
E53 on each host. Every case is run with the feature off and on, and against
both the shipped `foreign-dlopen.so` and the one built from `src/`, because a
one-sided result cannot tell a working fix from a fallback that was already
happening.

The six skips on Alpine are named rather than counted: no libglvnd vendor
library (E38), no host glibc runtime set to switch to (E51, E52), and no
Vulkan-or-GL-on-D3D12 driver (E53a, E53, E53b). On a machine with no GPU at all
the driver's own capability probe turns E41-E53 into skips as well, and the
suite still passes -- that is the point of probing rather than assuming.

### Tier 2, 3 and 4

| ID | Test | Result |
|---|---|---|
| T2.1 | Alpine native lavapipe baseline | **PASS**, named by `vulkaninfo` |
| T2.2 | foreign-load the real ICD | **PASS** |
| T2.3 | rewritten once, cached, no musl libc load | **PASS** |
| T2.4 | corpus, zero regressions | **PASS.** 2/247 to 247/247, 0 regressions. Re-measured by E33/E34 on a leaner Alpine image: 2/177 to 177/177. The denominator is however many `.so` files the image happens to have; the ratio is the result |
| T2.5 | selector across the distro matrix | **PASS** |
| T2.6 | forced `ANYLINUX_RUNTIME=bundled` on a newer host | **PASS** (E19) |
| T2.7 | cache-only library found via `--library-path` | **PASS** (E13c) |
| T3.1 | Alpine baseline fails before the fix | **PASS with a caveat**, below |
| T3.2 | `vkcube` with the host driver | **PASS.** `Selected GPU 0: llvmpipe (LLVM 20.1.8)` on Alpine, feature on; `reported zero accessible devices` as shipped. Section 6.2 |
| T3.3 | `glxgears` | **PASS** on a glibc host (E38). **SKIPPED** on Alpine: its Mesa is not libglvnd, see the skipped list |
| T3.4 | driver provenance is the host's | **PASS**, below |
| T4.1 | exactly one libc family | **PASS.** glibc mapped, musl not, with the feature on (E35) |
| T4.2 | bundled wins, via `dladdr` | **PASS** |
| T4.3 | no host file modified | **PASS.** Identical sha256 over `/usr/lib`, `/lib`, `/etc/ld.so.conf.d` |
| T4.4 | no regression on glibc hosts | **PASS**, below, and E30-E39 on `debian:trixie-slim` |
| T4.5 | 100 load/unload cycles, and 60 s of continuous rendering | **PASS.** Cycles: rss +68 kB, fds +0, rewritten images +0 over 99 steady-state cycles (E36). 60 s: rss 157656 kB, 5 fds, 48 threads, identical at t=6 s, 33 s and 60 s |

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

### Every test that was once skipped, and where it stands now

Five of these -- T1.3 through T1.7 -- were SKIPPED and UNVERIFIED for the life of
this project and are resolved here rather than quietly dropped. The other four
each still carry something unverified, and each says what would unblock it.

```
T1.3  PASS - allocator ownership crosses in both directions. Memory
      malloc'd inside a musl-built guest is freed by the process and the
      reverse; strdup likewise. Both sides reach one malloc and one free,
      named by dladdr. E49, section 7.4.

T1.4  PASS - one errno location. A failing open inside the musl guest sets
      ENOENT and the process reads 2 from its own errno in the same thread,
      before anything else can clobber it. E49.

T1.5  PASS - a FILE* opened by the process is written from inside the musl
      guest and read back byte for byte, and both sides carry the same
      stdout FILE object address. glibc's FILE is 216 bytes and musl's is
      neither, so only one of them can be right about the object; the
      measurement says which. E49.

T1.6  PASS - a mutex made by the process is locked and unlocked from the
      guest and left unlocked; a mutex the guest allocated with its own
      sizeof is locked by the process; and a condition variable the process
      waits on is signalled from the guest, bounded by a 5 s timeout so a
      broken binding fails rather than hangs. E49. Not run under TSan: that
      remains UNVERIFIED.

T1.7  PASS, with two live hazards named. The divergences are real --
      regmatch_t 16 vs 8, rusage 272 vs 144, sched_param 48 vs 4,
      ucontext_t 936 vs 968, all seven FTW_* off by one, O_LARGEFILE
      32768 vs 0 -- and mostly harmless, because every named FIELD is at the
      same offset in both. What is NOT harmless is a musl-built object
      reading back a struct glibc filled at its own stride: regexec reports
      a match ending at byte 7 and the guest reads 12884901888, and an nftw
      walk over two directories counts none. Those two cannot be fixed from
      a loader shim. E50 fails if the count of live hazards ever changes.
      Section 7.4.

T3.3  SKIPPED on Alpine - glxgears cannot run there for a reason that is not
      libc: Alpine's mesa-gl is classic Mesa, not libglvnd, so no
      libGLX_<vendor>.so.0 exists for the AppImage's bundled libglvnd to
      dlopen. PASSES on a glibc host with libglvnd, in software (E38,
      GL_RENDERER = llvmpipe) and on hardware (E53, GL_RENDERER = D3D12
      (NVIDIA GeForce RTX 3050 Ti Laptop GPU)). No loader shim can supply a
      file the distribution does not ship; unblocking this needs a musl
      distro that ships libglvnd.

T5.1  PARTIAL - no DRM render node, which is NOT the same as no GPU, which
      in turn is not the same as no hardware result. This machine has a
      discrete NVIDIA GeForce RTX 3050 Ti Laptop (driver 580.97) and an
      Intel Iris Xe, both live from Linux, and neither reachable through
      /dev/dri: WSL2 publishes no DRM render nodes at all, so radv, anv and
      radeonsi cannot initialise however much silicon is present.

      What does reach them is /dev/dxg. Mesa's d3d12 GALLIUM driver needs no
      DRM node and Debian packages it, so the OpenGL path runs on hardware
      (E53, GL_RENDERER = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU), 121
      FPS through the unmodified AppImage). NVIDIA's CUDA userspace reaches
      the same device for compute (E41, E52).

      Still UNVERIFIED: hardware VULKAN. Mesa's Vulkan-on-D3D12 driver
      (dzn) is microsoft-experimental and Debian does not package it, so
      every ICD result here is lavapipe. Hardware-specific failures in the
      DRM drivers -- libdrm ioctl ABI above all -- stay UNVERIFIED and need
      a non-WSL Linux host.

T5.2  PASS, and the result is not the one the task predicted. NVIDIA's
      libcuda.so.1 is a real closed-source glibc-built host driver, and it
      loads under the AppImage's bundled glibc 2.44 on Alpine and
      round-trips 4096 bytes through the GPU (E41). So does the control with
      the feature off, and upstream's shim, and no shim at all: the blob is
      built against a GLIBC_2.2.5 floor, so nothing in it can be missing and
      zero objects are rewritten (E42). A proprietary driver turns out to be
      the LEAST likely host library to need this fix.

      What the vendor stack did need is in section 7.2: Microsoft's
      libdxcore.so and libd3d12.so carry no symbol versioning at all, so as
      shipped the CUDA stack binds two different pthread_cond_* families in
      one process (E43a, 5 of 6 symbols MIXED). This repository's preload
      makes them one (E43). Latent rather than currently fatal; the limit of
      that claim is stated where it is made.

      Still UNVERIFIED: the proprietary GRAPHICS driver. /usr/lib/wsl/lib
      has no libGLX_nvidia.so.0, no nvidia_icd.json and no /dev/nvidia*, so
      the closed-source GL and Vulkan drivers cannot be tested here at all.

T5.3  SKIPPED - no aarch64 hardware. This machine is x86_64 (i7-12700H).
      The code is arch-parameterised (RS_LDSO, RS_TRIPLET, the syscall
      number fallbacks) but this is UNVERIFIED outside x86-64.
```

---

## 9. Measured versus assumed

**Measured:** every table and quoted output above, plus `experiments/run.ps1`
(31/31), `experiments/appimage.ps1` (31/31 glibc, 25/25 musl with six named
skips), `gap.py --fetch`, the eight-distro inventory, the AppImage inventory,
the corpus test, and the compiled-and-run sharun patch.

**Assumed or UNVERIFIED:**

- The three tests still skipped above: `glxgears` on a musl host (T3.3),
  hardware **Vulkan** and the DRM-native drivers (T5.1), the proprietary
  **graphics** driver (T5.2), and aarch64 (T5.3). Each names what would unblock
  it, and none of them is unblockable from this machine.
- **T1.6 was not run under a thread sanitiser.** The crossings are exercised and
  bounded by a timeout, which catches a broken binding; it does not catch a race
  that happens not to fire.
- **The generated shim's stub-only symbols have never been called.** Their abort
  path is exercised by construction, not by a driver reaching it.
- **The `--host-dir` override and the symlink farm are tested in containers,
  not on a real desktop** where `XDG_RUNTIME_DIR` is a user-owned tmpfs. The
  permission model there is UNVERIFIED.
- **Design R has never run a GPU workload under an AUTO decision.** It now
  drives both a Vulkan device and the CUDA round trip under the switched runtime
  (E51, E52, section 7.6), but the switch is forced with `ANYLINUX_RUNTIME=host`:
  no host here has a glibc newer than the bundled 2.44, so auto correctly
  declines every time. The path where auto chooses to switch AND a driver runs
  is still UNVERIFIED, and needs a host with a newer glibc than the bundle.
- A 32-bit or aarch64 build is UNVERIFIED.

---

## 10. Known unfixed and out of scope

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

## 11. Residual risk

1. **The version-trap set is per-libc and computed, not universal.**
   `version-compat.c` covers what `tools/version_traps.py` finds in the libc it
   is audited against. A glibc that adds a trap after this was built is caught
   by `make traps` (E26) only if someone runs it. The audit is a build target,
   not an automatic gate, and nothing regenerates it on a bundled-glibc bump.
   Same class as risk 6.
2. **Two of the glibc-vs-musl hazards are live, and no loader can fix them**
   (T1.7, section 7.4). The list is no longer six unknowns: every named field of
   every divergent struct sits at the same offset, so `rusage`, `sched_param`
   and `stat` cross harmlessly, and passing host-allocated storage to the guest
   is safe because glibc's implementation writes glibc's layout. What breaks is
   a musl-built object reading a glibc-filled struct back at its own stride --
   `regoff_t` is 4 bytes on glibc and 8 on musl -- and comparing against its own
   `FTW_*` values, which are off by one. Nothing here reaches either, and
   nothing here would notice if it did except E50, which is why E50 asserts the
   count rather than merely printing it. An offset compiled into an object is
   not reachable from a preload; the only real mitigations are not loading such
   an object or switching the whole runtime.
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
7. **The forwarders are process-wide.** A bundled library's own
   `pthread_cond_init@GLIBC_2.3.2` reference also lands in `version-compat.c`,
   because glibc lets an unversioned definition satisfy a versioned reference --
   that is how `LD_PRELOAD` interposition has always worked. It then forwards to
   the same default definition it would have reached directly, so behaviour is
   unchanged and the cost is one indirect call. The case this would get wrong is
   an object that genuinely wants an obsolete version: glibc 2.2.5-era condition
   variables, 2003 or earlier. Nothing that ships in an AppImage does, and
   nothing was found that does, but this is an assumption rather than a
   measurement.
8. **Library discovery, not `dlopen`, is what breaks a host driver most often
   here, and two of the three assemblers are still hardcoded lists.**
   `src/runtime-select.c` now derives its directories from `/etc/ld.so.conf`
   (section 7.3). Sharun does not yet -- the patch exists and is unapplied --
   and `foreign-dlopen.c` deliberately never will, because finding libraries is
   `ld.so`'s job. Until the patch lands, any host that puts a driver somewhere
   only the cache knows about will fail in a way that does not mention a
   library: `CUDA_ERROR_NO_DEVICE` (E44) or `glXCreateContext failed` (E53a).
   Both were measured on this machine, on drivers people actually use.
9. **On a musl host, "the feature off" is not a safe fallback.** Measured under
   the demo AppImage's own AppRun on Alpine: with `ANYLINUX_LIB_FOREIGN_DLOPEN=0`
   and a search path that reaches `/lib`, the bundled glibc `ld.so` finds
   `libc.musl-x86_64.so.1`, loads it, and the process ends up with **two libc
   families initialised** (`calling init:` names both). It renders, which is
   worse than failing, because rule 3 of the design says exactly one libc family
   may ever be in a process and E8/E9 measure why. With the feature on, only
   glibc is initialised (E35). This is upstream behaviour, not something this
   work introduced, and it is not fixed here -- it is recorded because "it
   worked with the feature off" is not the reassurance it looks like.
