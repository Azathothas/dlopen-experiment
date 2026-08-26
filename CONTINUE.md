# CONTINUE

Start here if you have no context on this repository. This file is written to
be self-contained: it states what the project is, what is already done, what is
left, and exactly how to reproduce every result so you can trust or refute it.

Read [README.md](README.md) for the design and [REPORT.md](REPORT.md) for the
full per-test results. This file is the working handover.

---

## 0. Your assignment, if you were handed this file and nothing else

⭐ **You are here to close section 4.0, in the order it gives.** Not to audit,
not to summarise, not to confirm that it works. It does work; the claim around
it is wider than the evidence in eight named ways and 4.0 is those eight.

⚠ **Read that sentence twice, because the previous handover failed exactly
here.** It presented a status, an agent read the status, concluded the
experiment was closed, and a whole class of failure went unexamined for a
session. Sections 4.2 and 4.3 are a status. 4.0 is the work.

### Before you touch anything

```powershell
.\experiments\run.ps1
```

Expect **36/36**. ⛔ A MISMATCH *before* you have changed anything is a finding
about this machine or about a container image that moved, and it is the first
thing to understand -- not something to work around. Section 3 has the rest of
the reproduction, and section 2 has the environment, including where podman is
and how to reach the GPU. Neither is guessable.

### While you work

- ⛔ **State a prediction, then measure.** Every case in `experiments/` declares
  what it expects and the harness reports MATCH or MISMATCH. **A MISMATCH is a
  finding, not a harness bug**, and the correct response is to investigate
  before coding.
- ⛔ **Measured, or labelled UNVERIFIED. Never estimated.** That is this
  repository's whole standard and every document is written to it.
- ⛔ **Never single-sided.** Run the feature off and on. "It worked" cannot tell
  a fix from a fallback that was already happening.
- Section 7 is the invariants that must not break. Section 5 is everything that
  has already cost somebody a day. Section 6 is what to do when it fails.

### Before you finish

1. Both suites green: `run.ps1` **36/36**, `appimage.ps1` **40/40** on the glibc
   host and **35/35** with five named skips on musl.
2. **Update 4.0 in place.** An item closes where it is written, with the
   command that proves it and the output. ⛔ **A premise a measurement disproves
   keeps its title and gets the correction written underneath it** -- never a
   silent edit, because the title is how the item has been referred to.
3. **Rewrite 4.3** so the next reader does not redo what you did.
4. Update `README.md` and `REPORT.md` for anything a user or a reviewer would
   now be told wrongly. ⚠ Every headline count appears in more than one
   document today; if you change one, `git grep` for it.
5. **Commit and push.** The owner has granted that standing, for this
   repository, bounded by section 8. Anything section 8 forbids still stops and
   asks.

### What you are NOT here to do

⛔ **Do not start the port.** [`PORTING.md`](PORTING.md) is a standalone brief
for a *different* session with a *different* agent, and it should begin only
once B1, B2, B3 and B6 are closed. Porting a claim wider than its evidence just
publishes the gap. ⭐ If you have closed those four, say so plainly at the end of
your session and stop; the next session picks up `PORTING.md` and needs nothing
from this file.

---

## 1. What this is

An AppImage bundles its own glibc so it runs on any distro. It does **not**
bundle GPU drivers, because Mesa plus LLVM is 100-200 MB. So it has to use the
**host's**. Two different things stop it, and conflating them is the mistake
this repository has now made once and documented at length.

**Gap 1, libc.** The host's driver exists and is nameable, and it was built
against a different libc: a newer glibc, or musl on Alpine. The repair is
`foreign-dlopen.c` from
[Anylinux-AppImages](https://github.com/Samueru-sama/Anylinux-AppImages),
`LD_PRELOAD`ed, intercepting `dlopen` and rewriting host objects in a private
copy so their symbol version requirements stop mattering. The patched version
lives in [`src/foreign-dlopen.c`](src/foreign-dlopen.c).

**Gap 2, interface.** The host has the *capability* but ships nothing in the
shape the bundled loader looks for. The AppImage bundles libglvnd, whose
`libGL.so.1` is a dispatcher that `dlopen`s `libGLX_<vendor>.so.0`, and a
classic-Mesa host -- every musl distro -- has no such file. No amount of libc
bridging carries a file that does not exist. The repair is to replace the
bundled dispatcher: [`src/gl-fwd.c`](src/gl-fwd.c), built with that SONAME,
forwarding all 3470 entry points.

**The state as of this handover: both gaps are closed for GL, EGL and Vulkan.**
On Alpine, the demo AppImage's bundled glibc 2.44 drives Alpine's own musl-built
Mesa: `vkcube` renders, `glxgears` renders, `glprobe` clears a pixel and reads
the same colour back, `eglprobe` gets a surfaceless context, and exactly one
libc family is in the process. On a glibc host it drives NVIDIA's closed-source
CUDA userspace on a real RTX 3050 Ti and renders OpenGL on that GPU at over 100
FPS. Section 3.3 reproduces all of it in one command.

**Go to section 4.0 next.** It is the work order, and it exists because the
answer to "does this work on systems without glvnd" is **yes, but** rather than
either "no" or a clean "yes". The result is real and measured; the claim around
it is currently wider than the evidence, in eight named ways, and 4.0 is those
eight in the order they should be closed.

**Do not read section 4 as "the experiment is closed".** The handover before
this one said that, and it was true of the question as it had been framed and
false of the thing anyone running the AppImage wants. Sections 4.2 and 4.3 are
what is blocked by hardware, by packaging, or by the permission rule in section
8. What no section can list is a gap nobody has framed yet, and section 5.0 is
about how the last one hid.

**When 4.0 is closed, the next thing is not more experiments.**
[`PORTING.md`](PORTING.md) is a standalone brief for a separate session that
takes this repository production-grade under
[Azathothas/TEMPLATE](https://github.com/Azathothas/TEMPLATE). It requires no
other document and deliberately does not assume this one has been read.

## 2. Environment

Everything runs in throwaway containers. No GPU is needed for any mandatory
test; Mesa's software rasterisers (**lavapipe** for Vulkan, **llvmpipe** for GL)
exercise the identical `dlopen` path, and the cases that do need hardware are
SKIPPED by name without it.

```
podman 5.8.6 at %LOCALAPPDATA%\Programs\Podman\podman.exe   (NOT on PATH)
podman machine: podman-machine-default, a running WSL2 Fedora 44 VM
Python 3.13 on PATH as `py -3`
```

**There is a GPU, and reaching it is not obvious.** This machine has a discrete
NVIDIA RTX 3050 Ti Laptop (driver 580.97) and an Intel Iris Xe. WSL2 exposes
both through `/dev/dxg` paravirtualisation and bind-mounts the driver userspace
from Windows at `/usr/lib/wsl`; it publishes **no `/dev/dri` at all**, so `radv`,
`anv` and `radeonsi` are out regardless. Two flags put a container on the
silicon:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm \
  --device /dev/dxg -v /usr/lib/wsl:/usr/lib/wsl:ro \
  debian:trixie-slim sh -c 'LD_LIBRARY_PATH=/usr/lib/wsl/lib \
      /usr/lib/wsl/lib/nvidia-smi -L'
# GPU 0: NVIDIA GeForce RTX 3050 Ti Laptop GPU (UUID: GPU-df849629-...)
```

For **graphics**, the route is Mesa's `d3d12` Gallium driver, which needs no DRM
node and which Debian packages as `dri/d3d12_dri.so`:

```bash
GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
  xvfb-run -a -s '-screen 0 1024x768x24 +extension GLX +render' glxgears -info
# GL_RENDERER = D3D12 (NVIDIA GeForce RTX 3050 Ti Laptop GPU)   115 FPS
```

`MESA_D3D12_DEFAULT_ADAPTER_NAME` is the only way to choose between the two
GPUs; without it you get the Intel one, which is still hardware and still a
valid result, just not the one you probably meant.

`scripts/wsl-ephemeral.ps1` creates throwaway WSL2 distros from any OCI image
if you need a real init-less VM rather than a container.

**Bind mounts from Git Bash need `MSYS_NO_PATHCONV=1`**, otherwise Git Bash
mangles the container-side path:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm -v "$PWD:/repo:ro" alpine:3.22 sh -c '...'
```

`.tmp/` is gitignored scratch. `appimage.ps1` caches the demo AppImage and the
extracted AppDir there, so the second run is much faster than the first.

## 3. Reproduce the current state

### 3.1 The evidence table (3 minutes, the regression gate)

```powershell
.\experiments\run.ps1
```

Expect **36/36 predictions held**. Run this before every commit. A MISMATCH is
a finding, not a harness bug: investigate before coding.

Three container stages over one shared volume: `alpine:3.22` builds a faithful
musl probe, `debian:trixie-slim` (glibc 2.41) builds libraries needing new
symbols, `debian:bullseye-slim` (glibc 2.31) plays "an AppImage bundling an
older glibc" and runs the tests. The repo is mounted at `/repo`, so cases
E14-E29 build and test `src/` as it actually ships.

### 3.2 The musl symbol gap (Tier 0, no Linux needed)

```bash
mkdir -p /tmp/gap && cd /tmp/gap
PYTHONPATH=<repo> py -3 <repo>/gap.py --fetch
```

Expect the union over the Mesa+LLVM closure to be exactly
`['___environ', 'atexit']`.

### 3.3 The end-to-end proof (15 minutes the first time)

```powershell
.\experiments\appimage.ps1
```

Expect **40/40 on the glibc host** and **35/35 with five named skips on musl**.
It downloads the demo AppImage once into `.tmp` (sha256 verified), extracts it
inside a container because the payload is DwarFS, builds `src/` on the glibc
2.31 **floor**, builds the musl half of the ABI probe on Alpine, and then runs
the same suite on `alpine:3.22` and `debian:trixie-slim`.

**On a machine with no GPU the count is lower and that is correct.** The driver
probes for `/dev/dxg` plus a bind-mountable `/usr/lib/wsl` by running a
throwaway container, and when either is absent E41-E53 are SKIPPED with the
capability named. The five skips on Alpine here are the same mechanism: no host
glibc runtime to switch to (E51, E52) and no d3d12 driver (E53a, E53, E53b).

Every case runs the feature **off and on**, and against **both** the
`foreign-dlopen.so` shipped inside the AppImage and the one built from `src/`.
That is not thoroughness for its own sake: see section 6, where "it rendered
with the feature off" turns out not to mean what it looks like.

The headline rows, on Alpine:

```
E30  AppImage as shipped, feature on   NO-DEVICES  (segfault)
E31  control, feature off              NO-DEVICES  (VK_ERROR_INCOMPATIBLE_DRIVER)
E32  this repo, feature on             DEVICES     (llvmpipe)
E37a AppImage as shipped, vkcube       reported zero accessible devices
E37  this repo, vkcube                 Selected GPU 0: llvmpipe (LLVM 20.1.8)
E40  one file replaced, no variables    Selected GPU 0: llvmpipe (LLVM 20.1.8)
E41  NVIDIA's closed-source libcuda     4096 bytes round-tripped through the GPU
E43a bindings, AppImage as shipped     MIXED: 5 of 6 condvar entry points
E43  bindings, this repo               UNIFORM: 0 of 6
E44  the same, without the wsl dir     cuInit -> 100 CUDA_ERROR_NO_DEVICE
E46  the vendor's own nvidia-smi        GPU 0: NVIDIA GeForce RTX 3050 Ti Laptop
E46a the same binary, no AppImage       env: can't execute .../nvidia-smi
E49  a musl guest, 26 ABI crossings     ABI CROSSING PASSED
E59  every bundled loader classified    8 import dlopen, 0 unclassified
E61  glxgears, no GL shim               couldn't get an RGB, Double-buffered visual
E62  glxgears, with it                  GL_RENDERER = llvmpipe (LLVM 20.1.8)
E63  glprobe, no GL shim                FAILED: no RGB double-buffered visual
E64  glprobe, with it                   readback rgba 64 128 191 255, OK: GL is complete
E65  eglprobe, GL shim only             FAILED: eglGetDisplay -> EGL_NO_DISPLAY
E66  eglprobe, GL + EGL shims           OK: EGL is complete
E67  vkcube with both shims loaded      Selected GPU 0: llvmpipe
E68  the shim pointed at itself         refusing to forward to ourselves
```

E61 through E66 are the ones to read after E40. Every case above them measures
the libc gap; those six measure the other one, and the difference between E62
and E64 is the difference between a shim that makes `glxgears` run and a shim
that replaces a library.

and three more on Debian only, which is where there is a host glibc runtime to
switch to and a hardware GL driver to drive:

```
E51  Design R, then a Vulkan device    OK: 1 physical device
E52  Design R, then the CUDA round trip on the GPU
E53a the AppImage on the d3d12 driver  Error: glXCreateContext failed
E53  the same, plus the conf dirs      GL_RENDERER = D3D12 (NVIDIA RTX 3050 Ti)
```

E40 is the one to look at first. Every other case forces something -- the
feature, the ICD, the loader. E40 replaces `lib/foreign-dlopen.so` inside the
AppDir and runs it with no `ANYLINUX_*` and no `VK_DRIVER_FILES` at all, which
is the only form of the claim that matches what was asked.

### 3.4 Driving it by hand

The suite is the reproducible form, but when you are debugging you want the
pieces. Build the preload on **bullseye** so it only needs old symbols, drop it
into the AppDir, and run against Alpine:

```bash
# in debian:bullseye-slim, with the repo mounted
cd src && make                      # foreign-dlopen.so, gl-fwd.so, egl-fwd.so,
                                    # runtime-select

# in alpine:3.22, with AppDir and the built .so mounted
apk add --no-cache mesa-vulkan-swrast vulkan-tools vulkan-loader
export APPDIR=/w/AppDir XDG_RUNTIME_DIR=/tmp/xdg
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
mkdir -p $XDG_RUNTIME_DIR

# A/B: the same command, feature off then on. Never trust a single-sided run.
ANYLINUX_LIB_FOREIGN_DLOPEN=0 \
  "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib" ./vkprobe

ANYLINUX_LIB_FOREIGN_DLOPEN=1 \
  "$APPDIR/lib/ld-linux-x86-64.so.2" --library-path "$APPDIR/lib" \
  --preload "$APPDIR/lib/foreign-dlopen.so" ./vkprobe
```

> Use `ld.so --preload` rather than `LD_PRELOAD` when a musl binary is anywhere
> in the pipeline (`strace`, `env`). `LD_PRELOAD` applies to those too, and
> musl's loader cannot load a glibc `.so`.

Test programs, all built on a glibc host and run under the bundled loader:

| File | What it does |
|---|---|
| [`tests/icd-harness.c`](tests/icd-harness.c) | foreign-load one ICD, resolve `vk_icdGetInstanceProcAddr`, call through it |
| [`tests/vkprobe.c`](tests/vkprobe.c) | bundled Vulkan loader plus host ICD, `vkCreateInstance` then `vkEnumeratePhysicalDevices`, no window system |
| [`tests/corpus.c`](tests/corpus.c) | `dlopen` every `.so` in a directory, print OK/FAIL with the reason |
| [`tests/invariants.c`](tests/invariants.c) | one libc family in `/proc/self/maps`, bundled sonames win |
| [`tests/soak.c`](tests/soak.c) | N load/unload cycles with RSS, fd and rewritten-copy counts |
| [`tests/verprobe.c`](tests/verprobe.c) | the version-binding trap as a loadable probe: returns 0 or 22 |
| [`tests/vertrap.c`](tests/vertrap.c) | the three libc properties `version-compat.c` rests on |
| [`tests/allocprobe.c`](tests/allocprobe.c) | interpose the allocator family; every counter carries its total |
| [`tests/glprobe.c`](tests/glprobe.c) | GL past the 33 symbols `glxgears` links, then clears to a known colour and reads the pixel back |
| [`tests/eglprobe.c`](tests/eglprobe.c) | the same question asked of EGL, surfaceless, with no X server at all |
| [`tests/cudaprobe.c`](tests/cudaprobe.c) | foreign-load a closed-source vendor driver, then push bytes to the GPU and read them back |
| [`tests/bindprobe.c`](tests/bindprobe.c) | walk every loaded object's relocations and report which DEFINITION each one bound |
| [`tests/abi-host.c`](tests/abi-host.c) | the cross-libc crossings, against [`abi-guest.c`](tests/abi-guest.c) built by the other libc |

Reaching the GPU by hand needs two flags on the container and one directory on
the library path, none of which is guessable:

```bash
MSYS_NO_PATHCONV=1 "$PODMAN" run --rm \
  --device /dev/dxg -v /usr/lib/wsl:/usr/lib/wsl:ro \
  -v "$PWD:/repo:ro" -v "$PWD/.tmp:/w" alpine:3.22 sh -c '
    APPDIR=/w/AppDir; LP=$APPDIR/lib
    ANYLINUX_LIB_FOREIGN_DLOPEN=1 APPDIR=$APPDIR \
      "$LP/ld-linux-x86-64.so.2" --library-path "$LP:/usr/lib/wsl/lib" \
      --preload "$LP/foreign-dlopen.so" \
      /w/build/cudaprobe /usr/lib/wsl/lib/libcuda.so.1'
```

Drop `:/usr/lib/wsl/lib` and it still loads, still resolves every entry point,
and then reports `cuInit -> 100 CUDA_ERROR_NO_DEVICE`. That is E44, and it is
the single most misleading failure in this repository.

## 4. What was blocking it, and what is left

### 4.0 START HERE: the priority for the next session

The question asked of the last session was: **did you get this to work on
systems without glvnd?** The honest answer is **yes, but**, and this subsection
is the list of "buts" in the order they should be closed. Everything else in
section 4 is blocked by hardware or by packaging. This list is not: every item
on it is work that can be done on this machine, and each one narrows a claim
that is currently wider than its evidence.

**What is actually demonstrated.** On alpine:3.22 and alpine:3.15 -- musl,
classic Mesa, no `libGLX_<vendor>.so.0` anywhere -- the demo AppImage's bundled
glibc 2.44 drives the host's GL: `glxgears` renders (E62), `glprobe` clears to a
known colour and reads `64 128 191 255` back out of the framebuffer (E64), and
`eglprobe` gets a working surfaceless context (E66). On a glvnd host all three
are unchanged, so the shim is transparent where GL already worked. That is a
real result and it is not in doubt.

**What is not demonstrated, in priority order.** ⭐ The titles below are how
each item has been referred to, so they do not change; what an item's row says
after it closes is written under the table in 4.0.1, including where a premise
in the row turned out to be wrong.

| # | status | the "but" | why it matters | how to close it |
|---|---|---|---|---|
| **B1** | ✅ CLOSED | **1097 of 3470 entry points return zero and say nothing.** They forward to `glfwd_absent`. An application that links one gets a silent no-op, not a diagnostic | This is the difference between "works" and "works for the applications tried". A silent zero is the failure mode this repository spends the most words warning about, and the shim now has one by construction | Make the absent case observable without making it fatal: a one-line-per-name report at first call under `ANYLINUX_LIB_DEBUG=1`, which needs the register-saving resolver stub described in B2. Then measure which of the 1097 a real application actually touches -- likely zero, and "likely" is the problem |
| **B2** | ✅ CLOSED | **The trampolines cannot report, because a table slot cannot run code.** The current design fills the table in a constructor and every slot is either a real address or a silent stub | It also forces the eager load in B4 and blocks per-symbol laziness | Write the x86-64 register-saving resolver: save `rdi rsi rdx rcx r8 r9 xmm0-7 rax`, call a C resolver with the index, restore, tail-jump. This is `_dl_runtime_resolve` minus the bookkeeping, ~60 lines, and it is the single change that unlocks B1 and B4 |
| **B3** | ✅ CLOSED | **Only ONE non-glvnd host family has been tested: musl Alpine.** The other half of the claim -- pre-glvnd **glibc** distros, Ubuntu 14.04/16.04, Debian 8 -- is asserted, not measured, here | The README and REPORT 9 both say "every musl distro, and every pre-glvnd glibc distro". Half of that sentence has evidence | `ubuntu:14.04` and `ubuntu:16.04` still exist on Docker Hub; their apt repositories moved to `old-releases.ubuntu.com`, which is a `sources.list` rewrite, not a blocker. Add them as a third and fourth host to `appimage.ps1` and run E59-E68 there. Mesa 10.1 is also the one stack where the `_glapi_tls_Dispatch` case in REPORT 9.5 might reproduce, which would close a second open question at the same time |
| **B4** | ✅ CLOSED | **The shims load the host GL stack in every process, GL or not.** Measured cost: +30 ms and +30 MB on a Vulkan-only run | Small, but it is 30 MB of host Mesa mapped into a process that will never call it, and the reason it is not gated is that the gate was judged too dangerous to write. With B2 done it becomes trivial: nothing resolves until something calls | Gate on first call, via B2's resolver. Delete the constructor's `dlopen` entirely |
| **B5** | ✅ CLOSED | **No GLES shim.** `libGLESv2.so.2` and `libGLESv1_CM.so.1` are the same shape and are not covered | An AppImage bundling Mesa's GLES has the identical gap and would fail identically. The generator and the shim already do everything needed; this is a table and two `-D` flags | `make gl-syms` against a bundled `libGLESv2.so.2`, add the build rule, add an E-case. Note the demo AppDir bundles neither, so this needs an AppDir that does -- see B6 |
| **B6** | ✅ CLOSED | **Two applications, one of which I wrote.** `glxgears` (33 GL symbols) and `glprobe` (15). Nothing real | 3470 forwarded entry points have been exercised at a rate of about 1%. The claim "it replaces libGL" rests on the export count, not on use | Build a demo AppDir around something with a real GL surface. `pkgforge-dev/Anylinux-AppImages` `useful-tools/demo/` has recipes for gtk3/gtk4/qt6/sdl/webkit2gtk AppImages; any of those on Alpine is a far harder test than `glxgears` |
| **B7** | ⛔ BLOCKED | **Never on real silicon on the non-glvnd path.** Every GL result on Alpine is llvmpipe under Xvfb | The d3d12 path (E53) proves hardware GL works through the AppImage on a **glvnd** host. The classic-Mesa path has no hardware result at all | Alpine has no `d3d12_dri.so` (E53a's skip reason). Either build Mesa's d3d12 Gallium driver for Alpine, or accept this as hardware-blocked and say so in one sentence instead of leaving it implied |
| **B8** | ✅ CLOSED | **aarch64 trampolines assemble and have never run.** `make gl-fwd-asm-check` produces correct instructions and relocations and proves nothing else | The repository already carries this caveat for `RS_LDSO` and `RS_TRIPLET`; `gl-fwd` adds hand-written assembly to it, which is a larger thing to be unverified | Hardware, or a qemu-user run under `--platform linux/arm64`. The second is cheap and worth trying before declaring it blocked |
| **B9** | ✅ CLOSED | **The shim guessed where the host keeps its libraries.** A hardcoded directory list had `<triplet>/mesa` and not `<triplet>/mesa-egl`, so EGL failed on every pre-glvnd Ubuntu while GL worked | Reported from outside ([PR #4](https://github.com/Azathothas/dlopen-experiment/pull/4)). Not a missing entry: a guess about somebody else's packaging, which had already drifted from the path sharun assembles | Derive the list from `/etc/ld.so.conf` instead, sharing one walk with `runtime-select.c`. See 4.0.1 |

⭐ **B2 is the keystone.** B1 and B4 both reduce to it, and it is the one piece
of genuinely new machinery. B3 is the highest-value item that needs no new
machinery at all.

⛔ **Do not start the port until B1, B2, B3 and B6 are closed.**
[`PORTING.md`](PORTING.md) is written and waiting, and it is deliberately a
separate session with a separate agent. Porting a claim that is wider than its
evidence just publishes the gap.

### 4.0.1 Closure records

One entry per item that has closed, with the command that proves it and the
output it produced. ⛔ Where a premise in the row above turned out to be wrong,
the correction is here and the row keeps its wording, because the row is how
the item has been referred to everywhere else.

#### B2 ✅ -- the resolver exists, and it is measured

`src/gl-fwd.c`. Every slot now starts at `glfwd_resolve_asm` instead of at an
address, and each trampoline carries its own index in a register the ABI
already lets a call destroy -- `%r11` on x86-64, `x17`/IP1 on aarch64:

```asm
glClearColor:
	endbr64
	mov    $0xc1, %r11d            # 193, and glfwd_tab+0x608 is 8*193
	jmp    *0x296b8(%rip)
```

The resolver saves `rax rdi rsi rdx rcx r8 r9 r10 xmm0-7`, calls
`glfwd_resolve_one(index)`, restores and tail-jumps. `and $-16,%rsp` makes the
alignment unconditional rather than argued, because a trampoline is reached
from anywhere and the `movaps` faults on a misaligned address.

Measured by **E69-E73** in `experiments/run.ps1`, section N, against the real
`src/gl-fwd.c` built with a five-name table -- not a copy of the resolver that
could drift from it:

```
E69  OK: first-call ints=204 floats=285.00 varargs=10 struct=[2..12]
         second-call-identical=yes absent-returned=0
```

Eight integer registers, nine float registers, a varargs `%al` count and a
struct returned through hidden memory, all surviving a C call made in the
middle of the forward -- and the second call agreeing with the first, which is
what says the slot was patched with the right address rather than that the
resolver got lucky once.

#### B4 ✅ -- nothing loads until something calls

The constructor's `dlopen` is gone; `glfwd_ensure_target()` runs at the first
call through any slot. `ANYLINUX_GL_FWD_EAGER=1` restores the old behaviour,
so the cost of not doing it stays a measurement rather than a memory.

Asked of `/proc/self/maps`, because "it started faster" is not evidence about
what was loaded, and asked on **both** sides -- E71 alone would also pass if
the shim were simply broken:

```
E71   OK: shim mapped=1 target mapped=0 (called=-1)     no call
E71b  OK: shim mapped=1 target mapped=1 (called=204)    one call
```

and at AppImage scale, on both host classes, in `appimage.ps1`:

```
E74   Vulkan-only run: 2 shim(s) loaded, 0 resolved, no host GL mapped
E74b  the same shims, after a GL call: 2373 of 3470 entry points resolved
```

⚠ **The row's "+30 ms and +30 MB" is now the cost of `ANYLINUX_GL_FWD_EAGER=1`
and not of the default.** REPORT.md 9.9 is rewritten to say which.

#### B1 ✅ -- and the answer to the question the row could not ask

The absent case is now a line at the first call of that name, under
`ANYLINUX_LIB_DEBUG=1`, and not fatal -- returning zero is what the application
would get natively on a host where the name is equally absent:

```
E72   [gl-fwd.so] >> ABSENT entry point called: t_absent -- this host's
                     libtgt.so has no implementation; returning zero
```

The row asked for something the old design could not measure at all: *which of
the 1097 does a real application actually touch -- likely zero, and "likely" is
the problem.* On alpine:3.22, `glprobe` through the full AppDir:

```
libGL.so.1: 2373 of 3470 entry points resolved from the host library
            (1357 exported, 1016 via glXGetProcAddressARB, 1097 absent)
libGL.so.1: 15 of 3470 entry points were CALLED (15 forwarded, 0 absent)
            out of 2373 this host could resolve
absent entry points this application reached: 0
```

**Zero, measured.** ⭐ And the second line is the number B6 has been guessing
at: `glprobe` touches **15 of 3470**, which is 0.4%, not the "about 1%" B6's
row estimates. That number is reported and never thresholded -- it is a
property of the application, and a bar here would be a bar on somebody else's
program.

#### B3 ✅ -- the other host class, measured, and one premise corrected

⚠ **The row's route is wrong and would have stopped you.** It says these
images' repositories "moved to `old-releases.ubuntu.com`, which is a
`sources.list` rewrite, not a blocker". As of 2026-08 `old-releases` does not
carry `trusty` or `xenial` **at all** -- its `dists/` listing jumps from
`saucy` to `utopic` and every path 404s. Both releases are still inside their
ESM window and are still served from **`archive.ubuntu.com` at the default
path**, so the prescribed rewrite is the thing that breaks them. What does have
to go is the image's own ESM source: it points at `esm.ubuntu.com`, needs
credentials, and apt fails the whole update over it and then reports every
package as "unable to locate", which reads exactly like a dead mirror.

```sh
rm -f /etc/apt/sources.list.d/*esm*      # and leave sources.list alone
```

`experiments/46-host-ubuntu.sh` is the third and fourth host; `appimage.ps1`
runs all four by default and `-Only ubuntu1404` runs one.

```
ubuntu:14.04   glibc 2.19   Mesa 10.1.3   26/26, 19 named skips
ubuntu:16.04   glibc 2.23   Mesa 18.0.5   26/26, 19 named skips
```

Both are classic: no `libGLX_<vendor>.so.0` anywhere. On 14.04 `glxgears`
renders (`Gallium 0.4 on llvmpipe (LLVM 3.4)`), `glprobe` reads its pixel back,
and `eglprobe` gets a context. **The pre-glvnd glibc half of the claim has
evidence now.**

⭐ **And the resolution counts match an independent run on hardware nobody here
has.** [issue #1](https://github.com/Azathothas/dlopen-experiment/issues/1)
reported Ubuntu 14.04 from a seven-distro matrix on a real RX 580:

```
reported : libGL.so.1: 1889 of 3470 resolved (1405 exported, 484 via glXGetProcAddressARB, 1581 absent)
measured : libGL.so.1: 1889 of 3470 resolved (1405 exported, 484 via glXGetProcAddressARB, 1581 absent)
```

Same numbers, different hardware, different display path, different Mesa point
release. That is a prediction that held.

**Two things the row hoped for did not happen, and one it did not expect did.**
The `_glapi_tls_Dispatch` case in REPORT 9.5 did **not** reproduce on Mesa
10.1: GL works there, and nothing needed the global scope to do it. What
happened instead is on 16.04, and it is worth more -- see B3's second finding
below.

#### B3's second finding: the `ld.so.cache` blindness, fourth sighting

Ubuntu 16.04 failed three cases, and the failure was not the shim. Its host
`libGL.so.1` loads, the shim resolves 2354 of 3470 entry points from it, and
then Mesa `dlopen`s its own `swrast_dri.so`, which needs `libLLVM-6.0.so.1` --
reachable on that host **only** through `/etc/ld.so.cache`, which the bundled
`ld.so` is patched not to read (E13b). What the user sees:

```
libGL error: unable to load driver: swrast_dri.so
X Error of failed request:  BadValue
  Major opcode of failed request:  151 (GLX)
  Minor opcode of failed request:  3 (X_GLXCreateContext)
```

A display fault, apparently. It is the same bug as `CUDA_ERROR_NO_DEVICE`
(E44) and `glXCreateContext failed` (E53a). **E77** now measures it on every
host, and what it scores is the DIAGNOSTIC rather than the outcome -- the
outcome is a property of how a host packages its driver, but "when this bites,
the process names the library it could not find" is true everywhere.

#### B3's third finding: predict what the HOST does, not what you hoped

`eglprobe` failed on 16.04 with the shims. So it does **natively**, with no
AppImage, no preload and no shim in the process:

```
native eglprobe on ubuntu:16.04    EGL_VERSION : 1.4   EGL_VENDOR : Mesa Project
                                   readback rgba : 0 0 0 255 (want ~64 128 191 255)
                                   FAILED: the pixel does not carry the colour that was set
```

Mesa 18.0.5 does not produce that pixel on that host at all. A shim that then
produced it would be inventing one. ⛔ So **E78 and E79 build and run the
probes natively and E64/E66 are predicted against THAT**, not against a
constant: the shim's claim is transparency, so the yardstick is the host. This
also corrects a hypothesis offered in the issue -- that 16.04's readback fails
because the GL and EGL shims do not share dispatch state. There are no shims in
the native run.

#### B5 ✅ -- the GLES dispatcher, from an AppDir that has one

`src/gl-fwd-gles2.h`, **358 entry points**, read out of the `libGLESv2.so.2`
bundled by the gtk4 demo AppImage; `make gles-syms GLES=<dir>` regenerates and
`make gles-syms-check` fails on drift. The row is right that it is "a table and
two `-D` flags" -- and right that it needed an AppDir that bundles GLES, which
is why it waited for B6.

GLES finds its implementation the way EGL does, through
`/usr/share/glvnd/egl_vendor.d`, so `gles-fwd.so` is the same source file with
EGL's vendor marker and its own table.

⭐ It is not a completeness exercise. **GTK4 renders through GLES**: E83
measures gtk4-demo calling 46 distinct GLES entry points, 13 EGL and 1 GL. On a
classic host without this shim, those 358 names are 358 silent zeros.

`libGLESv1_CM.so.1` is **not** done, and not because it is hard: no AppImage
available here bundles one, and the generator's rule is that the list comes out
of the object being replaced. One `make gles-syms` against an AppDir that has
one is the whole job.

#### B6 ✅ -- a real application, and it found a bug

`experiments/47-gtk4.sh`, a fifth stage: the gtk4-demo AppImage -- 272
libraries, its own Mesa, its own `libEGL_mesa.so.0`, a real GTK4 application --
on musl Alpine. This is the other SHAPE of AppImage, self-contained rather than
host-drivers, and four synthetic cases and two host classes had never seen one.

**It failed, and the shim was wrong.** `glfwd_host_has_vendor()` asked only
whether the HOST had a vendor library. On Alpine it does not, so the shim
forwarded a bundled GTK4 stack onto Alpine's Mesa: two Mesas in one process,
`SIGFPE`. The same AppImage with no shim in `.preload` ran fine, which is what
made it a shim bug and not a host one.

The repair is `glfwd_bundle_has_vendor()`: if the BUNDLE carries its own vendor
library, the bundled dispatcher is what the application was built and tested
against and the shim leaves it alone. That is also what makes this shim safe to
put in *every* AppImage's `.preload` rather than only in the host-drivers ones.

```
E80a  as shipped, no shims          rc=143  (still running when the timeout ended)
E80   gl + egl + gles shims         rc=143  (was 136/SIGFPE)
E81   target chosen                 the bundled dispatcher, because the BUNDLE
                                    has its own vendor library
E82   3470 of 3470, 44 of 44, 358 of 358 entry points resolved
E83   gtk4-demo called 1 GL, 13 EGL and 46 GLES entry points
```

⭐ **E83 is the number the row is about.** "3470 forwarded entry points
exercised at about 1%" was an estimate; the measured figures are `glprobe` at
15 of 3470 (0.4%) and gtk4-demo at 46 GLES of 358 (13%) -- and the useful part
is not the percentage, it is that **the application's renderer was GLES**, a
dispatcher this repository did not cover until this AppDir arrived.

#### B7 ⛔ -- hardware-blocked here, and the reason is structural

The row asks for one sentence rather than an implication, so: **there is no
host on this machine that is both classic-Mesa and able to reach a GPU, and
that is not an accident of what is installed.** Measured, not assumed:

```
alpine:3.22  /usr/lib/dri: crocus i915 iris kms_swrast libdril nouveau r300
             r600 radeonsi swrast virtio_gpu vmwgfx zink -- no d3d12_dri.so
/usr/lib/wsl/lib: libcuda, libnvidia-*, libd3d12.so, libdxcore.so
             -- no libGL, no libGLX_nvidia.so.0, no libEGL
```

The only GPU route here is Mesa's `d3d12` Gallium driver over `/dev/dxg`, which
needs Mesa ≥ 21. Every glibc distro shipping Mesa ≥ 21 uses libglvnd, and the
classic-Mesa holdouts are the musl distros, which do not build `d3d12`. The two
properties are anti-correlated, so this is not "wait for a package".

⭐ **It has been measured elsewhere, and that is recorded as elsewhere.**
[issue #1](https://github.com/Azathothas/dlopen-experiment/issues/1) reports
`glprobe` passing on Alpine 3.21 with hardware `radeonsi` on an RX 580 -- GL on
real silicon on the classic path. Not reproduced here, not adopted as if it
were, and named the same way REPORT 9.5 names the Mesa 10.1 report it also
could not reproduce.

#### B8 ✅ -- the aarch64 trampolines have now run

`make gl-fwd-qemu-check`, and **E76/E76b** in `run.ps1` section P. qemu-user
runs an aarch64 binary on an x86_64 kernel in userspace, and everything under
test is userspace: the trampoline, the resolver, ld.so binding a `DT_NEEDED` to
a preloaded object with the same SONAME, and `dlopen`.

```
t_ints:
    bti  c
    mov  w17, #0x0
    adrp x16, glfwd_tab
    ldr  x16, [x16, #272]
    br   x16

E76   OK: first-call ints=204 floats=285.00 varargs=10 struct=[2..12]
      second-call-identical=yes absent-returned=0
E76b  ABSENT entry point called: t_absent
```

⚠ **Not the row's route.** `podman run --platform linux/arm64` replaces the
cached image for that tag, and one probe left `alpine:3.22` resolving to arm64
and killed the next suite run with `Exec format error`. Naming
`qemu-aarch64-static` needs no binfmt registration -- a kernel-wide setting --
and no privilege.

Still UNVERIFIED: aarch64 **silicon**. qemu-user emulates the instructions, not
a real memory model or a real Mesa.

#### B9 ✅ -- a new item, from outside: the shim stops guessing where libraries are

Not in the original eight. [PR #4](https://github.com/Azathothas/dlopen-experiment/pull/4)
reported that `egl-fwd.so` cannot find the host's classic `libEGL.so.1` on
Ubuntu's alternatives layout: `glfwd_host_dirs[]` listed `<triplet>/mesa`,
where classic `libGL` lives, and not `<triplet>/mesa-egl`, where classic
`libEGL` lives. EGL therefore failed on every pre-glvnd Ubuntu while GL worked.

The PR's own follow-up made the better argument: a hardcoded list of somebody
else's packaging conventions is a guess, it had already drifted from the path
sharun assembles, and adding an entry treats the symptom. Section 7 says the
same thing -- gl-fwd's list is "the one deliberate exception, and it is
bounded... it must not grow into one".

So the list is now **derived**. [`src/ld-conf.h`](src/ld-conf.h) is one walk of
`/etc/ld.so.conf`, shared by `gl-fwd.c` and `runtime-select.c` so there is one
parser rather than two, and the shim looks in this order:

1. `ANYLINUX_GL_HOST_DIR` -- the explicit handoff a launcher can use
2. every directory `/etc/ld.so.conf` and its includes name -- **the host's own
   answer**, which on Ubuntu is `x86_64-linux-gnu_EGL.conf` naming `mesa-egl`
3. the conventional list, which now includes PR #4's `mesa-egl` entries --
   still needed, and not in name only: musl distros have no `/etc/ld.so.conf`
   at all, so on the very host class this shim exists for, list 3 is the only
   one that answers

**E75/E75b** measure it on a directory no list could contain, so a pass cannot
come from the hardcoded entries, and the control is the conf file's absence
rather than a switch invented to disable the feature:

```
E75   target /opt/anylinux-unguessable-42/libtgt.so     conf file present
E75b  no target; all 5 entry points return zero          conf file removed
```

### 4.1 The blocker, now fixed

`vkEnumeratePhysicalDevices` returned `VK_ERROR_OUT_OF_HOST_MEMORY` with zero
devices. This was attributed to glibc-vs-musl ABI differences. **It was not
that.**

The measurement that broke it open was reproducing the failure on
`debian:trixie-slim` with **one libc**: Debian's own glibc-built
`libvulkan_lvp.so`, glibc 2.41 on both sides, no musl anywhere. Then the chain
fell out in an afternoon, because Debian ships Mesa's `__FILE__` strings and
`mesa-vulkan-drivers-dbgsym` exists:

```
lvp_device.c:1315            lvp_init_wsi() failed
wsi_display_init_wsi()       -> VK_ERROR_OUT_OF_HOST_MEMORY
wsi_common_display.c:2323    u_cnd_monotonic_init() -> thrd_error
                             pthread_cond_init() -> 22 (EINVAL)
gdb, info symbol $pc         libc+0x909f0 = pthread_cond_init@GLIBC_2.2.5
                             (the working run: libc+0x91b00 = @@GLIBC_2.3.2)
```

`pthread_cond_init@GLIBC_2.2.5` is the pre-2003 compat definition and its whole
body is `if (cond_attr != NULL) return EINVAL;`.

**An unversioned reference does not get the default definition of a symbol.**
A version-stripped object has only unversioned references. So does every
musl-built object, which never had version information at all. That is why the
same failure showed up on Alpine, on Gentoo with a glibc `radv`, and on Debian
once the ICD manifest named an absolute path.

The fix is [`src/version-compat.c`](src/version-compat.c) plus
[`tools/version_traps.py`](tools/version_traps.py); REPORT.md 6.2 has the whole
chain with the commands.

### 4.2 What is genuinely BLOCKED

⭐ **This list and 4.0 are different lists and must stay different.** 4.0 is
work that can be done on this machine. Everything here is blocked by hardware,
by what a distribution ships, or by the permission rule in section 8, and
nothing on it is merely unwritten. If you find yourself with a machine that
unblocks a row, that row is the work; if not, the honest thing is to verify what
is here rather than add to it.

⚠ Three rows that were on this list moved to 4.0 and are now CLOSED, because
they turned out not to be blocked: the aarch64 trampolines RUN under qemu-user
(B8), the absent GL entry points are observable at the call (B1), and the
`_glapi_` case had a host that settled it (B3 -- it did not reproduce on Mesa
10.1). They are named here only so nobody restores them.

⚠ **`B7` is on this list now**, having come the other way. Its closure record
in 4.0.1 has the two `ls` commands that establish it and the reason it is
structural rather than a packaging accident.

| Item | Why it is not done | What would unblock it |
|---|---|---|
| **Vulkan on hardware** | Mesa's Vulkan-on-D3D12 driver (`dzn`) is `microsoft-experimental` and Debian does not package it, so every ICD result here is lavapipe. OpenGL *is* on hardware now (E53), through the `d3d12` **Gallium** driver, which Debian does package as `dri/d3d12_dri.so` | build Mesa with `-Dvulkan-drivers=microsoft-experimental`, then re-run the suite against that ICD. Watch for `libd3d12.so` being glibc-built: on Alpine the chain becomes musl-Mesa on a glibc D3D12 layer |
| DRM-native drivers (`radv`, `anv`, `radeonsi`) | there is no `/dev/dri` anywhere on this machine. WSL2 publishes no DRM render nodes, so these three cannot initialise however much silicon is present | a non-WSL Linux host |
| The unmeasured plugin boundaries | `libva`, `libvdpau`, `libasound`, `libpulse`, `libOpenCL` and `libgbm` are the same shape as the OpenGL one and this AppDir bundles none of them, so there is nothing here to measure. `tools/plugin_boundaries.py` classifies them on sight if one turns up. `libX11.so.6` IS bundled and its loadable-i18n boundary is unmeasured. ⚠ B6's gtk4 AppDir has 272 libraries and has NOT been run through `plugin_boundaries.py`; doing so is the cheapest way to find the next boundary and it is not blocked | an AppImage that bundles one of them. The gtk4 AppDir may already be one |
| **Hardware GL on the CLASSIC-Mesa path (B7)** | there is no host on this machine that is both classic-Mesa and able to reach a GPU. The only GPU route here is Mesa's `d3d12` Gallium driver over `/dev/dxg`, which needs Mesa >= 21; every glibc distro at Mesa >= 21 uses libglvnd, and the classic holdouts are musl distros that do not build `d3d12`. Measured: Alpine 3.22 ships no `d3d12_dri.so`, and `/usr/lib/wsl/lib` ships no GL, GLX or EGL at all | a machine with a DRM render node and a classic-Mesa distro. Reported working on an RX 580 from outside ([issue #1](https://github.com/Azathothas/dlopen-experiment/issues/1)), not reproduced here |
| **Ubuntu 12.04's EGL (B10)** | Mesa 8.0.4 ships EGL 1.4 and, per the same outside report, `eglInitialize` fails there even with the right directory. Not measured here -- 12.04 is on `old-releases` and was not added as a host -- and it is the host's Mesa either way: 16.04's EGL fails the same probe NATIVELY, with no AppImage in the process (E79) | nothing in this repository. An AppImage cannot give a host an EGL implementation it does not have |
| No host implementation for 1097 of the GL entry points | a property of Alpine's Mesa 25.1, not of this repository: they are extensions glvnd knows the names of and that Mesa has no code for. Making the absent case OBSERVABLE was B1 and is DONE -- a call to one is a line naming it, and `glprobe` reaches zero of the 1097 (4.0.1). Making Mesa implement them is not this project's work | nothing here. See B1 |
| The two live ABI hazards | `regoff_t` is 4 bytes on glibc and 8 on musl, and the `FTW_*` values are off by one, so a musl-built object reads a glibc-filled `regmatch_t[]` or classifies an `nftw` entry wrongly (E50). An offset compiled into an object is not reachable from a preload | nothing in this repository. It is a property of the two libcs, and the useful output is the list of two, which E50 keeps honest |
| Three residual library-path gaps upstream | the sharun fix is **upstreamed** ([Anylinux-sharun@`54208d2`](https://github.com/pkgforge-dev/Anylinux-sharun/commit/54208d2bc7d4c919ba46a6c234f6af7f8426b537)) and the patch here is deleted. What that change does not reach is musl's `/etc/ld-musl-<arch>.path`, multiarch triplets past three, and the non-FHS prefixes; `analysis/ground-truth.md` has the measurement | a different repository, and section 8 forbids writing there |

### 4.3 What the last session closed, so you do not redo it

⭐ **Section 4.0 is closed except B7, which is hardware-blocked and says so in
one sentence.** Every closure is written under the item it closes, in 4.0.1,
with the command that proves it and the output it produced. What follows is the
shape of the session, not a substitute for reading those.

**The keystone was one instruction.** A table slot is an address, so nothing
could happen AT a call; the repair is that each trampoline now carries its own
index in a register the ABI already lets a call destroy (`%r11`, `x17`), and an
unresolved slot points at a register-saving resolver where that index is the
whole message. B1, B4 and the two counters that answer B6 all reduce to it, and
it is about sixty lines of assembly.

**Three of the eight items were answered by measuring rather than by building.**
B3 needed two container images and a corrected premise. B7 needed two `ls`
commands and the honesty to say the two properties are anti-correlated. B8
needed `qemu-aarch64-static` instead of the `--platform` flag the row suggested.

**The two most valuable results came from things that had never been run.**

- **A real application found a real bug.** gtk4-demo -- 272 bundled libraries,
  its own Mesa -- died with SIGFPE under the shims and ran fine without them,
  because `glfwd_host_has_vendor()` asked only about the host and hijacked a
  self-contained AppImage onto Alpine's Mesa. Four synthetic cases and two host
  classes had never seen an AppImage of that shape. B6.
- **A native control settled an attribution that both a maintainer and I had
  got wrong.** `eglprobe` fails on Ubuntu 16.04 with the shims -- and natively,
  with no AppImage in the process at all. E64 and E66 are now predicted against
  what the host does rather than against a constant, because the shim's claim
  is transparency and the yardstick for transparency is the host.

**Two items came from outside and are not in the original eight.** B9 is a
defect reported in a PR, and the PR's own follow-up made the better argument:
the shim's hardcoded directory list was a guess about somebody else's
packaging, it had drifted, and the repair is to read `/etc/ld.so.conf` --
which `runtime-select.c` already did, so the two now share one walk
([`src/ld-conf.h`](src/ld-conf.h)). B10 is a host limitation to record rather
than patch, and it is in 4.2.

**One harness lesson is worth more than any of the code.** Sections 5's new
entries are all from this session and all the same shape: a measurement that
changed something it was not supposed to change. Hand-debugging left the shims
in the shared `.tmp/AppDir` and the next full run reported hardware failures
that were not happening. An aarch64 probe replaced a cached image and the suite
died on `Exec format error`. Adding the host's library directories to every GL
case made `glxgears` render on Alpine **with no shim at all**, through the X
server's own GLX -- which would have looked like a triumph and was the controls
quietly ceasing to control anything.

**What the next session should NOT redo:**

- `old-releases.ubuntu.com` for 14.04/16.04. They are on `archive.ubuntu.com`.
- Adding `mesa-egl` to `glfwd_host_dirs[]`. The list is derived now.
- Forcing `EGL_PLATFORM=surfaceless` and reading a Mesa 18 failure as a bug.
- Looking for a classic-Mesa host with a GPU on this machine. There is none.

## 5. Things that will waste your time

Each of these cost real time here. They are recorded so they cost you none.

### 5.0 How a whole gap hid for a session, and what to do about it

This one is not a technique, it is the reason the previous handover was wrong,
and it will happen again in a different place unless you know its shape.

`glxgears` on Alpine was recorded like this:

```
E38  SKIPPED  no libGLX_<vendor>.so.0 on this host; its Mesa is not libglvnd,
              so the bundled libglvnd has no vendor to dlopen
```

with the README adding: *no loader shim can supply a file the distribution does
not ship.* The skip reason was measured and correct. The sentence after it was
neither, and it turned the whole thing into a closed question for a session. It
is now closed the other way: a shim cannot supply the missing file, but it can
replace the object that was looking for it, and section 9 of REPORT.md is the
whole chain.

Four habits come out of that, in descending order of how much they would have
helped:

- **A SKIP names a missing capability and stops.** It may say "this host has no
  X". It may not say "and therefore nothing can be done", because that is a
  claim about the design space, it needs its own evidence, and welded to a
  measured fact it inherits the measured fact's authority. Every "not fixable",
  "cannot be", "no ... can" in a skip reason or a "what is not done" entry is a
  separate claim; go and look for the one you wrote last.
- **Scope by the user's outcome, not by your mechanism.** "This project fixes
  libc mismatches" made "host packaging, not libc" a reason to stop. The person
  running the AppImage does not know which side of that line their black screen
  is on. The two gaps in section 1 both produce the same symptom and only one of
  them is about libc.
- **Enumerate the class, do not wait to think of the members.** The gap needed
  someone to *wonder* whether libglvnd was a loader. `tools/plugin_boundaries.py`
  replaces the wondering with a measurement: a bundled object that imports
  `dlopen` is a loader by construction, and E59 fails the suite on one that is
  not classified. It immediately found `libdecor-0.so.0`, which nobody had
  looked at. Do this for the next class rather than trusting the next reader to
  be more imaginative.
- **Check whether the thing you handed off has landed.** The sharun patch sat
  under "blocked: a human must give this to a maintainer" while the fix had
  already been merged upstream. `gh api repos/<owner>/<repo>/commits/<sha>` is
  ten seconds.

And one that applies to the claim itself: **a closure claim must be stated in
terms of the outcome and must list what it did not examine.** "The experiment is
closed" was true of *can a bundled glibc drive a foreign-libc driver* and false
of *does the AppImage work on this host*. Section 4 is written that way now.

### About symbol versions

- **An unversioned reference does NOT get the default definition.** This is the
  whole bug. `pthread_cond_init`, `realpath`, `regexec`, `glob`, `nftw`,
  `sched_getaffinity` and about 27 others have an obsolete definition glibc
  still exports, and a stripped or musl-built object lands on it silently.
  `tools/version_traps.py <libc>` prints the current list for any libc.
- **`dlsym` is not a way to find the default definition.** Measured in E27:
  `dlsym(RTLD_NEXT, "pthread_cond_init")` returns the **obsolete** definition on
  glibc 2.31 and the default one on 2.41. `dlsym(RTLD_DEFAULT, ...)` gets it
  right on both, but from inside the preload it finds the preload's own
  forwarder and recurses. Read the version name out of the ELF and use `dlvsym`.
- **Multi-versioned does not mean dangerous.** glibc 2.34's libpthread merge
  re-versioned 191 symbols in place: same address, two labels, either is
  correct. Only a name whose versions have **different `st_value`** is a trap.
  A list built from "has two versions" is 191 false positives long.

### About preloads, and driving the loader by hand

- **`ld.so --preload A --preload B` loads only B.** glibc's option parser keeps
  a single `preloadarg`, so a second `--preload` REPLACES the first rather than
  appending. The command line reads as if both are loaded, the process has one,
  and here that made `foreign-dlopen.so` silently vanish while every debug line
  it would have printed simply did not appear. `--preload "A B"` -- one flag,
  space-separated -- is the working form. `LD_PRELOAD` uses `:` and appends
  normally; this trap is specific to the flag.
- **Preload constructors run in REVERSE of the list.** Listing `gl-fwd.so`
  after `foreign-dlopen.so` in `.preload` runs `gl-fwd`'s constructor FIRST.
  E56 and E57 measure it both ways. Never order two preloads by writing them in
  the order you want them initialised; have the later one ask (that is what
  `foreign_dlopen_init_now()` is).
- **A `timeout` on a program that never exits hangs a `$( )` capture, and it
  hangs on the case that WORKED.** `timeout 25 xvfb-run ... glxgears` kills
  `xvfb-run`, the shell script, and leaves `Xvfb` and `glxgears` holding the
  stdout pipe; the command substitution then waits for a writer that will never
  close. The case that FAILS exits immediately and returns fine. Write to a file
  and `pkill` afterwards. This cost two full container runs before it was even
  visible as a harness bug rather than a hang in the shim.

### About the test environment

- **`.tmp/AppDir` is shared state, and debugging one host by hand poisons the
  next full run.** Section J rewrites `.preload`; so does anyone reproducing a
  case at the prompt. The next `appimage.ps1` then runs sections A through I
  with GL shims those cases know nothing about, and what you get is not a
  crash: E53 and E53b failed on hardware that was working and E59 counted
  nineteen bundled loaders where the AppImage ships eight. `40-appimage.sh` now
  resets `.preload` and removes the shims before anything runs, and refuses to
  start if `shared/bin` holds a file the AppImage does not ship. If you are
  debugging by hand, `rm -rf .tmp/AppDir` afterwards.
- **`podman run --platform linux/arm64 <tag>` REPLACES the cached image for
  that tag.** The pull is per-tag, not per-tag-per-arch, so one aarch64 probe
  left `alpine:3.22` resolving to arm64 and the next suite run died with
  `exec container process: Exec format error` on an image it had used all day.
  `podman pull --platform linux/amd64 <tag>` puts it back. To run aarch64 code,
  name `qemu-aarch64-static` instead -- section P does, and it also avoids
  registering a binfmt handler, which is a kernel-wide setting.
- **Ubuntu 14.04 and 16.04 are NOT on `old-releases.ubuntu.com`.** As of
  2026-08 that host jumps straight from `saucy` to `utopic`; every
  `dists/trusty/...` and `dists/xenial/...` path 404s. Both releases are still
  inside their ESM window and are still served from **`archive.ubuntu.com` at
  the default path**, so the `sources.list` rewrite that every guide prescribes
  is what breaks them. What does have to go is the image's own ESM source,
  which points at `esm.ubuntu.com` and needs credentials: apt fails the whole
  update over it and then reports every package as "unable to locate", which
  reads exactly like a dead mirror. `rm -f /etc/apt/sources.list.d/*esm*`.
- **Handing sharun the host's library directories changes what the NO-SHIM
  controls do.** With `/etc/ld.so.conf`'s directories on
  `SHARUN_FALLBACK_LIBRARY_PATH`, `glxgears` renders on Alpine with no GL shim
  at all -- through the X server's own softpipe GLX -- and E61 stops being a
  control for anything. Add host directories only where a measurement showed
  they are needed, and print that they were added.

- **Debian's Vulkan ICD manifest names a bare soname, not a path.**
  `foreign-dlopen` only ever intercepts absolute paths, so on Debian the whole
  feature is a no-op and every A/B looks identical and healthy. Alpine and
  Gentoo use absolute paths. Write your own manifest if you want the code path.
- **`xvfb-run -a` alone gives you an X server with no GLX.** `glxgears` then
  says `couldn't get an RGB, Double-buffered visual`, which reads exactly like a
  driver failure. You need
  `-s '-screen 0 1024x768x24 +extension GLX +extension RANDR +render'`.
- **Alpine's `mesa-gl` is not libglvnd.** There is no `libGLX_mesa.so.0` on
  Alpine, so anything bundling libglvnd has no vendor library to load. That is
  not fixable from a loader shim.
- **`ANYLINUX_LIB_FOREIGN_DLOPEN=0` is not always a clean control.** Under the
  demo AppImage's own AppRun on Alpine, with a search path that reaches `/lib`,
  the bundled glibc `ld.so` finds `libc.musl-x86_64.so.1` and loads it: the
  process ends up with **two libc families initialised** and renders anyway.
  `LD_DEBUG=libs` and `grep 'calling init:'` is what distinguishes an object
  that was *searched for* from one that was *loaded*.
- **`/bin/true` is not always an ELF binary.** On Rocky 9 it is a 51-byte shell
  script, and `ld.so` answers `file too short`, which looks exactly like a
  broken runtime and is not. `runtime-select` re-execs itself instead.
- **`/proc/self/exe` is the loader, not you**, when a program is started as
  `ld-linux.so --library-path ... ./prog`. The kernel exec'd the loader. Use
  `argv[0]`. This produced a false `SELF-TEST FAILED` on every newer host.

### About writing the tests themselves

- **`tests/vkprobe.c` used to smash its own stack on SUCCESS.** Its `struct
  Props` was ~800 bytes shorter than `VkPhysicalDeviceProperties`, so the driver
  wrote past it every time enumeration *worked*. A segfault that only happens
  when things go right is the most misleading result available. It now has the
  tail, a guard band, and a check.
- **stdout is block-buffered when it is a pipe.** A probe that crashes loses
  every line it printed, and you conclude it crashed at the start. `setvbuf(...,
  _IONBF, ...)`.
- **A verdict computed inside `$( )` increments the counter in a subshell.** The
  per-line verdicts and the totals then disagree, and the suite reports success
  while showing a MISMATCH.
- **`ls a b` fails as a whole when either glob misses.** Used as a capability
  probe, that silently skips a case on a host that could have run it.
- **A verdict grep that matches inside another word is a silent wrong answer.**
  `grep -E 'OK|FAILED|rror'` over a probe's output matches `glGetError` and
  reports the diagnostic line as the verdict. Take the probe's own
  `^(OK|FAILED)` line FIRST and only then fall back to something looser -- the
  same two-pass shape `summarise()` already uses, for the same reason.
- **A probe that prints `GL_RENDERER` has not finished.** A GL shim exporting a
  subset gets a context, prints a renderer, and dies on the next call. Any test
  whose success condition is "a renderer appeared" passes such a shim. Make the
  probe do something whose RESULT you check -- `glprobe` clears to a known
  colour and reads the pixel back -- because a stub that returns zero and a
  driver that cleared the buffer are otherwise identical from outside.
- **Predicting `FAIL` scores a segfault as a MISMATCH.** "It did not work"
  arrives as an error code, a refusal to load, or a crash. Normalise first, then
  predict.
- **`dlerror()` is destructive, and so is anything that calls `dlsym`.** Reading
  it to log it consumes it. Less obviously: a diagnostic that probes with
  `dlsym` replaces the pending message with its own last miss, so the caller is
  told the wrong object failed. Both were live bugs here.
- **`mkstemp` rewrites its template in place.** Reusing a spent template makes
  the second loop a silent no-op. This made a fuzz test "pass" nothing.
- **glibc serves a 16 KB `malloc` from its arena; musl `mmap`s it.** So an
  absent `mmap` in an `strace` comparison proves nothing about whether the
  allocation happened. Compare on syscalls that must appear in both, such as
  `openat` of a specific file.
- **`RTLD_DEFAULT` does not see an object's own dependencies.** A "missing
  symbol" report that only consults the global scope accuses a library of
  missing 446 symbols its own `DT_NEEDED` closure supplies.
- **glibc puts version *names* in `.dynsym`** as zero-sized `SHN_ABS` entries
  (`GLIBC_2.32`, `GLIBC_ABI_DT_RELR`). They are ABI markers, not API. A
  generator that treats them as symbols emits C identifiers containing a dot.
- **Run every runtime test twice**, `ANYLINUX_LIB_FOREIGN_DLOPEN=0` then `=1`. A
  single-sided result cannot distinguish "the fix worked" from "it was already
  falling back to something else".
- **Shell scripts must be LF.** A CR becomes `$'...\r'` and yields baffling
  "not found" errors. `.gitattributes` enforces it and `run.ps1` verifies rather
  than trusts.
- **`podman machine ssh` drops a file called `NUL` in your working directory.**
  It writes its known_hosts entry to the Windows null device, and from Git Bash
  that resolves to a real file. `git add` then fails the whole commit with
  `short read while indexing NUL`, which reads like repository corruption and is
  a stray 99-byte SSH host key. `rm -f ./NUL` clears it. Check for it after any
  `podman machine ssh`.
- **PowerShell corrupts a string piped to a native process.** Mount scripts into
  the container instead. A PowerShell function that leaves native output on the
  success stream returns an array, not your exit code.
- **`& $exe @array 'x' 'a','b','c'` passes the comma list as ONE argument.**
  PowerShell parses `a, b, c` in a command position as an array expression and
  stringifies it. The GPU-capability probe did this and reported "no GPU" on a
  machine that has two, and the whole suite then SKIPPED nine cases while
  looking perfectly healthy. Build one flat array and splat it once.

### About reaching the GPU

- **A missing library directory does not announce itself as a missing library.**
  This is the single most misleading failure here. `libcuda.so.1` loads,
  resolves every entry point, and then `cuInit` returns 100
  `CUDA_ERROR_NO_DEVICE` -- because it `dlopen`ed `libdxcore.so` by bare soname
  and `ld.so` could not find it. Mesa's `d3d12_dri.so` does the same with
  `libd3d12.so` and the user sees `Error: glXCreateContext failed`. Both read as
  hardware faults. `LD_DEBUG=libs` plus `grep 'find library='` is the one
  command that distinguishes them.
- **`sharun` re-execs and replaces your `--library-path`.** Running
  `$APPDIR/bin/<prog>` under `ld.so --library-path ...` does not do what it
  looks like: everything in `bin/` is sharun, which re-execs the real binary
  with a path it assembles itself. The trace shows two pids and only the second
  one matters. `SHARUN_FALLBACK_LIBRARY_PATH` is the supported way to add to it
  without editing anything.
- **`MESA_D3D12_DEFAULT_ADAPTER_NAME` is the only adapter selector.** Without it
  a machine with two GPUs quietly gives you the integrated one. `GALLIUM_DRIVER=d3d12`
  is what selects the driver; `MESA_LOADER_DRIVER_OVERRIDE=d3d12` alone is not
  enough and falls back to llvmpipe without saying so.
- **`glxgears -info` prints the whole `GL_EXTENSIONS` string**, which is several
  kilobytes on one line and will bury whatever you were reading. Grep for
  `GL_RENDERER` with `-m1`.

### About measuring what the loader did

- **`LD_DEBUG=bindings` prints the version a reference ASKED for, not the one it
  got.** For the version-binding trap that is exactly the wrong half: an
  unversioned reference asks for nothing, and the line is silent about which of
  the two definitions it landed on. `tests/bindprobe.c` reads the slot instead.
- **A lazily-bound GOT slot still holds the PLT resolver stub.** Reading it
  without `LD_BIND_NOW=1` measures the stub, not the definition. Eager binding
  changes *when* the choice is made, never which definition is chosen.
- **A `d_ptr` in a mapped `PT_DYNAMIC` may be absolute or link-time**, depending
  on the port, and dereferencing the wrong guess is a segfault. `dladdr` decides
  it safely: it searches the loaded objects for an address and never
  dereferences it, so the candidate that lands inside that object is the right
  one.
- **Taking `&func` in an EXECUTABLE gives you its own PLT entry**, not libc's
  address, so comparing that against a shared object's `&func` can differ for a
  linking reason rather than a libc one. Compare the FILE each address lands in.

### About what a control is allowed to do

- **Some controls do not flip, and that is the result.** The CUDA cases were
  written expecting the feature-off control to fail. It passes, because NVIDIA
  ships against a `GLIBC_2.2.5` floor and nothing in the blob can be missing.
  The correct response was to state that as the finding (REPORT.md 7.1), not to
  keep forcing the test until it broke. A control that has to be engineered into
  failing is not evidence of anything.
- **`$?` after a pipeline is the LAST command's status.** `probe | sed` then
  `echo $?` reports `sed`. Same family as the subshell-counter bug above, and it
  made three scratch runs look like they all succeeded.

## 6. Diagnostic ladder

When something fails, report **which rung caught it**.

0. **Is this even the libc gap?** Before anything else, ask whether the host has
   the plugin AT ALL in the shape the bundled loader wants. `couldn't get an
   RGB, Double-buffered visual` from a GL app on a musl host is not a libc
   failure and no amount of rung 3 through 11 will find anything: the host's
   Mesa is classic, there is no `libGLX_<vendor>.so.0`, and the answer is
   `gl-fwd.so` rather than `foreign-dlopen.so`.
   `python3 tools/plugin_boundaries.py $APPDIR --verbose` lists every bundled
   loader and what it looks for; `ls /usr/lib/libGLX_*.so.0
   /usr/lib/*/libGLX_*.so.0` answers it for GL in one command. With the shim
   loaded, `ANYLINUX_LIB_DEBUG=1` prints which target it chose and how many of
   the entry points resolved -- a line reading `0 of 3470` means it found no
   target at all, and every GL call in the process is returning zero.
1. **Host driver sane?** `vulkaninfo --summary` natively. If this fails, stop.
2. **Display, not libc?** Re-run under
   `xvfb-run -a -s '-screen 0 1024x768x24 +extension GLX +render'`. WSI errors
   are not this project's bug.
3. **Feature on?** `ANYLINUX_LIB_DEBUG=1`. No ` [foreign-dlopen.so] >> ` lines
   means the marker, the env switch, or the `.preload` order is wrong. No
   `foreign: rewriting` line and no `needs no rewrite` line means the path was
   not absolute and nothing was ever intercepted.
4. **Which object, which symbol?** `ANYLINUX_LIB_FOREIGN_DRYRUN=1`, or
   `LD_DEBUG=libs,bindings`. An `undefined symbol: X` names the next candidate.
5. **Is the library findable at all?** The failure most likely to send you the
   wrong way. A driver that `dlopen`s the rest of its own stack by BARE SONAME
   -- `libdxcore.so` from CUDA, `libd3d12.so` from Mesa's d3d12 -- is not
   intercepted, so `ld.so` searches `--library-path` and nothing else, because
   the cache is inhibited. What you see when it misses is
   `CUDA_ERROR_NO_DEVICE` or `glXCreateContext failed`, neither of which
   mentions a library. `LD_DEBUG=libs LD_DEBUG_OUTPUT=/tmp/ld` then
   `grep 'find library=' /tmp/ld.*` is what settles it in one command.
7. **Is `X` really absent?** Check with `elfsym.py` against the **bundled**
   `libc.so.6`. If present, this is a scope or visibility problem, not
   availability, and needs a different fix.
6. **Is `X` merely re-homed?** musl folds `libm`, `libpthread`, `libdl`, `librt`
   and the resolver into `libc`; glibc splits them out, and glibc 2.34 merged its
   own split libraries back in. Load the library instead of shimming the symbol.
   `fgn_global_scope_libs[]` in `src/foreign-dlopen.c` is the list.
8. **Did the rewrite corrupt the image?** Re-parse
   `$XDG_RUNTIME_DIR/.anylinux-fgn-*.so` with `elfsym.py`.
9. **Did it need rewriting at all?** `ANYLINUX_LIB_DEBUG=1` prints
   `provider <file> -> ...` for each `DT_VERNEED` file and says which version it
   could not vouch for. On a host older than the bundle the answer should be
   "nothing was rewritten" (E39).
10. **Loads, but the wrong definition?** `ANYLINUX_LIB_FOREIGN_NOSTRIP=1` keeps
    the version tags while still loading from the private copy, which separates
    "the rewrite broke it" from "the path broke it". If NOSTRIP fixes it, you
    are looking at a version-binding trap: run `tools/version_traps.py` against
    the libc and check the symbol is covered by `version-compat.c`. To read the
    answer rather than infer it, `LD_BIND_NOW=1 tests/bindprobe <lib> <symbol>`
    walks each loaded object's relocations and names the file and version behind
    the address the loader actually stored. `LD_DEBUG=bindings` cannot do this:
    it prints the version a reference ASKED for, and for this trap the whole
    point is that the reference asks for nothing.
11. **Loads and still misbehaves?** ABI territory. `tests/abi-host.c` against a
    guest built by the other libc covers the allocator, `errno`, `FILE*`,
    mutexes, condition variables and the divergent structs (E47-E50); if a
    crossing there is red, that is your answer. If they all pass, the remaining
    shapes are the two live hazards in 4.2 -- a musl-built object reading back a
    glibc-filled struct at its own stride, or comparing against its own `FTW_*`.
    Otherwise bisect with gdb: breakpoint the failing library call, `finish`,
    read the return value, and `info symbol $pc` at entry — that last step is
    what found the version trap.

## 7. Rules that must not be broken

- **Never modify a host file.** Every write goes under `$XDG_RUNTIME_DIR` or
  `$TMPDIR`. `tests/invariants.c` and the checksum comparison in REPORT.md guard
  this.
- **Bundled libraries always beat host libraries**, for everything except the
  libc runtime set when Design R deliberately switches it.
- **Exactly one libc family per process.** Never `dlopen` a second libc. E8 and
  E9 measure why for glibc. musl's libc *can* be mapped by a glibc `ld.so`,
  which is worse, not better: it succeeds quietly. See section 5.
- **Never strip symbol versions partially.** `DT_VERSYM`, `DT_VERNEED`,
  `DT_VERDEF` and `DT_VERDEFNUM` go together. A verdef without its versym
  segfaults `ld.so`.
- **Strip only when the object actually needs it.** A rewritten object is a
  private copy loaded from a path the application did not ask for, and the
  Vulkan loader says so. On a host that can satisfy every requirement, the right
  number of rewrites is zero.
- **Never touch `ld-linux*`, `libc.so.*`, `ld-musl*`.** `fgn_never_touch[]`
  exists for this. `ld-linux` has no `SONAME`, so `RTLD_NOLOAD` cannot catch it.
- **Do not add library searching to `foreign-dlopen.c`.** Finding libraries is
  `ld.so`'s job, driven by `--library-path`. Two search implementations would
  diverge and the C one would be the buggy one.
  **Assembling that path is a different job, and it belongs to whatever launches
  the process.** There are two such launchers and they needed the same fix
  independently: sharun assembles the path for the bundled runtime, which is now
  fixed upstream in [Anylinux-sharun@`54208d2`](https://github.com/pkgforge-dev/Anylinux-sharun/commit/54208d2bc7d4c919ba46a6c234f6af7f8426b537), and
  `rs_library_path()` in `src/runtime-select.c` assembles it for the switched
  one, which is done here. Adding a directory to a path is not searching it; the
  shim still never opens a library it was not handed by name.
  **`src/gl-fwd.c` is the one deliberate exception, and it is bounded.** It
  resolves exactly ONE soname -- the one it is impersonating -- because ld.so
  cannot: that name is taken by the shim itself, so `dlopen("libGL.so.1")` would
  hand back the shim's own handle and every forward would recurse. A closed,
  single-name lookup over a fixed directory list plus `ANYLINUX_GL_HOST_DIR` is
  not a search implementation, and it must not grow into one.
- **Anything appended to a library path goes at the END.** Bundled directories
  first, then the host runtime dir, then the conventional host dirs, then
  whatever `/etc/ld.so.conf` names. Inserting anywhere else hands a host library
  a win it should not have, which is the defect two corrections in README.md are
  already about.
- **A shim that replaces a library exports everything that library exports.**
  `src/gl-fwd-gl.h` and `src/gl-fwd-egl.h` are generated by
  `tools/gen_gl_fwd.py` from the bundled libglvnd, never hand-edited, and
  `make gl-syms-check` plus E60 fail if they drift. A subset is not a smaller
  version of this design: it renders `glxgears` and hands the next application
  `undefined symbol`.
- **Regenerate the GL tables when the bundled libglvnd changes.**
  `make gl-syms GLVND=<AppDir>/lib`. Same treadmill as `make shim`, same reason.
- **Regenerate the shim when the bundled glibc changes.**
  `make shim FLOOR=... TARGET=... MUSL=...`. A stale shim interposes over symbols
  libc now provides. `src/forward-shim-manifest.json` records the floor it
  targets. `MUSL` has a default in the Makefile because omitting it drops 33 of
  35 definitions and silently disarms the musl bridge.
- **Re-audit the version traps when the bundled glibc changes.** `make traps`.
  The set only grows with newer glibc.
- **A test you cannot run is SKIPPED with the specific missing capability
  named.** Never silently omitted, never guessed.

## 8. Repository permissions

Work in `https://github.com/Azathothas/dlopen-experiment` only. `gh` is
authenticated with account-wide scope, so this is a policy rule you enforce
yourself.

**Allowed:** commits, branches, tags, push, issues, PRs and releases on **this
repo only**.

**Forbidden everywhere else:** creating or commenting on issues, PRs,
discussions or reviews; starring, forking, watching or editing any other
repository; pushing to any other remote; any non-`GET` `gh api` call outside
this repo; touching account settings, keys, gists or org membership.

Read-only elsewhere is fine. Upstream projects (`Anylinux-AppImages`, `solo`,
`detour`, `sharun`) are **read-only**: study them, never file anything on them.
If a task seems to need a write outside this repo, stop and ask.
