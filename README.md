# dlopen-experiment

Can a process load a host shared library that was built against a **different libc** — a newer
glibc, or musl — without patching host files, without a second libc, and without symbol collisions?

This repo answers that with **runnable experiments**, not argument. Everything below was measured.

```powershell
.\experiments\run.ps1        # ~2 min, needs podman or docker. Last run: 14/14 predictions held.
```

## The short version

| Question | Answer |
|---|---|
| Does today's version-stripping give forward compatibility? | **No.** Strip `GLIBC_2.38` off `arc4random` and you get `undefined symbol: arc4random` (E4) |
| Can you load a second glibc in-process to get the missing symbols? | **No.** `libc.so.6` needs `GLIBC_2.35`+`GLIBC_PRIVATE` **from `ld-linux`**; a newer libc will not load under an older loader — by `dlopen` **or** `dlmopen` (E8, E9) |
| Then where do future symbols come from? | **The host's own runtime, chosen at `execve`.** The library that fails in E3/E4 runs fine under the host's complete runtime with **no shim at all** (E12) |
| Is a shim still needed? | **Yes, for the gap you can enumerate** — and for musl hosts, where there is no host glibc to switch to (E2, E5) |
| Why does stripping work as often as it does? | glibc 2.34 merged `libpthread`/`libdl`/`librt` into `libc.so.6`. Those symbols still exist in the old split libs — stripping succeeds **iff** those libs are loaded (E6 vs E7) |
| Is finding the library a separate problem from resolving its symbols? | **Yes.** Anylinux patches `ld.so` to skip `/etc/ld.so.cache` (it segfaults on some hosts), so `--library-path` is the *only* discovery mechanism. A library in `/usr/local/lib` — present on every distro surveyed, absent from sharun's list — becomes invisible (E13) |

The design that follows: **switch to the host runtime when it is newer and complete; otherwise keep
the bundled runtime and cover the enumerable delta with a generated shim; always fail loudly naming
the symbol.** Details and the full task breakdown are in [PROMPT.md](PROMPT.md).

## Evidence table

| ID | Experiment | Result |
|---|---|---|
| E1 | musl lib, musl `DT_NEEDED` dropped, into glibc | FAILS — `undefined symbol: atexit` |
| E2 | same + 3-line `atexit` shim | WORKS |
| E3 | glibc-2.41 lib into glibc 2.31 | FAILS — `` version `GLIBC_2.38' not found `` |
| E4 | same, version-stripped | FAILS — `undefined symbol: arc4random` |
| E5 | same, stripped + compat shim | WORKS |
| E6 | `pthread_create@GLIBC_2.34`, stripped, no `libpthread` loaded | FAILS |
| E7 | same, with `libpthread.so.0` in the process | WORKS |
| E8 | `dlopen` newer `libc.so.6` under older `ld.so` | FAILS — version lock |
| E9 | same via `dlmopen(LM_ID_NEWLM)` | FAILS — identical |
| E10 | exec-time switch to host's **whole** runtime | WORKS |
| E11 | exec-time switch with a **mixed** runtime | SIGSEGV |
| E12 | E3/E4's library under host's complete runtime, **no shim** | WORKS |
| E13a | lib in `/usr/local/lib`, `--library-path` omits it, cache allowed | WORKS (found via cache) |
| E13b | same, `--inhibit-cache` (= anylinux's patched loader) | FAILS |
| E13c | same, cache inhibited, directory added to `--library-path` | WORKS |

## Library discovery

Separate from symbol resolution, and separately broken. `foreign-dlopen.c` has no
`LD_LIBRARY_PATH` handling and no cache parsing — it scrapes `dlerror()` text for
`"(required by ...)"`. The fix belongs in **sharun** (which already assembles `--library-path` and
already honours `LD_LIBRARY_PATH`), not in a second search implementation in C. Measured gaps:
`/usr/local/lib`, `/usr/local/lib64`, `/etc/ld.so.conf{,.d}` parsing, musl's
`/etc/ld-musl-<arch>.path`, and non-x86-64/aarch64 triplets. See PROMPT.md §5.5.

## No GPU?

Every Tier 2/3 test runs on software rendering — Mesa **lavapipe** (Vulkan) and **llvmpipe** (GL)
exercise the identical `dlopen` path. Upstream CI does the same. A missing GPU is not a reason to
skip a test; only vendor-driver-specific behaviour (Tier 5) needs hardware.

## The musl case

Static analysis over real Alpine v3.22 Mesa + LLVM (`python3 gap.py --fetch`): of musl's 1 645
exports, only **53** are absent from glibc, and the entire Mesa/LLVM closure needs exactly **two**
of them — `atexit` (strong → fatal) and `___environ` (weak → latent). Every object is `DF_BIND_NOW`,
so `RTLD_LAZY` cannot defer the problem.

## Layout

```
PROMPT.md                        full task prompt: evidence, designs, tests, tooling
experiments/run.ps1              one-command evidence table (E1-E13)
experiments/*.sh                 the three container stages
elfsym.py                        dependency-free ELF64 reader
gap.py                           symbol-gap driver (--fetch)
scripts/wsl-ephemeral.ps1        ephemeral WSL2 distros from any OCI image
```

## Ephemeral distro tooling

Verified across six distros spanning **glibc 2.31 → 2.44 and musl**:

```powershell
.\scripts\wsl-ephemeral.ps1 -Action New -Image alpine:3.22
.\scripts\wsl-ephemeral.ps1 -Action New -Image debian:bullseye-slim -Command "ldd --version" -Ephemeral -Force
.\scripts\wsl-ephemeral.ps1 -Action List
.\scripts\wsl-ephemeral.ps1 -Action Purge -Force
```

Removal is guarded four ways — an `eph-` prefix, an explicit protected list
(`podman-machine-default`, Docker/Rancher Desktop), a containment check confining deletion to
`%LOCALAPPDATA%\wsl-ephemeral\<distro>`, and `-Force` required non-interactively.

## Prior art

- [pg83/solo](https://github.com/pg83/solo) — the reverse direction (static musl loading host
  glibc drivers), with its own ELF loader and a ~6 000-line glibc→musl ABI bridge. Production-grade:
  CI across Alpine/Fedora/NixOS/Termux plus a 2 100-object corpus test per commit.
- [graphitemaster/detour](https://github.com/graphitemaster/detour) and
  [pfalcon/foreign-dlopen](https://github.com/pfalcon/foreign-dlopen) — driving a foreign `ld.so`
  in-process. Requires a **libc-free** process, so not applicable to an AppImage.
- [Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages) — `foreign-dlopen.c`,
  the implementation this work targets. Its `quick-sharun.sh` already links a lot of tooling
  (pathmap, uruntime, debloated packages, `hooks/`) — check there before writing anything new.
- [VHSgunzo/sharun](https://github.com/VHSgunzo/sharun) — the launcher that assembles
  `--library-path`; where the §5.5 discovery fix belongs.
- [QaidVoid/onelf](https://github.com/QaidVoid/onelf) — single-binary packager whose
  `bundle/gpu.rs` enumerates DRI/GBM/Vulkan ICD search paths; useful prior art for that path list.
