# Ground truth, measured

Every number here came from a command that was run. Nothing is estimated.
Reproduce with the commands shown; the tooling is in `tools/`.

---

## The evidence table still holds

```
.\experiments\run.ps1
```

**22/22 predictions held.** E1 to E13 unchanged, E14 to E21 added by this work.
Before any change it was 14/14, so nothing here regressed the baseline.

## The musl gap is exactly two symbols

```
python3 gap.py --fetch
```

```
glibc exports : 4155
musl  exports : 1645
musl-only (absent from glibc entirely): 53
UNION OF GAP over whole musl closure: ['___environ', 'atexit']
```

`atexit` is STRONG, so fatal under `DF_BIND_NOW`. `___environ` is WEAK, so it
resolves to 0 and the bug is latent.

Note that this is the union over the **Mesa and LLVM closure**. Over the whole
Alpine `/usr/lib` one more musl-only symbol is load-bearing: `issetugid`, which
blocks `libX11.so.6` and `libdbus-1.so.3`. See REPORT.md section 5.3.

---

## The demo AppImage, inventoried

`vkcube+glxgears-host-drivers-demo-x86_64.AppImage`, sha256
`712766f8a4dc6b5ea3193ed7bb0282b64c7b781f7334056416edd3d00e8960bd`,
10 736 056 bytes. Extracted with `--appimage-extract`; the embedded filesystem
is DwarFS, not squashfs.

| Question | Answer |
|---|---|
| **Bundled glibc version** | **2.44**, from `ld.so (GNU libc) stable release version 2.44` |
| Legacy split libs bundled? | **Yes**: `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libutil.so.1`, `libresolv.so.2`. `libanl.so.1` is **absent** |
| Are they real libraries? | **No, they are post-2.34 stubs.** Measured export counts: libpthread **13**, libdl **4**, librt **6**, libutil **2** |
| `.preload` contents | `path-mapping.so`, `anylinux.so`, `foreign-dlopen.so`, in that order |
| `.foreign-dlopen-enabled` | present, 0 bytes, so the feature is on by default |
| Bundled `foreign-dlopen.c` | **byte-identical** to upstream `main`, 24 785 bytes |
| Bundled libraries | 51 sonames, plus `gconv/`, `locale/` and `vkmark/` subdirectories |
| gconv bundled? | **Yes**, so the Dolphin issue #63 lesson has been applied |

### Why the stub detail matters

Keeping the legacy split libraries loaded is what makes symbols re-homed by
glibc 2.34 still resolve after version stripping. On **this** AppImage that is
a no-op, because its glibc is 2.44 and those files are 4-to-13-symbol stubs.
The fix is still implemented, because it is load-bearing for any AppImage built
on a pre-2.34 glibc, but it is not what makes this artifact work and saying
otherwise would be wrong.

What **is** load-bearing here is the mirror image, which was not anticipated:
musl folds `libm` into `libc` while glibc splits it out. See REPORT.md
section 3.1.

---

## The forward-compatibility surface for this AppImage is empty

```
python3 tools/libc_inventory.py scan <appdir>/lib --name appdir-bundled -o inventories/appdir.json
python3 tools/libc_inventory.py matrix <distro-runtimes>/
```

Bundled is glibc **2.44**, 4 287 dynamic symbols across the runtime set.

| Host | Release | Symbols the host has and the bundle lacks | Branch taken |
|---|---|---|---|
| debian bullseye | 2.31 | 35 | bundled, host older |
| ubuntu 20.04 | 2.31 | 35 | bundled, host older |
| rocky 9 | 2.35 | 10 | bundled, host older |
| debian trixie | 2.41 | 4 | bundled, host older |
| fedora 44 | 2.43 | 1 | bundled, host older |
| opensuse tumbleweed | 2.43 | 1 | bundled, host older |
| arch | 2.44 | 1 | bundled, host equal |
| alpine 3.22 | musl 1.2.5 | n/a | bundled plus shim, no host glibc |

The single host-only symbol on the newest hosts is
`__libanl_version_placeholder`, an empty ABI placeholder from `libanl.so.1`,
which this AppDir does not bundle. It is not reachable from a driver.

**This AppImage has no enumerable forward-compatibility gap today, because it
bundles the newest released glibc.** Running the generator against its own floor
produces a shim whose entire version-gap contribution is zero, with only the
musl bridge left:

```
floor  : appdir-bundled glibc 2.44 (4287 symbols)
target : glibc-2.44 (4288 symbols)
gap    : 3 symbols the floor lacks
   implementable     2      <- atexit, at_quick_exit
   stub-only         0
   irrelevant        1      <- __libanl_version_placeholder
```

This reframes the priority order:

- Case 1 is already solved for this artifact by bundling a new-enough glibc.
  No shim, no runtime switch, nothing to do.
- It is **not** solved in general. Any AppImage built on an older distro has the
  gap, and this one acquires it the day glibc 2.45 ships.
- So the generator is parameterised by the floor and regenerated when the
  bundled glibc changes, and it is demonstrated against a realistic older floor
  of 2.31, where the gap is 628 symbols.

---

## Collision surface

Sonames present in **both** the AppDir and the Alpine 3.22 Mesa and LLVM
closure:

| soname | AppDir | Alpine host |
|---|---|---|
| `libGL.so.1` | `libGL.so.1.7.0`, GLVND | `mesa-gl` |
| `libgcc_s.so.1` | bundled | `libgcc 14.2.0` |
| `libstdc++.so.6` | `libstdc++.so.6.0.36` | `libstdc++ 14.2.0`, so `.6.0.33` |

**Three sonames.** These are exactly what the "bundled wins" invariant must
guard, and exactly where the pre-fix implementation was violating it. See
REPORT.md section 3.2.

Host-only, so they must come from the host: `libLLVM.so.20.1`,
`libgallium-25.1.9.so`, `libvulkan_lvp.so`, `libc.musl-x86_64.so.1`.

---

## The distro matrix

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

Rocky 9 is often cited as glibc 2.34; the current image measures **2.35**.
Fedora 44 and openSUSE Tumbleweed both measure 2.43, Arch 2.44, as expected.

**All eight provide a complete matched runtime set**, meaning every member of
`{ld.so, libc, libm, libdl, libpthread, librt, libutil, libanl, libresolv}` is
present from one directory. So the completeness check is never what blocks a
switch on a mainstream distro; the version comparison is.

### Selector decision on each host

Run with a fake AppDir bundling glibc 2.31, so the newer hosts are genuinely
newer. The real AppImage bundles 2.44 and picks `bundled` everywhere, which is
correct, and is why the probe is run both ways.

| Host | Host glibc | Decision | Reason given |
|---|---|---|---|
| debian bullseye | 2.31 | **bundled** | not newer than bundled |
| ubuntu 20.04 | 2.31 | **bundled** | not newer than bundled |
| rocky 9 | 2.35 | **host** | newer, set internally consistent |
| debian trixie | 2.41 | **host** | newer, set internally consistent |
| fedora 44 | 2.43 | **host** | newer, set internally consistent |
| opensuse tumbleweed | 2.43 | **host** | newer, set internally consistent |
| arch | 2.44 | **host** | newer, set internally consistent |
| alpine 3.22 | musl | **bundled** | no host glibc, bundled plus shim is the only option |

Every switch was verified empirically by the self-test before being committed
to.

---

## Library-path configuration, per distro

Input to the sharun patch.

| Distro | `/usr/local/lib` | `/usr/local/lib64` | `/usr/libexec` | `ld.so.conf` names | cache |
|---|---|---|---|---|---|
| alpine 3.22 | yes | no | no | none, musl | absent |
| debian bullseye | yes | no | yes | `/usr/local/lib`, `/usr/local/lib/<triplet>`, `/lib/<triplet>`, `/usr/lib/<triplet>` | present |
| debian trixie | yes | no | yes | same as bullseye | present |
| ubuntu 20.04 | yes | no | no | same as bullseye | present |
| rocky 9 | yes | yes | yes | empty in the base image | present |
| fedora 44 | yes | yes | yes | empty in the base image | present |
| opensuse tumbleweed | yes | yes | yes | `/usr/local/lib64`, `/usr/local/lib` | present |
| arch | yes | no | no | empty in the base image | present |

`/usr/local/lib` exists on **all eight** and was absent from sharun's hardcoded
list. That is the concrete bug E13b demonstrates, and what
`patches/sharun-library-path.patch` fixes.
