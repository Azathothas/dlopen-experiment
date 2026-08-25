# Phase A — ground truth, measured

Every number here came from a command that was run. Nothing is estimated.
Reproduce with the commands shown; the tooling is in `tools/`.

---

## A1 — the evidence table still holds

```
.\experiments\run.ps1
```

**22/22 predictions held** (E1–E13 unchanged, E14–E21 added by this work; see
`REPORT.md`). Before any change was made it was 14/14, so nothing here
regressed the baseline.

## A2 — the musl gap is exactly two symbols

```
python3 gap.py --fetch
```

```
glibc exports : 4155
musl  exports : 1645
musl-only (absent from glibc entirely): 53
UNION OF GAP over whole musl closure: ['___environ', 'atexit']
```

Matches PROMPT.md §3.4 exactly. `atexit` STRONG (fatal under `DF_BIND_NOW`),
`___environ` WEAK (resolves to 0, latent).

---

## A3 — the demo AppImage, inventoried

`vkcube+glxgears-host-drivers-demo-x86_64.AppImage`, sha256
`712766f8a4dc6b5ea3193ed7bb0282b64c7b781f7334056416edd3d00e8960bd`, 10 736 056
bytes. Extracted with `--appimage-extract` (the embedded filesystem is DwarFS,
as PROMPT.md §A3 warns).

| Question | Answer |
|---|---|
| **Bundled glibc version** | **2.44** — `ld.so (GNU libc) stable release version 2.44` |
| Legacy split libs bundled? | **Yes**: `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libutil.so.1`, `libresolv.so.2`. `libanl.so.1` is **absent**. |
| …but are they real? | **No — they are post-2.34 stubs.** Measured export counts: libpthread **13**, libdl **4**, librt **6**, libutil **2**. |
| `.preload` contents | `path-mapping.so`, `anylinux.so`, `foreign-dlopen.so` — in that order |
| `.foreign-dlopen-enabled` | present (0 bytes), so the feature is on by default |
| Bundled `foreign-dlopen.c` | **byte-identical** to upstream `main` (24 785 bytes) |
| Bundled libraries | 51 sonames; `gconv/`, `locale/`, `vkmark/` subdirectories |
| gconv bundled? | **Yes** — the Dolphin#63 lesson has been applied |

### Why the stub detail matters

PROMPT.md §5.2 B4 says to keep the legacy split libraries loaded so E6-class
failures become E7-class successes. On **this** AppImage that is a no-op,
because its glibc is 2.44 and those files are 4-to-13-symbol stubs. The fix is
still implemented, because it is load-bearing for any AppImage built on a
pre-2.34 glibc — but it is not what makes this artifact work, and saying
otherwise would be wrong.

What *is* load-bearing here is the other half of the same idea, which the task
did not anticipate: **musl folds `libm` into `libc`, glibc splits it out**. See
`REPORT.md` §"What actually blocked the musl case".

---

## A4 — the forward-compat surface for *this* AppImage is empty

```
python3 tools/libc_inventory.py scan <appdir>/lib --name appdir-bundled -o inventories/appdir.json
python3 tools/libc_inventory.py matrix <distro-runtimes>/
```

Bundled = glibc **2.44**, 4 287 dynamic symbols across the runtime set.

| Host | Release | Symbols the host has and the bundle lacks | Decision-matrix branch |
|---|---|---|---|
| debian bullseye | 2.31 | 35 | BUNDLED (host older) |
| ubuntu 20.04 | 2.31 | 35 | BUNDLED (host older) |
| rocky 9 | 2.35 | 10 | BUNDLED (host older) |
| debian trixie | 2.41 | 4 | BUNDLED (host older) |
| fedora 44 | 2.43 | 1 | BUNDLED (host older) |
| opensuse tumbleweed | 2.43 | 1 | BUNDLED (host older) |
| arch | 2.44 | 1 | BUNDLED (host equal) |
| alpine 3.22 | musl 1.2.5 | n/a | BUNDLED + shim (no host glibc) |

The single "host-only" symbol on the newest hosts is
`__libanl_version_placeholder` — an empty ABI placeholder from `libanl.so.1`,
which this AppDir does not bundle. It is not reachable from a driver.

**So: this AppImage has no enumerable forward-compat gap today, because it
bundles the newest released glibc.** Running the generator against its own
floor produces a shim containing exactly two functions, and both are the musl
bridge rather than a version gap:

```
floor  : appdir-bundled glibc 2.44 (4287 symbols)
target : glibc-2.44 (4288 symbols)
gap    : 3 symbols the floor lacks
   implementable     2      <- atexit, at_quick_exit
   stub-only         0
   irrelevant        1      <- __libanl_version_placeholder
```

This is the honest answer and it reframes the priority order:

- **Case 1 is already solved for this artifact by bundling a new-enough
  glibc.** No shim, no runtime switch, nothing to do.
- It is *not* solved in general. Any AppImage built on an older distro has the
  gap, and this one acquires it the day glibc 2.45 ships.
- Therefore the generator is parameterised by the floor and regenerated on a
  glibc bump, and it is demonstrated against a realistic older floor (2.31),
  where the gap is 628 symbols. See `REPORT.md`.

---

## A5 — collision surface

Sonames present in **both** the AppDir and the Alpine v3.22 Mesa+LLVM closure:

| soname | AppDir | Alpine host |
|---|---|---|
| `libGL.so.1` | `libGL.so.1.7.0` (GLVND) | `mesa-gl` |
| `libgcc_s.so.1` | bundled | `libgcc 14.2.0` |
| `libstdc++.so.6` | `libstdc++.so.6.0.36` | `libstdc++ 14.2.0` (`.6.0.33`) |

**Three sonames.** These are exactly what T4.2 must guard, and they are exactly
where the pre-fix implementation was violating it — see the T4.2 defect in
`REPORT.md`.

Host-only (no bundled counterpart, so they must come from the host):
`libLLVM.so.20.1`, `libgallium-25.1.9.so`, `libvulkan_lvp.so`,
`libc.musl-x86_64.so.1`.

---

## A6 — the distro matrix, and which branch each takes

Runtime sets copied out of eight real images and inventoried offline.

```
set                family  release    syms  missing
debian-bullseye    glibc   2.31       3697  -
ubuntu-2004        glibc   2.31       3697  -
rocky-9            glibc   2.35       3904  -
debian-trixie      glibc   2.41       4226  -
fedora-44          glibc   2.43       4286  -
opensuse-tw        glibc   2.43       4286  -
arch               glibc   2.44       4288  -
alpine-3.22        musl    1.2.5      1645  -
```

Note: PROMPT.md §6.2 lists rocky as glibc 2.34; the current image measures
**2.35**. Fedora 44 and openSUSE Tumbleweed both measure 2.43, Arch 2.44 —
matching the prompt.

**Every one of the eight provides a complete matched runtime set** (every
member of `{ld.so, libc, libm, libdl, libpthread, librt, libutil, libanl,
libresolv}` present, from one directory). So the completeness check is never
the thing that blocks a switch on a mainstream distro; the *version
comparison* is.

### Design R decision, measured on each host

Run with a fake AppDir bundling glibc 2.31, so the newer hosts are genuinely
newer (the real AppImage bundles 2.44 and would pick `bundled` everywhere —
which is correct, and is why the probe is run both ways):

| Host | Host glibc | Decision | Reason given |
|---|---|---|---|
| debian bullseye | 2.31 | **bundled** | not newer than bundled |
| ubuntu 20.04 | 2.31 | **bundled** | not newer than bundled |
| rocky 9 | 2.35 | **host** | newer + set internally consistent |
| debian trixie | 2.41 | **host** | newer + set internally consistent |
| fedora 44 | 2.43 | **host** | newer + set internally consistent |
| opensuse tw | 2.43 | **host** | newer + set internally consistent |
| arch | 2.44 | **host** | newer + set internally consistent |
| alpine 3.22 | musl | **bundled** | no host glibc — bundled + shim is the only option |

Every switch was additionally verified empirically (`--self-test`) before being
committed to. This is T2.5, and it passes.

---

## Library-path configuration, per distro (input to Design P)

| Distro | `/usr/local/lib` | `/usr/local/lib64` | `/usr/libexec` | `ld.so.conf` names | cache |
|---|---|---|---|---|---|
| alpine 3.22 | yes | no | no | *(no ld.so.conf; musl)* | absent |
| debian bullseye | yes | no | yes | `/usr/local/lib`, `/usr/local/lib/<triplet>`, `/lib/<triplet>`, `/usr/lib/<triplet>` | present |
| debian trixie | yes | no | yes | same as bullseye | present |
| ubuntu 20.04 | yes | no | no | same as bullseye | present |
| rocky 9 | yes | yes | yes | *(empty in base image)* | present |
| fedora 44 | yes | yes | yes | *(empty in base image)* | present |
| opensuse tw | yes | yes | yes | `/usr/local/lib64`, `/usr/local/lib` | present |
| arch | yes | no | no | *(empty in base image)* | present |

`/usr/local/lib` exists on **all eight** and was absent from sharun's hardcoded
list. That is the concrete bug E13b demonstrates, and what
`patches/sharun-library-path.patch` fixes.
