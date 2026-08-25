# Cross-libc `dlopen`: load host shared libraries regardless of their libc

> **Task prompt for an autonomous engineering agent.** Read it end to end before running anything.
>
> - **[MEASURED]** — reproduced on real artifacts or in real containers. Re-run, don't re-derive.
> - **[UNVERIFIED]** — plausible, not yet tested. Yours to confirm or refute.
>
> **Primary goal is a working proof of concept with tests, not polish.** A narrow thing that
> demonstrably works beats a broad thing that demonstrably demos.

---

## 1. Mission

Make `dlopen()` work when the host library was built against a different — usually **newer** —
libc than the one the process is running. Do it without patching host files and without symbol
collisions.

Two concrete, falsifiable goals, in priority order:

1. **A host GPU driver built against a newer glibc loads into a process carrying an older
   bundled glibc.** This is the common case: it hits every user whose distro is newer than the
   AppImage's build host.
2. **A musl-built host driver (Alpine) loads into that same glibc process.**
   `vkcube` from `vkcube+glxgears-host-drivers-demo-x86_64.AppImage` must render on Alpine
   using Alpine's own Mesa.

### 1.1 What "done" means

1. Both goals demonstrated by a test that fails before the change and passes after.
2. `experiments/run.ps1` (or its Linux equivalent) still reports **all predictions held**.
3. No host file modified — verified by checksum (`T4.3`).
4. Bundled libraries still win over host libraries — verified via `dladdr` (`T4.2`).
5. A forward-compatibility story that does not depend on foresight: host-runtime
   selection for the unenumerable gap (§5.0), a **generated** shim for the enumerable one (§5.2).
6. `REPORT.md` separating *measured* from *assumed*.

---

## 2. The problem space — reprioritised

| # | Process libc | Host library libc | Priority | Status |
|---|---|---|---|---|
| **1** | **older glibc** | **newer glibc** | **PRIMARY** | Partly solved; breaks on genuinely-new symbols **[MEASURED]** |
| **2** | glibc | musl | **SECONDARY** | Broken; root cause identified **[MEASURED]** |
| 3 | musl | glibc | **OUT OF SCOPE** | Solved by [Solo](https://github.com/pg83/solo); we always ship glibc |
| 4 | musl | musl | n/a | Works natively |

Case 3 is explicitly **not** our problem: the packaging always bundles glibc, deliberately
([FAQ](https://github.com/Samueru-sama/Anylinux-AppImages/blob/main/FAQ.md): musl would lose the
proprietary NVIDIA driver). Do not spend effort there. Cite Solo if someone asks.

---

## 3. Evidence **[MEASURED]**

Everything here is reproducible with **one command**:

```powershell
.\experiments\run.ps1        # ~2 min; needs podman or docker
```

It orchestrates three throwaway containers over a shared volume — `alpine:3.22` builds a
faithful musl library, `debian:trixie-slim` (glibc 2.41) builds libraries needing new symbols,
`debian:bullseye-slim` (glibc 2.31) stands in for "an AppImage bundling an older glibc" — and
prints a MATCH/MISMATCH table. **Last run: 14/14 predictions held.**

### 3.1 The evidence table

| ID | Experiment | Result |
|---|---|---|
| **E1** | musl lib, musl `DT_NEEDED` dropped, into glibc | **FAILS** — `undefined symbol: atexit` |
| **E2** | same, with a 3-line `atexit` shim preloaded | **WORKS** — loads, runs, handler fires |
| **E3** | glibc-2.41-built lib into glibc 2.31, plain `dlopen` | **FAILS** — `` version `GLIBC_2.38' not found `` |
| **E4** | same, after **version stripping** (what `foreign-dlopen.c` does today) | **FAILS** — `undefined symbol: arc4random` |
| **E5** | same, stripped **+ forward-compat shim** | **WORKS** — `newlib_answer()=99` |
| **E6** | re-homed symbol (`pthread_create@GLIBC_2.34`), stripped, no `libpthread` loaded | **FAILS** — `undefined symbol: pthread_create` |
| **E7** | same, with `libpthread.so.0` actually in the process | **WORKS** — `thr_answer()=77` |
| **E8** | `dlopen` a newer `libc.so.6` under an older `ld.so` | **FAILS** — `` ld-linux: version `GLIBC_2.35' not found `` |
| **E9** | same via `dlmopen(LM_ID_NEWLM)` (private namespace) | **FAILS** — identical error |
| **E10** | exec-time switch to the host's **whole** runtime (`ld.so` + `libc`) | **WORKS** |
| **E11** | exec-time switch with a **mixed** old/new runtime | **SIGSEGV** (exit 139) |
| **E12** | the E3/E4 failing library, run under the host's **complete** runtime, **no shim** | **WORKS** — `newlib_answer()=99` |
| **E13a** | lib in `/usr/local/lib`, sharun-style `--library-path` **without** it, cache allowed | **WORKS** — found via `/etc/ld.so.cache` |
| **E13b** | same, `--inhibit-cache` (reproduces anylinux's patched `ld.so`) | **FAILS** — `cannot open shared object file` |
| **E13c** | same, cache inhibited but the directory **added to `--library-path`** | **WORKS** |

### 3.2 What each result means

**E4 is the important one.** Version stripping only removes a *version predicate*; it cannot
conjure a symbol that does not exist. Strip `GLIBC_2.38` off a reference to `arc4random` and you
get `undefined symbol: arc4random` instead. Today's approach therefore has **no forward-compatibility
story** for genuinely-new symbols. That is a real defect, not a theoretical one.

**E6/E7 show why stripping works as often as it does.** glibc 2.34 merged `libpthread`, `libdl`,
`librt`, `libutil`, `libanl` into `libc.so.6`. A modern build emits `pthread_create@GLIBC_2.34`
with **no `DT_NEEDED` on `libpthread`**. On an older host the symbol still exists — in
`libpthread.so.0` — so stripping succeeds **iff that library is already loaded in the process**
(E7). If it is not, stripping fails (E6). Actionable: the bundled runtime should keep the legacy
split libraries loaded.

**E8/E9 close off "just load the host libc too".** `libc.so.6` and `ld-linux.so` are **version-locked**:
```
new libc.so.6 requires from ld-linux : GLIBC_2.2.5, GLIBC_2.3, GLIBC_2.35, GLIBC_PRIVATE
old ld-linux  defines                : GLIBC_2.2.5, GLIBC_2.3, GLIBC_2.4
```
A second glibc cannot enter the process by `dlopen` **or** by `dlmopen` into a private namespace.
`ld.so` is mapped by the kernel at `execve` and owns TLS; it cannot be swapped mid-process.

**E10/E11/E12 identify the one mechanism that does get symbols from the host libc**: choose the
host's runtime at **exec** time, so there is still exactly one libc. **E12 is the load-bearing
result** — the very library that failed in E3 and E4 loads and runs with **no shim of any kind**,
because `arc4random` and `strlcpy` came from the host's own `libc.so.6`. But it only works if the
**entire matched set** is switched: mixing an old `libdl.so.2` with a new `libc.so.6` segfaults
instantly (E11).

**E13 shows library *discovery* is a separate failure mode from symbol resolution.** Anylinux
patches `ld-linux.so` to stop it reading `/etc/ld.so.cache` — documented in `HALL-OF-FAME.md`,
because reading it [segfaults instantly on some
systems](https://github.com/pkgforge-dev/Anylinux-AppImages/issues/766#issuecomment-5182230177).
`--inhibit-cache` reproduces that patched loader exactly. With the cache gone, **`--library-path`
is the only discovery mechanism there is**, and anything it omits is invisible — even though the
host's own loader would find it. `/usr/local/lib` is exactly such a directory. See §5.5.

### 3.3 How big is the forward-compat surface? **[MEASURED]**

Diffing glibc 2.31 (all split libs, 3 825 exports) against glibc 2.41 (libc+libm, 4 120):

| Bucket | Count |
|---|---|
| Genuinely new (absent from **every** 2.31 library) | **548** |
| …excluding `_thread_db_*` and `__*` internals | **414** |
| — C23 `stdc_*` bit utilities | 70 |
| — C23 / IEEE-754-2019 math (`sinpi`, `fmaximum*`, `log2p1`, …) | 186 |
| — NSS / resolver internals re-homed from `libnss_*`/`libresolv` | ~90 |
| — new syscall wrappers (`pidfd_*`, `fsopen`, `close_range`, `execveat`, …) | 21 |
| — BSD string/random (`strlcpy`, `strlcat`, `wcslcpy`, `wcslcat`, `arc4random*`) | 7 |

The subset a **GPU driver** can plausibly touch is small — on the order of 20–40 symbols. The
sharp ones:

- **`stat` / `fstat` / `lstat` / `fstatat` / `mknod`** — only became real exported symbols in
  glibc **2.33**. Before that only `__xstat`/`__fxstat` existed. Anything built on ≥2.33 imports
  `stat` directly. Trivial to shim: `stat(p,b)` → `__xstat(1,p,b)`.
- **`_dl_find_object`** (2.35) — used by modern unwinders; returning `-1` makes callers fall back
  to `dl_iterate_phdr`.
- **`__libc_single_threaded`** (2.32) — a data symbol libstdc++ reads; `0` is the safe value.
- `arc4random*`, `strlcpy/strlcat`, `close_range`, `mallinfo2`, `sigabbrev_np`, `strerrorname_np`.

### 3.4 The musl case (case 2) **[MEASURED]**

Static analysis over real Alpine v3.22 artifacts (`libvulkan_lvp.so`, `libgallium`, `libGL`,
`libLLVM.so.20.1`, `libstdc++`, `libgcc_s`), reproducible with `python3 gap.py --fetch`:

- Symbols the whole musl Mesa+LLVM closure needs that glibc **cannot** supply: exactly
  **`atexit`** (STRONG → fatal) and **`___environ`** (WEAK → resolves to 0, latent).
- `atexit`: glibc exports `__cxa_atexit`/`__cxa_finalize`/`on_exit` from `libc.so.6` but keeps
  `atexit` in the static `libc_nonshared.a`. musl exports it dynamically. Confirmed live in E1/E2.
- `___environ`: musl has three underscores; glibc has `__environ`. Fix without touching any
  string — `"___environ" + 1` **is** `"__environ"`, so increment that symbol's `st_name` by 1.
  Safe because `DT_GNU_HASH` indexes only *defined* symbols (measured on `libLLVM`:
  `symoffset = 419`, `___environ` at index 191 — below the boundary, so no hash fixup).
- **Every object is `DF_BIND_NOW`** → `ld.so` ignores `RTLD_LAZY`. You cannot defer a missing
  symbol; you must supply it.
- TLS is **not** a blocker here: no object sets `DF_STATIC_TLS`, and `PT_TLS` blocks are tiny.
- Alpine's `libstdc++` has **no `DT_VERDEF`**, so C++ symbols bind unversioned to a bundled
  glibc-built `libstdc++` without version errors.
- `.dynstr` tail-merging is real (16 of 647 names in `libvulkan_lvp.so` are suffixes of another),
  so **never overwrite a string in place** without proving no other offset falls inside the range.

---

## 4. Adjudicating the design objections

Four objections were raised against the v1 plan. Each is answered with evidence, not opinion.
**Two were right and changed the design.**

### 4.1 "Dropping musl symbols breaks forward compat — this is no better than dlopening and hoping"

**CORRECT, and it is the most important objection.** E4 proves it directly: strip the version and
you get `undefined symbol` instead. Any fix that enumerates today's missing symbols is a treadmill.

But note what it is *not*: `foreign-dlopen.c` drops the musl **`DT_NEEDED`**, not "musl symbols".
That part is forced — see §4.2. The defect is that nothing replaces what it drops.

**Design consequence:** the shim must be **generated from glibc's own symbol tables**, with a
version floor, not hand-written. Regenerating on a glibc bump is mechanical; hand-patching per
report is the treadmill being warned about. See §5.2.

**But a generated shim still has a hard ceiling — see §4.5, which is the more important half.**

### 4.2 / 4.3 "We still need to load the second libc to resolve symbols we don't have"

**The goal is right; the mechanism is impossible.** E8 and E9 are decisive: a newer `libc.so.6`
cannot load under an older `ld.so`, by `dlopen` or by `dlmopen` into a fresh namespace, because
`libc.so.6` imports `GLIBC_2.35` and `GLIBC_PRIVATE` **from `ld-linux`**. And `ld.so` cannot be
replaced mid-process — the kernel maps it at `execve` and it owns the thread pointer and TLS.

Neither Solo nor Detour does what the objection assumes:

- **Solo** deliberately does **not** load glibc. It maps host objects with its own ELF loader and
  resolves their glibc imports to adapters over musl (`glibc_shim.cpp`, 5 948 lines). It is a
  shim architecture — the very thing the objection wants to avoid.
- **Detour** does load a foreign `ld.so`, but only from a **libc-free** process
  (`-static -nostartfiles -nodefaultlibs -nostdlib`). That is not incidental: `ld.so` installs its
  own TCB on init, which would destroy a live glibc. An AppImage is a full glibc process, so
  Detour's trampoline is inapplicable.

### 4.4 "Ideally the missing symbol should come from the host libc, not a shim"

**Legitimate preference, achievable only two ways — both with costs.** Being precise matters here,
so state it exactly:

| Route | Works? | Cost |
|---|---|---|
| `dlopen`/`dlmopen` the host libc in-process | **No** — E8, E9 | Impossible, full stop |
| **Exec-time switch to the host's whole runtime** | **Yes** — E10 | All-or-nothing (E11 segfaults on a mixed set); glibc-only; abandons the "bundle everything" guarantee and runs the app against an unaudited host libc |
| **Private ELF loader** mapping host libc without `ld.so` | In principle | This is Solo, ~2 700 lines of loader — and it still needs a bridge, so it does not avoid shims either |
| **Generated compat shim** | Works, but **does not source from the host** — E5 | Reimplements the symbol locally; bounded to what exists today (§4.5) |

So **in-process, the shim is the only option** — it is not a hack chosen over something cleaner.
But out-of-process, the exec-time switch does exactly what the objection asks, and E12 proves it.

### 4.5 "How do you guarantee forward compat for a symbol that doesn't exist yet?"

*"What if the next glibc introduces `openkek` replacing `openat`, and drivers need it?"*

**This objection is correct and it defeats the shim-only design.** You cannot generate a shim for
a symbol nobody has invented. Any answer of the form "enumerate what's missing" has a ceiling at
the day you enumerated.

**The answer is that the shim is the wrong tool for that half of the problem.** Split the gap:

| Gap | Nature | Tool | Forward-compatible? |
|---|---|---|---|
| Symbols that exist today and we lack | enumerable | **generated shim** (E5) | Bounded — by construction |
| Symbols invented after we shipped | **unenumerable** | **exec-time host-runtime switch** (E12) | **Yes, by construction** |

E12 is the proof: the library that failed in E3 and E4 runs correctly under the host's complete
runtime with **no shim at all**, because `arc4random` and `strlcpy` came from the host's own
`libc.so.6`. A future `openkek` resolves for exactly the same reason — **you are using the future
libc itself, so there is nothing to predict.** Forward compatibility is obtained by not needing
foresight.

This vindicates the objection's instinct — *the missing symbols really should come from the host
libc* — while correcting only the mechanism: not `dlopen` (impossible, E8/E9) but `execve`.

**Required policy — implement this decision matrix, not a single strategy:**

| Situation | Strategy | Why |
|---|---|---|
| Host glibc **newer** than bundled, complete matched set present | **Switch to the host runtime** | Forward-compatible forever (E12). Must be the *whole* set (E11) |
| Host glibc **older** than bundled | Keep the bundled runtime | Old host libraries cannot require newer symbols; nothing to bridge |
| Host is **musl** (no host glibc to switch to) | Bundled glibc + shim | Unavoidable, but the surface is closed and tiny: 53 musl-only symbols, exactly 1 fatal (§3.4) |
| Switch impossible or set incomplete | Bundled + generated shim (E5), then **fail loudly naming the symbol** | Best effort with an actionable error instead of a mystery |

Two honest caveats to record in `REPORT.md`:

- Switching to the host runtime **gives up the "bundle everything" guarantee** — the app then runs
  against an unaudited host libc. That is a real trade, and it is the user's call, not yours.
- The musl row has no escape hatch, because a glibc-linked process cannot exec under musl's `ld.so`.
  Its forward-compat risk is real but small: musl's exported surface grows slowly and glibc is
  very nearly a superset of it.

---

## 5. Designs

Implement **R**, **A** and **B**. Evaluate **C**/**D** on paper.
**R is the primary path for case 1; A and B are the fallback and the case-2 path.**

### 5.0 Design R — host-runtime selection at exec time (primary, forward-compatible)

The only strategy that survives symbols invented after you ship (§4.5, E12).

A launcher decides, before `execve`, which runtime the process will use:

1. **Probe the host.** Read the host `ld.so`'s and `libc.so.6`'s version definitions
   (`elfsym.py` already extracts `DT_VERDEF`) and compare against the bundled ones.
2. **Verify completeness.** E11 is the trap: the set must be internally consistent. Require the
   host to provide `ld-linux-x86-64.so.2`, `libc.so.6`, `libm.so.6`, and — for older hosts — the
   legacy split libs, all from the same glibc. **Never mix.**
3. **Decide.**
   - host newer **and** complete → re-exec via the host loader:
     `"$HOST_LD" --library-path "$HOST_LIBDIR:$APPDIR/lib" "$APPDIR/bin/app" "$@"`
   - otherwise → keep the bundled runtime and fall through to A/B.
4. **Make it observable and overridable.** `ANYLINUX_RUNTIME=host|bundled|auto`, and log the
   decision plus the reason under `ANYLINUX_LIB_DEBUG=1`. A user hitting a bad host libc must be
   able to force `bundled` without rebuilding.

Notes:
- The maintainers already do a version of this in shell; the work here is making the
  **completeness check** rigorous (E11) and the decision auditable.
- Bundled libraries must still win for everything that is not libc — keep `$APPDIR/lib` ahead of
  the host directory in `--library-path` for non-runtime sonames (`T4.2` guards this).
- This trades away the "bundle everything" guarantee. Surface the trade; do not hide it.

### 5.1 Design A — compat shim inside the existing `LD_PRELOAD` (do first)

`foreign-dlopen.so` is already `LD_PRELOAD`ed, so it is already in the global lookup scope and its
exports satisfy undefined symbols in anything `dlopen`ed later. Proven by E2 and E5.

```c
/* musl exports atexit dynamically; glibc keeps it in the static libc_nonshared.a.
   Host objects load RTLD_NODELETE, so a handler can never outlive its object. */
extern int __cxa_atexit(void (*)(void *), void *, void *);
VISIBLE int atexit(void (*fn)(void)) {
        return __cxa_atexit((void (*)(void *))fn, NULL, NULL);
}
```

- Collision risk ≈ none: glibc-built objects resolve `atexit` from their own hidden
  `libc_nonshared.a` copy and never import it dynamically — verify with `T0.6`.
- The `void(*)(void)` → `void(*)(void*)` cast is ABI-safe on x86-64 SysV (callee ignores `%rdi`)
  but formally UB; a trampoline is cleaner.
- Build check: `nm -D foreign-dlopen.so | grep atexit` must show exactly one exported definition.

### 5.2 Design B — **generated** compatibility shim (covers the *enumerable* gap)

Answers §4.1's treadmill concern by generating rather than hand-writing. It does **not** deliver
forward compatibility — §4.5 explains why only Design R can, and why you need both. Scope this to
the gap you can enumerate today, and make anything outside it fail loudly.

1. **Inventory.** Extract `nm -D --defined-only` from the bundled `libc.so.6`/`libm.so.6` **and**
   from a set of newer glibc releases. `gap.py` already does exactly this diff; extend it to emit
   JSON. (Solo does the same thing — `dev/generate_glibc_stubs.py`, `lib/glibc_symbols_x86_64.json`.)
2. **Classify** each newer-only symbol:
   - **implementable** — a small correct implementation over the older glibc
     (`strlcpy`, `arc4random_buf` → `getrandom`, `close_range` → syscall, `stat` → `__xstat`);
   - **forwardable** — an alias of something already present;
   - **stub-only** — cannot be implemented; must abort with the exact symbol name if ever called
     (Solo's discipline; silent corruption is worse than a loud failure);
   - **irrelevant** — `_thread_db_*`, NSS internals a driver cannot reach.
3. **Generate** `forward-shim.c` from that classification, plus a manifest recording which
   glibc version floor it targets.
4. **Regenerate** when the bundled glibc changes. The treadmill is now a build step.

Also in Design B, for the musl case:

| From | To | Mechanism | Safety |
|---|---|---|---|
| `___environ` | `__environ` | `sym.st_name += 1` | **Total** — suffix identity, undefined symbol, no hash fixup (§3.4) |
| `atexit` | shim export | Design A | Total |
| generic `X`→`Y` where `Y` is a suffix of `X` | | `st_name += len(X)-len(Y)` | Total |
| generic `X`→`Y`, `len(Y) ≤ len(X)` | | in-place `.dynstr` write | **Conditional — must prove no other referenced offset falls inside the clobbered range** (`T0.7`) |

And: **keep the legacy split libraries loaded** (`libpthread.so.0`, `libdl.so.2`, `librt.so.1`,
`libutil.so.1`) in the bundled runtime, so E6-class failures become E7-class successes.

Plus a **dry-run mode** (`ANYLINUX_LIB_FOREIGN_DRYRUN=1`) reporting what would be rewritten and
which symbols would remain unresolvable — this makes `T0.x` testable with no GPU and no Alpine.

### 5.5 Design P — library search-path completeness (fix in **sharun**, not here)

**E13 is the evidence; the architectural call is that this does not belong in
`foreign-dlopen.c`.**

`foreign-dlopen.c` today has no `LD_LIBRARY_PATH` handling and no `/etc/ld.so.cache` parsing. Its
`fgn_find_candidate()` only probes directories already on the active load stack, and it recovers
paths by *scraping `dlerror()` text* for `"(required by /abs/path)"`. That is a fallback, not a
search algorithm.

**Do not fix that by teaching `foreign-dlopen.c` to search.** Its job is to *rewrite* an object
`ld.so` has already located; finding libraries is `ld.so`'s job, driven by `--library-path`, which
**sharun** assembles. Two search implementations would diverge, and the C one would be the buggy
one. Put the fix in one place: make `--library-path` complete.

**What sharun already does** (`src/main.rs` ≈1030–1066) — more than the objection assumes:

1. the AppDir's generated `lib.path` (a walk of the bundled lib dir);
2. **appends `$LD_LIBRARY_PATH`** — so that part is already handled;
3. prepends `$SHARUN_EXTRA_LIBRARY_PATH`, appends `$SHARUN_FALLBACK_LIBRARY_PATH`;
4. appends `/usr/lib:/lib`, then `/usr/lib64:/lib64` + `/usr/lib/x86_64-linux-gnu`
   (or the 32-bit / aarch64 variants);
5. appends NixOS's `/run/opengl-driver/lib:/run/current-system/sw/lib`.

**The gaps to close — measured across five distros:**

| Distro | `ld.so.conf` entries | `/usr/local/lib`? | `/usr/local/lib64`? |
|---|---|---|---|
| Alpine 3.22 | *(empty; musl)* | yes | no |
| Debian 13 | `/lib/x86_64-linux-gnu`, `/usr/lib/x86_64-linux-gnu`, `/usr/local/lib`, `/usr/local/lib/x86_64-linux-gnu` | yes | no |
| Fedora 44 | *(empty in base image)* | yes | yes |
| Arch | *(empty in base image)* | yes | no |
| openSUSE TW | `/usr/local/lib`, `/usr/local/lib64` | yes | yes |

1. **`/usr/local/lib`, `/usr/local/lib64`, `/usr/local/lib/<triplet>`** — present on **every**
   distro surveyed, in `ld.so.conf` on Debian and openSUSE, and **absent from sharun's list**.
   This is the concrete bug E13b demonstrates.
2. **Parse `/etc/ld.so.conf` and `/etc/ld.so.conf.d/*.conf`** (honour `include` globs). These are
   **plain text** — safe to read, and they are the distro's own authoritative answer. This gets the
   benefit of the cache without touching the binary cache that caused the segfault.
3. **musl hosts:** read `/etc/ld-musl-<arch>.path`; musl's built-in default is
   `/lib:/usr/local/lib:/usr/lib`.
4. **Other triplets:** `riscv64-linux-gnu`, `arm-linux-gnueabihf`, `powerpc64le-linux-gnu`,
   `s390x-linux-gnu` — currently only x86-64/i386/aarch64 are handled.
5. **Non-FHS prefixes:** Termux `/data/data/com.termux/files/usr/lib`, Flatpak `/app/lib`,
   Guix `/run/current-system/profile/lib`.
6. **`/usr/libexec`** — present on Debian/Fedora/openSUSE, occasionally holds libraries.

**Rules for the assembled path:**
- **Bundled first, host after — always.** A host directory must never precede `$APPDIR/lib`
  (`T4.2` guards this). Everything appended here is a *fallback*.
- **Deduplicate and drop non-existent directories.** Every entry costs a `stat` per lookup per
  miss; a bloated path is a measurable startup cost, not just untidiness.
- **Keep it inspectable.** Log the final path under `ANYLINUX_LIB_DEBUG=1`; "library not found"
  with no way to see the search path is the failure mode being fixed.

> **Permission boundary.** `sharun` is a **different repository** (`VHSgunzo/sharun`). Under §7.1
> you may **not** open issues or PRs there. Produce the patch plus its rationale and evidence in
> *this* repo, and hand it to the user to upstream.

### 5.3 Design C — `dlmopen(LM_ID_NEWLM)` — **rejected, with evidence**

E9 settles it: a private namespace does not escape the `ld.so`↔`libc` version lock. Independently
it would also breach the one-libc-per-process rule and split the heap across the Vulkan
loader↔ICD boundary. Record the verdict; do not spend time here.

### 5.4 Design D — private ELF loader (Solo, mirrored) — escape hatch

Only if A+B provably cannot work. Cost is Solo's feature list: relocations, TLS **and TLSDESC**,
IFUNC, RELRO, `dl_iterate_phdr`, versioned lookup, `AT_SECURE`, RPATH/RUNPATH. For our cases
`ld.so` already does all of that correctly, so this trades a working implementation for ~2 700
lines. It also still needs a bridge, so it does not avoid shims. Justify loudly or not at all.

---

## 6. Environment — verified on this machine

### 6.1 What is already working **[MEASURED]**

| Tool | Status |
|---|---|
| **podman 5.8.6** | Working. `podman.exe` is at `%LOCALAPPDATA%\Programs\Podman\podman.exe` — **not on `PATH`**; call it by full path or add it |
| podman machine | `podman-machine-default`, a **running WSL2 Fedora 44** VM |
| WSL2 | Available; kernel 7.2.0. Only distro is the podman machine — **do not touch it** |
| `gh` 2.97.0 | Authenticated as `Azathothas` |
| Python 3.13 | On `PATH` as `py -3` |
| git, curl, tar | Available (git-bash) |

Verified working: `podman run --rm alpine:latest` pulls and runs; Alpine 3.24 musl confirmed.

### 6.2 Ephemeral distro tooling — `scripts/wsl-ephemeral.ps1`

Creates throwaway WSL2 distros from **any OCI image**, so any distro at any version is one command
away. Verified end to end across six distros spanning **glibc 2.31 → 2.44 and musl**:

```
alpine:3.18            8s   musl
debian:bullseye-slim   7s   glibc 2.31
rockylinux:9          13s   glibc 2.34
opensuse/tumbleweed    9s   glibc 2.43
fedora:44             44s   glibc 2.43
archlinux:latest      15s   glibc 2.44
```

```powershell
.\scripts\wsl-ephemeral.ps1 -Action New  -Image alpine:3.22
.\scripts\wsl-ephemeral.ps1 -Action New  -Image debian:bullseye-slim -Command "ldd --version" -Ephemeral -Force
.\scripts\wsl-ephemeral.ps1 -Action Run  -Name eph-alpine-3.22-ab12 -Command "apk add gcc"
.\scripts\wsl-ephemeral.ps1 -Action List
.\scripts\wsl-ephemeral.ps1 -Action Purge -Force
```

**Safety model** — removal is guarded four ways: prefix (`eph-`), an explicit protected list
(`podman-machine-default`, Docker/Rancher Desktop), a containment check confining deletion to
`%LOCALAPPDATA%\wsl-ephemeral\<distro>`, and `-Force` required when non-interactive.
`-Name podman-machine-default` sanitises to `eph-podman-machine-default` and is a harmless no-op —
**verified**. Windows drives are visible at `/mnt/c` for file exchange.

Use containers (fast, disposable) for most work; use ephemeral WSL distros when you need a real
init-less VM, systemd-free kernel access, or `/mnt/c` interop.

### 6.3 Minimum tooling, by tier

| Tier | Needs | Gets you |
|---|---|---|
| **0** | Python 3.8+, curl, tar — **any OS** | All static ELF analysis (§3.3, §3.4) |
| **1** | podman/docker **or** WSL | Everything in §3.1 — the entire evidence table |
| **2** | + `mesa-vulkan-swrast`, `vulkan-tools` in Alpine | Real Vulkan ICD loading, no GPU needed |
| **3** | + `xvfb`, the demo AppImage | End-to-end `vkcube` |
| **4** | + a real GPU | Optional hardware validation |

**No GPU is required for any mandatory test.** lavapipe (software Vulkan) covers Tiers 0–3, which
is exactly what Solo's Alpine CI and the AppImage's own CI use. There is always Tier-0 work —
**never report "blocked, no Linux"**.

---

## 7. Repository and permissions

Work in **`https://github.com/Azathothas/dlopen-experiment`** (already cloned; `main` tracks
`origin/main`).

### 7.1 Permission rules — non-negotiable

`gh` is authenticated as `Azathothas` with account-wide `repo` scope. **The token cannot be
technically restricted to one repository**, so these are policy rules you must enforce yourself:

**Allowed — full read/write on `Azathothas/dlopen-experiment` only:**
- `git push`, branches, tags, commits
- `gh issue`/`gh pr`/`gh release` **on this repo only**
- `gh run`, `gh workflow` on this repo

**Forbidden — everywhere else, no exceptions:**
- Creating or commenting on issues, PRs, discussions, or reviews on **any** other repository
- Starring, forking, watching, or editing any other repository
- Pushing to any other remote
- Any `gh api` call with a non-`GET` method against a resource outside this repo
- Modifying account settings, keys, gists, or org membership

**Read-only elsewhere is fine**: cloning public repos, `gh api` `GET`, fetching releases.

If a task seems to need a write outside this repo, **stop and ask**. Upstream projects
(`Anylinux-AppImages`, `solo`, `detour`) are **read-only** — study them, never file anything on
them on the user's behalf.

### 7.2 Layout

```
PROMPT.md                       this file
README.md                       results summary
.gitattributes                  LF for .sh -- a CR breaks container scripts
elfsym.py                       dependency-free ELF64 reader              [provided, validated]
gap.py                          symbol-gap driver (--fetch)               [provided, validated]
experiments/run.ps1             one-command evidence table (E1-E13)      [provided, 14/14 pass]
experiments/10-build-musl.sh    stage 1: Alpine musl probe
experiments/20-build-newglibc.sh stage 2: newer-glibc libs + runtime
experiments/30-run-tests.sh     stage 3: E1-E13
scripts/wsl-ephemeral.ps1       ephemeral WSL distros                     [provided, validated]
src/                            the fix
analysis/                       measured reports
REPORT.md                       §10
```

---

## 8. Tasks

### Phase A — Ground truth (Tier 0/1)

- **A1.** Run `experiments/run.ps1`; confirm 14/14. Any MISMATCH is a finding — investigate before coding.
- **A2.** Run `python3 gap.py --fetch`; confirm the musl gap is `['___environ', 'atexit']`.
- **A3.** Extract the demo AppImage and inventory it.
  The embedded filesystem is **DwarFS**, not squashfs — verified: the ELF runtime ends at offset
  **1 487 344** (`0x16b1f0`) and the `DWARFS\x02\x05` superblock sits exactly there. (`DWARFS`
  hits near 340 160 are just string constants — `DWARFS_BLOCKSIZE`, `URUNTIME_MOUNT` — inside the
  runtime. A naive magic scan will mislead you.) Use `--appimage-extract`.
  Record: bundled glibc version, whether the legacy split libs are bundled, `.preload` contents.
- **A4.** Re-run the §3.3 delta against the **actual bundled** `libc.so.6` to get the real
  forward-compat surface for *this* AppImage.
- **A5.** Collision surface: sonames present in both the AppDir and the Alpine host closure.
- **A6.** Across the six distros in §6.2 (glibc 2.31-2.44 + musl), record for each: host glibc
  version, whether a *complete* matched runtime set is present, and therefore which branch of the
  §4.5 decision matrix applies. This is the input to Design R.

### Phase B — Implement

- **B0.** Design R: host-runtime probe + completeness check + re-exec launcher (§5.0). **Primary.**
- **B1.** Design A (`atexit` shim).
- **B2.** Shim **generator** (§5.2 steps 1–3) + generated `forward-shim.c`.
- **B3.** `___environ` `st_name` remap + tail-merge guard.
- **B4.** Keep legacy split libs loaded (fixes the E6 class).
- **B5.** Dry-run mode and loud diagnostics.
- **B6.** Design P: a sharun patch closing the §5.5 path gaps (`/usr/local/lib*`,
  `/etc/ld.so.conf{,.d}` parsing, musl path file, extra triplets), plus a test proving a library
  reachable only via the cache is still found with `--inhibit-cache`. Deliver as a patch in this
  repo — **do not** upstream it yourself.
- **B7.** Written verdict on Designs C and D.

### Phase C — Prove

- **C1.** Extend `experiments/30-run-tests.sh` with a case for every fix.
- **C2.** Run the highest tier reachable; record tier and every skip.
- **C3.** `REPORT.md`.

### 8.1 The iteration loop

No need to rebuild an AppImage to test a change:

```bash
./vkcube+glxgears-host-drivers-demo-x86_64.AppImage --appimage-extract
ls -d */ && export APPDIR="$PWD/<extracted-dir>"      # confirm the name, do not assume
test -f "$APPDIR/AppRun" && test -f "$APPDIR/.preload" || echo "wrong APPDIR"
cc -shared -fPIC -O2 foreign-dlopen.c -o "$APPDIR"/lib/foreign-dlopen.so   # same as quick-sharun
"$APPDIR"/AppRun
```

| Switch | Effect |
|---|---|
| `$APPDIR/.foreign-dlopen-enabled` | marker file; enables the feature by default |
| `ANYLINUX_LIB_FOREIGN_DLOPEN=1` / `=0` | force on / off — **`=0` is your A/B control** |
| `ANYLINUX_LIB_DEBUG=1` | trace to stderr, prefixed ` [foreign-dlopen.so] >> ` |
| `SHARUN_ALLOW_SYS_VKICD=1` | permit host Vulkan ICDs (needed for lavapipe) |
| `APPIMAGE_EXTRACT_AND_RUN=1` | skip FUSE — required in most containers |
| `$APPDIR/.preload` | order matters: `foreign-dlopen.so` **after** `anylinux.so` |

**Run every runtime test twice**, `=0` then `=1`. A single-sided result cannot distinguish "the fix
worked" from "it was already falling back to bundled software rendering".

Source of the file being modified:
`https://raw.githubusercontent.com/Samueru-sama/Anylinux-AppImages/main/useful-tools/lib/foreign-dlopen.c`

---

## 9. Tests

Every test states its PASS condition and what failure means. A test you cannot run is **SKIPPED
with the specific missing capability named** — never silently omitted, never guessed.

### 9.0 Diagnostic ladder — walk this when something fails

`atexit` and the newer-symbol delta are the predicted blockers. They are not guaranteed to be the
only ones. Report **which rung** caught each failure.

1. **Host driver sane?** `vulkaninfo --summary` natively in Alpine. If this fails, stop.
2. **Display, not libc?** Re-run under `xvfb-run -a`. WSI errors are not your bug.
3. **Feature on?** `ANYLINUX_LIB_DEBUG=1` — no ` [foreign-dlopen.so] >> ` lines means the marker,
   env switch, or `.preload` order is wrong.
4. **Which object, which symbol?** `LD_DEBUG=libs,bindings`. An `undefined symbol: X` names your
   next shim candidate.
5. **Is `X` really absent?** Check with `elfsym.py` against the **bundled** `libc.so.6`. If present,
   this is a scope/visibility problem (`RTLD_LOCAL`, `DF_SYMBOLIC`), not availability — different fix.
6. **Is `X` merely re-homed?** Check the legacy split libs (E6/E7). Load them instead of shimming.
7. **Rewrite corrupted the image?** Re-parse `$XDG_RUNTIME_DIR/.anylinux-fgn-*.so` with `elfsym.py`.
8. **Loads but misbehaves?** ABI territory — §9.3 and the hazard table.


### 9.0.1 No GPU? Test with software rendering — this is mandatory, not a fallback

**If the machine has no GPU, or no usable host driver, you must still run every Tier 2/3 test
using a software rasteriser. Do not mark them SKIPPED.** Software Vulkan (Mesa's **lavapipe**,
`libvulkan_lvp.so`) and software GL (**llvmpipe**) exercise the *identical* `dlopen` path — the
same ICD discovery, the same cross-libc load, the same rewrite. The only thing they do not
exercise is vendor-driver-specific behaviour, which is Tier 5.

This is exactly what the upstream CI does: the demo's own build script installs `vulkan-swrast`
with the comment *"CI has no available gpu for the test"*, and Solo's Alpine job installs
`mesa-vulkan-swrast`.

| Distro | Software Vulkan package | Also install |
|---|---|---|
| Alpine | `mesa-vulkan-swrast` | `vulkan-tools`, `mesa-demos`, `xvfb-run` |
| Arch | `vulkan-swrast` | `vulkan-tools`, `mesa-utils`, `xorg-server-xvfb` |
| Debian / Ubuntu | `mesa-vulkan-drivers` | `vulkan-tools`, `mesa-utils`, `xvfb` |
| Fedora | `mesa-vulkan-drivers` | `vulkan-tools`, `glx-utils`, `xorg-x11-server-Xvfb` |
| openSUSE | `libvulkan_lvp` | `vulkan-tools`, `Mesa-demo-x`, `xorg-x11-server-Xvfb` |

Pin the software driver explicitly so a half-working host GPU cannot silently take over and
invalidate the result:

```bash
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json   # older loaders
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"                            # newer loaders
export LIBGL_ALWAYS_SOFTWARE=1                                        # llvmpipe for GL
export SHARUN_ALLOW_SYS_VKICD=1                                       # let sharun use a host ICD
vulkaninfo --summary | grep -i llvmpipe     # expect lavapipe/llvmpipe
xvfb-run -a vkcube --c 100                  # finite frame count: exits on its own
```

`vulkaninfo` needs **no surface**, so use it as the first check; `vkcube` and `glxgears` need one,
hence `xvfb-run`. Record in `REPORT.md` which renderer actually served each run — a result that
does not name the renderer is not a result.

### 9.1 Tier 0 — static (any OS, no Linux)

| ID | Test | PASS |
|---|---|---|
| T0.1 | `gap.py` musl gap | exactly `['___environ', 'atexit']` |
| T0.2 | binding-mode audit | every closure member `BIND_NOW` |
| T0.3 | TLS audit | no `DF_STATIC_TLS`; every `PT_TLS memsz` < 4 KiB |
| T0.4 | rewriter round-trip | output parses; all four version tags gone **together**; size unchanged |
| T0.5 | idempotence | rewriting twice is byte-identical; content-hash name stable |
| T0.6 | `atexit` interposition safety | zero bundled `.so` imports `atexit` dynamically |
| T0.7 | tail-merge guard | guard **refuses** an unsafe in-place `.dynstr` write |
| T0.8 | malformed-input fuzz | truncated/bit-flipped ELFs: no crash, no OOB, clean refusal |

### 9.2 Tier 1 — the evidence table

`experiments/run.ps1` — E1…E11 must all MATCH. Add a case for each new fix. **This is the
regression gate; run it before every commit.**

### 9.3 Tier 1b — cross-libc ABI microtests

| ID | Test | PASS |
|---|---|---|
| T1.3 | allocator crossing (host `malloc` / app `free`, both ways) | clean under `MALLOC_CHECK_=3`, ASan |
| T1.4 | `errno` coherence both directions | both sides observe the same value |
| T1.5 | `FILE*` crossing (`stdout` in, `fopen`'d handle out) | correct ordering, no crash |
| T1.6 | `pthread_mutex_t`/`cond_t` shared both ways + contention loop | no deadlock, clean under TSan |
| T1.7 | ABI divergence probe (port Solo's `dev/abi_probe.c`) | every divergence unused by the closure, or handled |

**glibc vs musl hazard list (x86-64), already measured** — prove the closure does not touch these:

| Probe | glibc | musl | Risk |
|---|---|---|---|
| `regmatch_t` size | 8 | **16** | index corruption |
| `struct rusage` | 144 | **272** | overrun |
| `struct sched_param` | 4 | **48** | overrun on write |
| `ucontext_t` | 968 | **936** | overrun on write |
| `FTW_F`/`FTW_D`/`FTW_SL`/`FTW_NS` | 0/1/4/3 | 1/2/5/4 | **silently wrong branch** |
| `HOST_NAME_MAX` / `NI_MAXHOST` | 64 / 1025 | 255 / 255 | truncation |
| `O_LARGEFILE` | 0 | 32768 | flag confusion |
| `FILE` size | 216 | opaque | see T1.5 |

**Matching (safe):** `struct stat`, `dirent`, `sigaction`, `siginfo`, `termios`, `tm`, `msghdr`,
`passwd`, `group`, `addrinfo`, `statvfs`, `statfs`, `flock`, `epoll_event`, `glob`, `hostent`.

### 9.4 Tier 2 — real driver, no GPU

| ID | Test | PASS |
|---|---|---|
| T2.1 | Alpine native baseline: `apk add mesa-vulkan-swrast vulkan-tools && vulkaninfo --summary` | **lavapipe listed by name.** If this fails, stop — nothing downstream is interpretable |
| T2.2 | foreign-load the real ICD | handle non-NULL, `vk_icdGetInstanceProcAddr` resolves, `vkCreateInstance` succeeds |
| T2.3 | debug trace | each host object rewritten once, cached; **no** attempt to load musl libc |
| T2.4 | corpus: foreign-`dlopen` every `.so` in Alpine `/usr/lib`, before vs after | successes ≥ baseline; **zero regressions**; gains dominated by `atexit` importers |

**T2.2 setup.** Alpine has no glibc, so give the harness the AppImage's own runtime:
`"$APPDIR"/lib/ld-linux-x86-64.so.2 --library-path "$APPDIR"/lib ./harness`.
Get this green before touching Tier 3 — it isolates the cross-libc load from AppImage mounting,
ICD discovery, X11, and rendering.

| T2.5 | Design R selector across the §6.2 distro matrix | picks `host` on every glibc newer than bundled, `bundled` on older and on musl; **never** assembles a mixed set; decision logged with a reason |
| T2.6 | forced `ANYLINUX_RUNTIME=bundled` on a newer host | still works via shim path; proves the fallback is real, not theoretical |
| T2.7 | with the patched (cache-inhibited) loader, a library reachable only via `/etc/ld.so.cache` | still resolves, because `--library-path` now covers its directory (E13c) |

**T2.4 is what separates a fix from a demo.** Solo runs exactly this over 2 100+ objects per commit.
The zero-regression clause is the hard gate.

### 9.5 Tier 3 — end to end

| ID | Test | PASS |
|---|---|---|
| T3.1 | baseline on Alpine (**must fail before the fix**) | fails **with a symbol-resolution error**, not a display error — run under `xvfb-run -a`, capture verbatim |
| T3.2 | `vkcube` with host driver | survives ≥ 12 s (the project's own criterion); exit 0 on SIGTERM; frames produced |
| T3.3 | `glxgears` (OpenGL path) | same |
| T3.4 | driver provenance | mapped driver path is the **host's**, not `$APPDIR` — else you tested bundled software rendering |

> ⚠ `T3.1` without `xvfb-run` fails with a WSI error that has nothing to do with libc, and you
> will "confirm" the bug for the wrong reason.

### 9.6 Tier 4 — invariants

| ID | Test | PASS |
|---|---|---|
| T4.1 | `grep -E 'libc\.so\.6\|ld-musl' /proc/<pid>/maps` | exactly one libc family |
| T4.2 | `dladdr` each collision-surface soname | `dli_fname` under `$APPDIR` for every bundled lib |
| T4.3 | hash `/usr/lib`, `/lib`, `/etc/ld.so.*` before/after | identical; writes only under `XDG_RUNTIME_DIR`/`TMPDIR` |
| T4.4 | no regression on glibc hosts (Arch **and** Ubuntu 20.04) | behaviour unchanged from pre-fix |
| T4.5 | 100 load/unload cycles, 60 s `vkcube` | stable RSS, no fd leak, no `.anylinux-fgn-*` accumulation |

**Capturing maps for a short-lived GUI process:**
```bash
xvfb-run -a "$APPDIR"/AppRun & pid=$!; sleep 5
cp /proc/$pid/maps maps.txt; kill -TERM $pid
awk '{print $6}' maps.txt | grep '\.so' | sort -u
```

### 9.7 Tier 5 — optional hardware

T5.1 real GPU (`radv`/`anv`/`radeonsi`); T5.2 NVIDIA proprietary on musl; T5.3 aarch64.
Mark **SKIPPED — no such hardware** if unavailable. That is acceptable; a fabricated result is not.

---

## 10. `REPORT.md` — required contents

1. **Environment reached** — highest tier and how. Include `uname -a` and versions.
2. **Every SKIPPED test**, with the specific missing capability.
3. **Measured vs assumed** — any claim not backed by a command whose output you pasted is
   labelled *unverified*.
4. **Forward-compat posture** — which glibc floor the generated shim targets, how many symbols it
   covers, and what happens when an uncovered one appears (it must fail loudly, naming the symbol).
5. **Known-unfixed** — say plainly that case 3 (glibc lib → musl process) is out of scope and
   point at Solo.
6. **Residual risk** — the `T1.7` hazards not exercised. Symbol availability is necessary but
   **not sufficient**: a load that succeeds can still be semantically wrong.

Skip template:
```
T5.1  SKIPPED - no discrete GPU in the test environment.
      Nearest evidence: T2.2 + T3.2 pass with lavapipe (software Vulkan),
      exercising the identical dlopen path. Hardware-specific failures
      (e.g. libdrm ioctl ABI) remain unverified.
```

---

## 11. Why this matters

### 11.1 Delivered by this work (cases 1 and 2)

1. **Portable apps stop bundling a driver stack.** The demo AppImage is **10.7 MB** because it
   ships *zero* drivers; bundling Mesa+LLVM would add 100–200 MB (Alpine's `llvm20-libs` alone is
   63 MB compressed).
2. **Apps stop breaking on newer distros.** Case 1 is the everyday failure: a user on a current
   rolling distro whose host driver was built against a glibc newer than the AppImage's.
3. **musl distros become first-class** — Alpine, Chimera, Void-musl, postmarketOS, Gentoo-musl get
   host acceleration with no `gcompat` and no second libc.
4. **Immutable / non-FHS distros** (NixOS, Silverblue, SteamOS) stop needing per-distro path shims.
5. **Plugin subsystems fail loudly instead of weirdly.**
   [Dolphin-emu-AppImage#63](https://github.com/pkgforge-dev/Dolphin-emu-AppImage/issues/63) is the
   cautionary tale: Wii games failed to load Riivolution patches, and the cause was
   `Iconv initialization failure [SJIS]: Invalid argument` — a **gconv** module, glibc's *own*
   `dlopen`-based plugin system, missing because the bundled glibc fell back to `/usr/lib/gconv`
   (valid on Arch, absent on Ubuntu). The maintainer: gconv "doesn't tell you what is missing,
   things just randomly break." gconv, NSS, PAM, GStreamer, ALSA, p11-kit all sit on this fault
   line. **Failing loudly with the symbol named is the part that generalises furthest.**

### 11.2 Needs case 3 as well — *not* delivered here

NVIDIA's glibc-only userspace on a musl process; static musl binaries with GPU access (Solo already
does this); bridging manylinux wheels into Alpine; distroless containers reaching host NSS/PAM.
Say so in `REPORT.md` rather than implying otherwise.

---

## 12. Pitfalls

- **"Load everything lazily."** Refuted — every Mesa object is `DF_BIND_NOW`; `ld.so` ignores
  `RTLD_LAZY`.
- **"Strip the versions and hope."** E4: you get `undefined symbol` instead of a version error.
- **"A generated shim gives forward compatibility."** It does not, and cannot -- you cannot shim a
  symbol nobody has invented yet (§4.5). Forward compatibility comes from Design R (E12), where the
  symbol is supplied by the host's own libc. The shim covers only the *enumerable* gap.
- **"Just load the host libc too."** E8/E9: impossible in-process, `ld.so`↔`libc` version lock.
- **"Switch to the host runtime."** E10 works, E11 shows it must be the **whole** matched set.
- **"Port Detour."** Requires a libc-free process. An AppImage is not one.
- **Never strip versions partially** — a `verdef` without its `versym` segfaults `ld.so`.
- **Never touch `ld-linux*`, `libc.so.*`, `ld-musl*`** — `fgn_never_touch[]` exists for a reason;
  `ld-linux` has no `SONAME`, so `RTLD_NOLOAD` cannot catch it.
- **Symbol availability ≠ ABI compatibility.** A successful `dlopen` is the start of correctness.
- **"The host loader can find it, so we can too."** Not with anylinux's patched `ld.so`: it never
  reads `/etc/ld.so.cache` (E13b). Discovery comes only from `--library-path`.
- **Do not add library searching to `foreign-dlopen.c`.** Two search implementations will diverge
  (§5.5). Fix the path in sharun.
- **PowerShell:** piping a string to a native process re-encodes it and corrupts the tail — mount
  scripts instead (this bit `run.ps1`; see its comments). A function that leaves native output on
  the success stream returns an **array**, not your exit code.
- **Shell scripts must be LF.** A CR turns into `$'...\r'` and yields baffling "not found" errors.
  `.gitattributes` enforces it; `run.ps1` verifies rather than trusts.

---

## 13. References

| Thing | Where |
|---|---|
| Implementation being fixed | `Samueru-sama/Anylinux-AppImages` → `useful-tools/lib/foreign-dlopen.c` (859 lines) |
| Build harness | `useful-tools/quick-sharun.sh` (`_add_foreign_dlopen_lib`, `--test`) |
| Demo recipe / binary | `useful-tools/demo/vkcube-glxgears-host-drivers-appimage.sh` · [release `demo`](https://github.com/Samueru-sama/Anylinux-AppImages/releases/download/demo/vkcube+glxgears-host-drivers-demo-x86_64.AppImage) (10.7 MB, DwarFS) |
| Case-3 reference (read-only) | [pg83/solo](https://github.com/pg83/solo) — `lib/elf_loader.cpp`, `lib/glibc_shim.cpp`, `dev/abi_probe.c`, `dev/generate_glibc_stubs.py`, `.github/workflows/ci.yml` |
| Foreign-`ld.so` trampoline | [graphitemaster/detour](https://github.com/graphitemaster/detour) · [pfalcon/foreign-dlopen](https://github.com/pfalcon/foreign-dlopen) |
| **Launcher assembling `--library-path`** | [VHSgunzo/sharun](https://github.com/VHSgunzo/sharun) — `src/main.rs` (~1 161 lines; path assembly at ≈1030–1066), plus the `lib4bin` collector script. **Read-only: patch here, do not upstream yourself** |
| **Single-binary packager with GPU bundling** | [QaidVoid/onelf](https://github.com/QaidVoid/onelf) — `crates/onelf/src/bundle/gpu.rs` enumerates DRI/GBM/Vulkan ICD search paths; `bundle/resolve.rs` does dependency resolution. Useful prior art for the §5.5 path list |
| Real-world breakage catalogue | `Anylinux-AppImages/HALL-OF-FAME.md` (incl. the `ld.so.cache` segfault that forced the patched loader) |
| Other tooling already wired up | the `Anylinux-AppImages` repo links many more tools from `quick-sharun.sh` — `pathmap`/`ld-preload-open`, `uruntime`, `get-debloated-pkgs.sh`, the `hooks/` directory. **Check there before writing anything new** |
| gconv cautionary tale | [Dolphin#63](https://github.com/pkgforge-dev/Dolphin-emu-AppImage/issues/63) · [Dolphin#20](https://github.com/pkgforge-dev/Dolphin-emu-AppImage/issues/20) |
| musl vs glibc | https://wiki.musl-libc.org/functional-differences-from-glibc.html |
