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

| Completion criterion | Status |
|---|---|
| Both goals demonstrated by a test that fails before and passes after | **Yes.** Goal 1: E5, E12. Goal 2: E22/E23 for the mechanism, E30/E32 and E37a/E37 for the end-to-end |
| The evidence harness still reports all predictions held | **Yes, 31/31**, up from 22/22, with 9 new cases. The AppImage suite adds 12 on a glibc host and 11 on a musl host |
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
two hosts and reports **12/12 on glibc** and **11/11 with one skip on musl**.
It fetches the demo AppImage once (sha256 verified), extracts it in a container
because the payload is DwarFS, builds `src/` on the glibc 2.31 floor, and then
measures E30 through E39 on each host. Every case is run with the feature off
and on, and against both the shipped `foreign-dlopen.so` and the one built from
`src/`, because a one-sided result cannot tell a working fix from a fallback
that was already happening.

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
      O_LARGEFILE) are still NOT proven unused by the closure. This was
      previously called "the single most likely home of the T3.2 failure".
      It was not: 6.2 found T3.2 elsewhere and fixed it, and vkcube now
      renders with those divergences untested. They remain a real hazard for
      code paths this workload does not reach, so this stays SKIPPED rather
      than being quietly dropped -- but it is no longer the top of the list.

T3.3  SKIPPED on Alpine - glxgears cannot run there for a reason that is not
      libc: Alpine's mesa-gl is classic Mesa, not libglvnd, so no
      libGLX_<vendor>.so.0 exists for the AppImage's bundled libglvnd to
      dlopen. PASSES on a glibc host with libglvnd (E38,
      GL_RENDERER = llvmpipe). No loader shim can supply a file the
      distribution does not ship.

T5.1  SKIPPED - no DRM render node, which is NOT the same as no GPU. The
      machine has a discrete NVIDIA GeForce RTX 3050 Ti Laptop (driver
      580.97, 4096 MiB) and an Intel Iris Xe, and the NVIDIA one is live
      from Linux:

          $ /usr/lib/wsl/lib/nvidia-smi -L      # inside a container
          GPU 0: NVIDIA GeForce RTX 3050 Ti Laptop GPU (UUID: GPU-df849629-...)

      What is absent is /dev/dri. WSL2 exposes GPUs through /dev/dxg
      paravirtualisation and publishes no DRM render nodes at all, so radv,
      anv and radeonsi cannot initialise however much silicon is present.
      Debian's mesa-vulkan-drivers ships libvulkan_intel.so and
      libvulkan_radeon.so in these containers; neither can open a device.
      Hardware-specific failures such as libdrm ioctl ABI stay UNVERIFIED.
      Nearest evidence remains T2.2 and T2.4 on lavapipe, which exercise the
      identical dlopen path.

T5.2  SKIPPED for the graphics stack, NEWLY POSSIBLE for the compute stack.
      The NVIDIA userspace here is the WSL flavour: /usr/lib/wsl/lib holds
      libcuda.so.1, libnvidia-ml.so.1, libnvoptix, libnvwgf2umx and
      Microsoft's libd3d12/libdxcore. There is no libGLX_nvidia.so.0, no
      nvidia_icd.json and no /dev/nvidia*, so the proprietary GL/Vulkan
      driver cannot be tested. libcuda.so.1 CAN be: it is a real
      closed-source glibc-built host driver library, max requirement
      GLIBC_2.2.5, with libdl.so.2 and libpthread.so.0 as separate
      DT_NEEDEDs -- the pre-2.34 layout, which is E6/E7's re-homing case.
      Every host driver measured so far has been open-source Mesa, so this
      would be the first proprietary one. Written up as a task in
      CONTINUE.md 4.3.

T5.3  SKIPPED - no aarch64 hardware. This machine is x86_64 (i7-12700H).
      The code is arch-parameterised (RS_LDSO, RS_TRIPLET, the syscall
      number fallbacks) but this is UNVERIFIED outside x86-64.
```

---

## 8. Measured versus assumed

**Measured:** every table and quoted output above, plus `experiments/run.ps1`
(31/31), `experiments/appimage.ps1` (12/12 glibc, 11/11 musl with one named
skip), `gap.py --fetch`, the eight-distro inventory, the AppImage inventory,
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

1. **The version-trap set is per-libc and computed, not universal.**
   `version-compat.c` covers what `tools/version_traps.py` finds in the libc it
   is audited against. A glibc that adds a trap after this was built is caught
   by `make traps` (E26) only if someone runs it. The audit is a build target,
   not an automatic gate, and nothing regenerates it on a bundled-glibc bump.
   Same class as risk 6.
2. **The glibc-vs-musl hazard list is still unexercised** (T1.7). `regmatch_t`,
   `rusage`, `sched_param`, `ucontext_t`, the `FTW_*` constants and
   `O_LARGEFILE` all differ, and nothing here proves the loaded closure avoids
   them. This was previously called the most probable cause of T3.2; it was not,
   and vkcube now renders with every one of them untested. That makes the list
   *less* urgent and no less real: a code path this workload does not reach can
   still hit any of them.
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
8. **On a musl host, "the feature off" is not a safe fallback.** Measured under
   the demo AppImage's own AppRun on Alpine: with `ANYLINUX_LIB_FOREIGN_DLOPEN=0`
   and a search path that reaches `/lib`, the bundled glibc `ld.so` finds
   `libc.musl-x86_64.so.1`, loads it, and the process ends up with **two libc
   families initialised** (`calling init:` names both). It renders, which is
   worse than failing, because rule 3 of the design says exactly one libc family
   may ever be in a process and E8/E9 measure why. With the feature on, only
   glibc is initialised (E35). This is upstream behaviour, not something this
   work introduced, and it is not fixed here -- it is recorded because "it
   worked with the feature off" is not the reassurance it looks like.
