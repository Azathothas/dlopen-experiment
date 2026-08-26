# PORTING.md

**Brief for one session: take this repository from a measured experiment to a
production-grade project, under a new name and a new owner.**

You are an agent with no memory of this project. **This file is the only
document you need in order to start.** It states what the project is, what is
already true about it, and every task, in order, with what "done" means for
each. You will read other files in this repository as you work -- that is
unavoidable and expected -- but you do not need to read another *brief* to
understand the assignment.

⛔ **This is not a refactor. The C, the tests and the measurements are correct
and stay correct.** What is missing is everything a project needs to be used by
somebody who is not its author: portable builds, honest documentation, real
examples, CI, and a work record that is not an agent's scratchpad.

---

## 0. What this project is, in one page

An AppImage bundles its own glibc so that it runs on any distribution. It does
**not** bundle GPU drivers, because Mesa plus LLVM is 100-200 MB. So it must use
the **host's** drivers. Two different things stop it, and telling them apart is
the whole idea of this repository.

**Gap 1, libc.** The host's driver exists and is nameable, and it was built
against a different libc: a newer glibc, or musl on Alpine. A bundled glibc 2.31
process cannot `dlopen` an object that wants `GLIBC_2.38`, and it cannot
`dlopen` a musl object at all. The repair is `src/foreign-dlopen.c`: an
`LD_PRELOAD`ed interposer that rewrites the host object in a private copy so its
symbol version requirements stop mattering, drops the musl libc dependency edge,
and bridges the imports that are left. `src/forward-shim.c` (generated) supplies
what the bundled libc lacks; `src/version-compat.c` (generated list, hand-written
forwarders) fixes a trap where an unversioned reference binds glibc's *obsolete*
definition of a symbol rather than its default one; `src/runtime-select.c` can
switch the whole libc runtime at `execve` time when the host's is newer.

**Gap 2, interface.** The host has the *capability* and ships nothing in the
shape the bundled loader looks for. The AppImage bundles libglvnd; an
application links `libGL.so.1`; behind it `libGLX.so.0` `dlopen`s a vendor
library, `libGLX_<vendor>.so.0` -- and a host whose Mesa was built without glvnd
ships no such file at all. No amount of libc bridging carries a file that does
not exist. The repair is `src/gl-fwd.c`: an object built with the SONAME of the
library it replaces, preloaded so `ld.so` binds the application's `DT_NEEDED` to
it, forwarding all 3470 entry points of the bundled `libGL.so.1` to whichever
target the host can actually stand behind. Built a second time with a different
table it is `egl-fwd.so`.

**What is measured.** Three suites, all green at the time this file was written:

| suite | command | result |
|---|---|---|
| evidence table | `experiments/run.ps1` | 36/36 predictions held |
| AppImage end to end, glibc host | `experiments/appimage.ps1` | 40/40, no skips |
| AppImage end to end, musl host | same command | 35/35, five named skips |

Including a closed-source driver round-tripping 4096 bytes through a real NVIDIA
RTX 3050 Ti, and OpenGL rendering on that GPU through the AppImage at over 100
FPS via Mesa's d3d12 Gallium driver.

**What is not.** `CONTINUE.md` section 4.0 lists eight named limits, and
`TODO/` will carry them after task 6 below. Do not let the port widen a claim
the evidence does not support. ⭐ **Where the port has to state a result, state
the one that was measured, with the host it was measured on.**

---

## 1. The template you are adopting

The destination conventions come from
[`Azathothas/TEMPLATE`](https://github.com/Azathothas/TEMPLATE). Fetch and read
its adoption guide in full before you change anything:

```bash
curl -sSL -o /tmp/ADOPT.md https://raw.githubusercontent.com/Azathothas/TEMPLATE/main/ADOPT.md
```

Everything else it names lives under that base:

```bash
TEMPLATE_RAW=https://raw.githubusercontent.com/Azathothas/TEMPLATE/main
```

### What ADOPT.md says, and where this assignment deviates

⚠ `ADOPT.md` opens with a safety contract written for adopting a **stranger's**
repository. Several of its rules are deliberately suspended here, because the
owner of this repository is the person assigning this work and has said so
explicitly. **Suspending a rule is not the same as ignoring it.** The exact
deviations, and nothing beyond them:

| ADOPT.md rule | status here |
|---|---|
| 1. Work on a new branch, never the default | **Applies.** Do the work on a branch. Task 9 is what ends up on the default branch, and it is a single commit. |
| 2. Never overwrite an existing file | **Relaxed for this repository's own documents only.** You are explicitly asked to repurpose `CONTINUE.md`, `REPORT.md` and `README.md`. Everything else keeps the rule: propose, show the diff, do not merge silently. |
| 3. Never delete anything | **Relaxed, narrowly.** Task 6 deletes agent-only files -- and ⛔ **only after confirming, file by file, that the content has been copied or repurposed somewhere that survives.** A deletion whose content went nowhere is a bug, not a tidy-up. |
| 4. Never rewrite history | **Suspended, once, at the very end.** Task 9 only. Not before. |
| 5. Never commit until the human has seen the diff | **Relaxed.** You may commit on your branch as you go. |
| 6. The project's existing conventions win | **Applies fully.** This repository has a strong voice, a testing idiom (state a prediction, report MATCH/MISMATCH, a MISMATCH is a finding) and a documentation habit (measured or labelled UNVERIFIED, never estimated). ⭐ Keep all three. Where the template offers a convention this project already has in a different form, take the project's. |
| 7. Nothing runs that writes outside the repository | **Applies fully.** |
| 8. A found secret is reported, never fixed silently | **Applies fully.** |

⭐ **Run ADOPT.md's Phase 0 measurement pass first, before task 2.** The probe
and the checks are read-only, and their "before" numbers are what task 9.3 hands
back an "after" against. ⚠ An adoption that did not move a number did not do
anything, so record them now or there is nothing to compare to:

```bash
curl -sSL -o /tmp/doctor.sh "$TEMPLATE_RAW/scripts/doctor/doctor.sh" && sh /tmp/doctor.sh --json
```

```bash
for c in check-no-secrets check-docs check-placeholders check-control-bytes; do curl -sSL -o "/tmp/$c.sh" "$TEMPLATE_RAW/scripts/common/$c.sh"; done
```

⚠ Expect findings. On this repository, expect them mostly in `check-docs`
(the documents are long and cross-referential) and in line endings. Record the
numbers; do not fix anything yet.

---

## 2. Move the repository

The project is changing name and owner.

**From:** `https://github.com/Azathothas/dlopen-experiment`
**To:** `https://github.com/pkgforge-dev/cross-libc-dlopen`

The destination repository already exists and contains **only a `LICENSE`
file**. ⛔ **Keep that LICENSE.** It is the destination's licence, not this
one's; if the two differ, stop and ask which governs before writing anything
over it. The account running this session (`Azathothas`) has full read-write
access to the destination through both `git` and `gh`.

### 2.1 Repoint the remote

```bash
git remote set-url origin https://github.com/pkgforge-dev/cross-libc-dlopen.git
```

```bash
git remote -v
```

### 2.2 Purge every reference to the old name

⛔ **When you are finished, no reference to `Azathothas/dlopen-experiment` may
remain anywhere in the tree, in any permutation.** That includes:

- the plain URL, with or without `https://`, with or without `.git`;
- `github.com/Azathothas/dlopen-experiment` inside a Markdown link target;
- raw-content URLs, `raw.githubusercontent.com/Azathothas/dlopen-experiment/...`;
- issue and pull-request links of the form `.../dlopen-experiment/issues/N` and
  `.../pull/N`, which **do** exist in the current documents and are load-bearing
  citations -- see the note below;
- the bare string `dlopen-experiment` in prose, in a path, in a script, in a CI
  file, or in a comment;
- clone commands, `gh` invocations and `git` remotes in any example.

The sweep, run from the repository root:

```bash
git grep -nIi -e 'dlopen-experiment' -e 'Azathothas/dlopen' -- . | cat
```

```bash
git grep -nIi -e 'dlopen.experiment' -- . | cat
```

⚠ **The second pattern is not redundant.** `.` matches any character, so it
catches a hyphen that became an underscore or a space during an edit -- which
the first pattern misses and which reads as correct to a human. ⚠ Neither
catches a name split across a line break in prose; for that, read the diff.

⚠ **Some of those references are citations to real issues and pull requests**
that will not exist at the new location. `REPORT.md` section 9 credits PR #2 for
the OpenGL finding, and the sharun work cites issue #1. ⭐ **Do not silently
drop the credit.** Convert each to a form that survives the move: name the
contributor and what they found, and cite the upstream artefact that still
exists where one does -- for example the sharun fix is a commit in
`pkgforge-dev/Anylinux-sharun`, which is stable. Where nothing stable exists,
keep the attribution in prose without a dead link.

Confirm the destination is what you think it is before pushing anything:

```bash
gh repo view pkgforge-dev/cross-libc-dlopen --json name,description,isPrivate,defaultBranchRef
```

---

## 3. Build scripts that work anywhere, from anywhere

⭐ **This is the single largest gap between what this repository is and what a
project is.** Today the only way to build the artefacts correctly is to know
that they must be compiled on `debian:bullseye-slim`, and to know why.

### 3.1 What has to be built, and the constraint on each

| artefact | source | constraint |
|---|---|---|
| `foreign-dlopen.so` | `src/foreign-dlopen.c` + `forward-shim.c` + `version-compat.c` | must need no symbol newer than the oldest glibc it will run under |
| `gl-fwd.so` | `src/gl-fwd.c` + `src/gl-fwd-gl.h` | SONAME **must** be `libGL.so.1`; must carry the IBT property note |
| `egl-fwd.so` | `src/gl-fwd.c` + `src/gl-fwd-egl.h` | SONAME **must** be `libEGL.so.1` |
| `runtime-select` | `src/runtime-select.c` | a normal executable, same floor rule |
| the probes | `tests/*.c` | built on the floor, run under the bundled loader |

⛔ **The floor rule is the one that will be got wrong.** Build on the **oldest**
glibc you intend to support, never the newest. A build on glibc 2.41 emits
references to `GLIBC_2.34` symbols and then fails to load inside an AppImage
whose bundled glibc is older. The current build uses `debian:bullseye-slim`
(glibc 2.31) and `src/Makefile` documents this in its header; the assertion at
the end of `experiments/42-build-floor.sh` prints the highest requirement each
artefact ended up with, and the current answer is `GLIBC_2.16`.

### 3.2 What the scripts must do

Build from **any** host libc to **any** target libc, with the minimum possible
prerequisites, safely and repeatably:

1. **Detect what is available and say so before doing anything.** `podman`,
   `docker`, a native toolchain, `zig cc`, a musl cross-toolchain. ⭐ A script
   that fails at step nine because a tool was missing at step one is worse than
   one that refuses at step one.
2. **Default to a container**, because the floor rule is a *property of the
   build environment* and a container is the only portable way to pin it. Accept
   `podman` and `docker` interchangeably. ⚠ The current scripts hardcode
   podman's path on one Windows machine; that is a local convenience and must
   not survive into a build script.
3. **Support a native build with an explicit floor override**, for a maintainer
   who is already on the floor distribution and does not want a container.
   Refuse, loudly and by name, if the detected libc is newer than the requested
   floor.
4. **Emit a manifest with every artefact**: source hashes, the compiler and its
   version, the floor, the maximum `GLIBC_*` requirement of each output, the
   SONAME of each shim, and the entry-point count of each forwarding table.
   `src/forward-shim-manifest.json` is the existing precedent for the shape.
5. **Be idempotent and offline-repeatable.** A second run with no source change
   produces byte-identical output or explains why not.
6. **Verify, not assume.** Every artefact is checked after it is built:
   the SONAME is what it must be, the export count matches the table, the
   maximum `GLIBC_*` requirement is at or below the floor. `src/Makefile`'s
   `gl-fwd-verify` target already does two of those three; promote and extend it
   rather than writing a second one.
7. **Cross-build for aarch64.** The trampolines in `src/gl-fwd.c` have an
   aarch64 form that has been assembled and never run; `make gl-fwd-asm-check`
   is the existing assemble-only gate and needs both `gcc-aarch64-linux-gnu`
   **and** `libc6-dev-arm64-cross` (the compiler alone has no headers and fails
   on `dirent.h`). Make aarch64 a first-class build target, not a check.

**Done when:** a person with only `podman` **or** only `docker` **or** only a
native gcc can produce every artefact with one command on x86-64 and on aarch64,
the manifest proves the floor was respected, and the build refuses rather than
silently producing an artefact that will not load.

---

## 4. Documentation and examples: show, do not tell

Today this reads as a laboratory notebook. It needs to read as a tool, without
losing the thing that makes the notebook valuable -- that every claim has a
command behind it.

### 4.1 `docs/`

Structure per the template's `docs/README.md`. What must be in it:

- **What the two gaps are**, with the failure message a user actually sees for
  each. ⭐ The single most useful sentence in this whole project is that
  `couldn't get an RGB, Double-buffered visual` is a message about visuals for a
  fault that is about neither visuals nor libc.
- **How to integrate it**, per integration target, in section 4.2.
- **What it cannot do**, as a list, with the measurement behind each.
- **The diagnostic ladder.** `CONTINUE.md` section 6 is a rung-by-rung procedure
  for "it did not work, which layer" and it is genuinely good. It belongs in
  `docs/`, addressed to a user rather than to the next agent.
- **The traps.** `CONTINUE.md` section 5 is a long list of things that cost real
  time. Most are about working *on* this project; a minority are about *using*
  it. Split them and put the second set in `docs/`.

### 4.2 `examples/`, with scripts that run

⛔ **Read each target repository first, from its own README and its own source.
Do not describe any of them from memory, and do not invent a flag, a file or an
API.** Each entry below names what was verified to exist at the time of writing;
verify again, because these are moving projects.

**`pkgforge-dev/Anylinux-AppImages`** -- shell, the project this work targets.
`useful-tools/lib/anylinux.c` is where the upstream foreign-dlopen implementation
lives. `useful-tools/quick-sharun.sh` builds AppDirs. `useful-tools/demo/`
carries per-toolkit recipes: `vkcube-glxgears-appimage.sh` (this is the exact
demo this repository has been testing against), plus gtk2/gtk3/gtk4, qt6-dbus,
sdl, webkit2gtk4 and bun. `useful-tools/hooks/` is a hook system documented in
`hook-system.md`. ⭐ **The example to write here is the one that matters most:
take one of those demo recipes, add `gl-fwd.so` and `egl-fwd.so` to the AppDir's
`.preload`, and show the before and after on a musl host.** The `.preload`
ordering does not matter, and `docs/` should say why -- preload constructors run
in reverse of that file, which is why `foreign_dlopen_init_now()` exists.

**`pkgforge-dev/Anylinux-sharun`** -- Rust, the launcher fork that assembles
`--library-path` for the bundled runtime. Relevant files are `src/main.rs` and
`src/utils.rs`. It already carries the library-path completeness fix this
repository used to ship as a patch (commit `54208d2`, "add more directories to
`--library-path`"). ⭐ **The example here is the coupling**: the foreign-dlopen
loader can only reach a host driver that sharun's `--library-path` already
reaches, and `SHARUN_FALLBACK_LIBRARY_PATH` is the supported way to extend it
without editing anything. Show a driver that is invisible without it and found
with it.

**`QaidVoid/onelf`** -- Rust workspace, "pack entire directories into
self-contained executables". Relevant: `crates/onelf/src/bundle/gpu.rs`
(enumerates DRI/GBM/Vulkan ICD search paths), `crates/onelf-format/src/drivers.rs`,
and `docs/guide/cross-libc.md`. ⚠ **onelf solves a different half of the same
problem and the contrast is the example.** It bundles the *entire* libc and uses
an `AT_EXECFN` bootstrap so a musl binary runs on a glibc host without the host
loader being consulted at all. Its own cross-libc guide states the wall it then
hits: the host's `libdrm`, `libgcc_s` and so on are the wrong family, so
"you need musl-built versions". That wall is exactly what this project removes.
⭐ Show an onelf bundle using a host driver it could not otherwise use.

**`VHSgunzo/runimage`** -- shell, "portable single-file linux container". It
carries its own root filesystem under `rootfs/` and a bootstrap under `RunDir`,
with `examples/` for alpine, debian, fedora, ubuntu, void and steam. ⚠ **Check
first whether this project applies at all**: a container that brings its own
userland may have no host-driver problem, or may have exactly this one for GPU
passthrough. ⭐ **If it does not apply, say so in one sentence and give the
reason.** A section that strains to include a project it does not help is worse
than an honest exclusion.

**Static and conventionally-linked binaries.** The same question, asked plainly,
and ⚠ **"static binaries cannot `dlopen`" is the wrong answer** -- it is close
enough to true to be repeated and wrong in the way that matters here. There are
three distinct cases and they need measuring, not assuming:

- **Static musl.** `dlopen` is present and is a stub: it fails, always. Nothing
  this project does can change that, and it is the only one of the three that is
  genuinely out of scope. ⚠ Confirm against the musl version in question rather
  than against this sentence.
- **Static glibc.** `dlopen` works, and glibc warns at link time that doing so
  "requires at runtime the shared libraries from the glibc version used for
  linking". ⭐ **That warning is a description of this project's entire subject.**
  A static-glibc binary that `dlopen`s a host GPU driver is not out of scope; it
  is the case with the sharpest version constraint of all. Measure whether the
  preload mechanism can even reach it -- a static binary has no `LD_PRELOAD`
  path, which may be the real blocker rather than `dlopen` itself.
- **Mostly static, dynamically linked against libc only.** The common shape for
  a portable release binary. Squarely in scope and probably the easiest win in
  this whole section.

⛔ **Write down which of the three you measured, on what, and what happened.**
An "N/A" here without a measurement behind it is the same mistake section 10's
last entry is about.

**Done when:** every example is a script that runs and prints a before and an
after; every claim about another project cites a file in that project; and any
target this does not help is listed with the reason.

---

## 5. CI, on x86-64 and aarch64

GitHub Actions provides both architectures as hosted runners and provides no
GPU. That is less of a limit than it looks, because **every mandatory test in
this repository already runs on Mesa's software rasterisers** -- lavapipe for
Vulkan, llvmpipe for OpenGL -- which exercise the identical `dlopen` path. The
hardware cases are already skipped by name when the capability is absent, which
is the mechanism CI needs and it already exists.

### 5.1 What CI must run

1. **Both suites, both architectures.** `experiments/run.ps1` and
   `experiments/appimage.ps1` orchestrate containers; ⚠ they are PowerShell
   and the shell stages they drive are POSIX `sh`. Either port the orchestration
   to something CI-native or run PowerShell on the runner. ⭐ **Do not port the
   `.sh` stages.** They are the tests.
2. **The build matrix from task 3**, every host/target combination the scripts
   claim to support. A build script that is only ever run one way is a build
   script that works one way.
3. **The generated artefacts are not stale.** `make gl-syms-check` and
   `make traps` both exist and both fail the build on drift. Run them.
4. **The checks from the template**: secrets, docs, placeholders, control bytes.
5. **Line endings.** `.gitattributes` enforces LF for `.sh`; a CR in a shell
   script becomes `$'...\r'` and produces baffling "not found" errors. There is
   an existing check for this in `experiments/run.ps1`; CI should have its own.
6. **Dependabot**, for the GitHub Actions the workflows use. ⛔ **Pin every
   action to a commit SHA**, which is also what the template's workflow scaffold
   requires.

### 5.2 What CI cannot do, and what stands in for it

⭐ **Say this in the workflow file itself, not only in a document.** A skipped
job with no reason is indistinguishable from a broken one.

| cannot | the correlate that CI *can* run |
|---|---|
| a real GPU | lavapipe and llvmpipe exercise the same `dlopen`, rewrite and forward paths. The driver differs; the loader does not |
| `/dev/dri` DRM drivers (`radv`, `anv`, `radeonsi`) | the ICD interface is identical for a software ICD; what is untested is the driver's own device handling |
| NVIDIA's closed-source stack | nothing. State it as untested in CI and point at the local result |
| the aarch64 trampolines executing GL | ⭐ they can at least **run** on an aarch64 runner against llvmpipe, which is far more than the current assemble-only check |

⚠ **Local scripts are not deprecated by CI.** `experiments/run.ps1` is a
three-minute pre-commit gate and stays that way. CI is the wider matrix and the
architectures a developer does not have.

**Done when:** CI is green on both architectures, every job's skip reason is
written in the job, and a deliberately broken artefact (a stale table, a wrong
SONAME, a CR in a `.sh`) makes it red. ⛔ **Prove that last one by planting each
defect and reading the exit code.** A gate never seen to refuse is a gate nobody
knows works.

---

## 6. Convert the agent files into a work record

The template's todo work model has this shape, described in its
`docs/methodology/work-todo.md`:

```
TODO/
  PROGRESS.md   the record: what the last session did, the measured baseline,
                and the work order. Rewritten every session, carries no history.
  INDEX.md      every entry, one line each, sorted by id, with the counts.
  RULES.md      how this repository is worked on, rule by rule.
  <category>.md the entries themselves, grouped by area.
```

⛔ **The work order lives in `PROGRESS.md` and nowhere else.**

### 6.1 Where each existing file goes

| file | disposition |
|---|---|
| `CONTINUE.md` | **Becomes `TODO/INDEX.md` and its entries, then is deleted.** That is the owner's instruction and the mapping is direct: section 4.0 is eight named limits and becomes eight `TODO/` entries -- they are already written in that shape, with a problem, a reason it matters and a way to close it. Sections 2 and 3 (environment, how to reproduce) become `docs/`. Sections 5, 6 and 7 (traps, the diagnostic ladder, the rules) split between `docs/` and `TODO/RULES.md`. Section 8 (repository permissions) is obsolete at the new location and must be rewritten for it, not copied. |
| `REPORT.md` | **Becomes `docs/`.** It is the measured record and it is the most valuable document here. ⚠ It is also ~1875 lines and is cross-referenced **by section number** from `README.md`, `CONTINUE.md` and `analysis/rejected-designs.md`; splitting it breaks every one of those. Fix them; a pointer to a section number that no longer exists is worse than no pointer. |
| `README.md` | **Stays a README**, rewritten for a user rather than a reviewer, plus task 7's comparison. |
| `analysis/ground-truth.md` | **Becomes `docs/`.** Measured distro survey. |
| `analysis/rejected-designs.md` | **Becomes `docs/`.** Three designs evaluated and refused with evidence, including one refused *after* it arrived as a pull request. ⭐ This is unusually valuable and must not be dropped as "old notes". |
| `PORTING.md` (this file) | **Delete**, once every task in it is done or has become a `TODO/` entry. |
| `.tmp/`, `__pycache__/` | Already gitignored. Confirm nothing tracked. |

### 6.2 One entry point for agents

⛔ **Exactly one `docs/AGENTS.md`**, standalone, the single door. Not one per
directory, not a `CLAUDE.md` beside it, not a second router in the root.
The template ships `docs/templates/AGENTS.md` as a starting shape.

⛔ **No document may contradict another.** ⭐ The rule the template states as
*one fact, one home* is the thing to enforce: a number that appears in two
documents will disagree within a month, and the reader has no way to tell which
is stale.

⚠ **This repository does not currently satisfy that rule, and you should know
what you are walking into.** Measured at the time of writing: `3470` appears in
five Markdown files, and `36/36`, `40/40` and `35/35` each appear in four. They
all agree today because they were audited together; that is a snapshot, not a
property. Every measured count gets exactly one home and a pointer from
everywhere else, and a check that greps for the headline numbers and fails on a
second occurrence is worth more than the edit that fixes them once:

```bash
for n in 3470 36/36 40/40 35/35; do echo "== $n"; git grep -lF "$n" -- '*.md'; done
```

**Done when:** `TODO/INDEX.md` lists every open item, no agent-only file remains
in the tree, `docs/AGENTS.md` is the only agent entry point, and a `git grep` for
each headline number finds it in exactly one place.

---

## 7. Compare and contrast with `solo`

[`pg83/solo`](https://github.com/pg83/solo) -- "Portable Linux binaries, solved",
C++, ~5100 files -- is the only other project of comparable ambition and maturity
in this space. The README needs a section on how this project differs.

⛔ **Base it on solo's actual code and this project's actual experiments.** Not
on its README's self-description, and not on inference. Cite files.

What is already known here, and what needs checking:

- solo goes the **opposite direction**: a static-musl process loading host glibc
  drivers, via its own ELF loader and a glibc-to-musl ABI bridge. This
  repository goes glibc-process to musl-or-newer-glibc driver. ⚠ Those are not
  the same problem reversed; check what solo does and does not claim.
- solo carries a large ABI bridge. This project carries a *generated* shim
  (`src/forward-shim.c`, ~35 definitions from a measured symbol gap of exactly
  two for the Mesa closure) plus a version-trap forwarder set. ⭐ Compare the
  **maintenance model**, not the line counts: generated-from-inventory versus
  hand-maintained is the real difference.
- ⚠ **This repository's prior-art section states that solo has CI across
  Alpine, Fedora, NixOS and Termux and a corpus test over ~2100 objects per
  commit. That was written from reading, not from checking, and it is exactly
  the kind of claim this comparison must not inherit.** Verify it against solo's
  own workflow files before repeating it. If it holds, it is a genuine advantage
  for solo and the comparison must say so; this project has two container
  suites on one machine.
- solo replaces the loader. This project deliberately does not, and
  `analysis/rejected-designs.md` records why with a measurement. State both
  positions.

**The section must contain:** what each does, the concrete benefits and
drawbacks of each approach, and ⭐ **an honest verdict on which is better and
why** -- including "for this use case, the other one" where that is true. A
comparison that concludes the author's own project wins on every axis is not a
comparison.

---

## 8. Mine `solo` for what transfers

Separately from the comparison: solo is a large, mature codebase solving an
adjacent problem, and it very likely contains things this project has not
thought of.

The template has a methodology for exactly this task, and it is binding on any
task whose verb is *clone, mine, survey or investigate*:

```bash
curl -sSL -o /tmp/references.md "$TEMPLATE_RAW/docs/methodology/references.md"
```

Its order, in brief -- ⛔ read the file itself, this is a summary and not a
substitute:

1. Clone shallow.
2. ⛔ **Capture the commit SHA before stripping anything.** Once the git
   directory is gone the commit is unrecoverable and every citation becomes
   unverifiable.
3. Read the code in **at least three passes, each asking a different question**:
   what is this and what shape is it; how is it actually built, at file and
   line; how does it handle the thing *this* project finds hard; and what
   transfers, what must not, and what it changes about the plan.
4. ⭐ **Read the tracker -- issues and pull requests, both states.** This is the
   step that gets skipped and it is where the measured failures, the refused
   ideas and the maintainer's own statement of purpose live.
5. Trim by deleting, never by moving, so citations stay valid.
6. Write the two files the methodology specifies.

Things worth looking for specifically, given what this project has found:

- how solo handles **symbol versioning**, and whether it hit the
  unversioned-reference trap this project's `version-compat.c` exists for;
- its **struct-layout hazards** between the two libcs, against the two live ones
  measured here (`regoff_t` is 4 bytes on glibc and 8 on musl; the `FTW_*`
  constants are off by one);
- whether it has anything on the **dispatcher/vendor** class of failure, which
  is not a libc problem at all;
- its **corpus test** design, which is a testing idea this project should
  probably steal outright.

**Done when:** the write-up exists where the methodology says, the commit SHA is
recorded, every claim cites a file, and everything actionable is a `TODO/` entry
rather than a paragraph in a document nobody will re-read.

---

## 9. Five deep reviews, then one commit

### 9.1 The reviews

⛔ **Five, minimum, and each asks a different question.** Five sweeps with one
question is one review written up five times, and the template's
`docs/methodology/reviews.md` says the same thing at greater length.

| # | the question |
|---|---|
| 1 | **What did I change that I was not asked to change?** Every unrequested change is a surprise in somebody else's repository. |
| 2 | **Can each check and each CI gate actually fail here?** Plant the defect it exists to catch and read the exit code, unpiped. |
| 3 | **Which sentence in the documentation is not backed by something I can point at?** This repository's own standard is: measured, or labelled UNVERIFIED. Hold the port to it. |
| 4 | **Does anything contradict anything else?** Numbers, file paths, command lines, and the old repository name. Run the sweeps from task 2.2 again. |
| 5 | **Would a person who has never seen this get from the README to a working build without asking a question?** ⭐ Actually follow your own instructions, from a clean checkout, and stop at the first thing that does not work. |

⚠ A pass with no findings means that pass was too shallow. Say what it swept and
what would have had to be true for it to fire.

### 9.2 The history rewrite

⛔ **Last. After everything else is done, CI is green and the reviews are
finished.** Not before.

The history is an experiment's history: at the time this file was written, 23
commits, several of which correct earlier ones by name, and none of it useful to
someone arriving at a production repository. The owner has explicitly authorised
a rewrite to a single commit. ⚠ Check the count yourself with
`git rev-list --count HEAD` -- the port itself will add to it.

- **Title:** `Initialize Project`
- **Body:** a very brief summary of where the project came from, and a note that
  the history was collapsed.

⛔ **Safely.** Before rewriting:

1. Push the pre-rewrite state to a branch on the destination, so the
   experimental history survives somewhere reachable. ⭐ **Confirm that backup
   exists and is fetchable before you touch anything** -- `git ls-remote origin`
   naming it is the confirmation; having pushed it is not.
2. Verify the working tree is exactly what you want the single commit to
   contain. `git status --porcelain` empty, and every file accounted for.
3. Only then collapse.

⭐ **Use an orphan branch, not a rebase or a filter.** It is one operation, it
cannot half-apply, and what it produces is exactly the tree you inspected in
step 2 -- an interactive rebase over twenty-odd commits can conflict on any of
them and leaves you reasoning about a partial state:

```bash
git checkout --orphan release-init
```

```bash
git add -A && git status --short | head -50
```

```bash
git commit -m "Initialize Project" -m "<the brief origin note>"
```

⚠ Then make it the default branch's tip deliberately, and read what you are
about to overwrite before you overwrite it:

```bash
git log --oneline origin/main | head -5
```

⛔ Force-push only after step 1's backup branch is confirmed present on the
remote.

⛔ **No document may mention the rewrite.** Not the README, not `docs/`, not
`TODO/`. The commit body is the only place it is recorded. A repository that
explains its own git history in its documentation is telling the reader about
its process instead of its purpose.

### 9.3 End the session

Hand back:

- what was ported, and what was deliberately left;
- the before and after numbers from the template's checks;
- the CI status on both architectures;
- everything that became a `TODO/` entry rather than being done;
- ⛔ anything the secret sweep found, restated at the top, whether or not it was
  acted on.

---

## 10. Things that will cost you a session if nobody tells you

Carried forward because each one was paid for once already.

- ⛔ **Build on the oldest glibc, never the newest.** Everything else in task 3
  follows from this and it is the one that looks like a detail.
- ⚠ **`ld.so --preload A --preload B` silently keeps only B.** glibc's option
  parser holds a single `preloadarg`, so the second flag replaces the first. The
  command line reads as if both are loaded. `--preload "A B"` -- one flag,
  space-separated -- is the working form.
- ⚠ **Preload constructors run in REVERSE of the list.** Listing a shim *after*
  another in `.preload` runs its constructor *first*. This is why
  `foreign_dlopen_init_now()` exists and why the ordering is free.
- ⚠ **`timeout` on a program that never exits hangs a `$( )` capture -- on the
  case that WORKED.** `glxgears` does not exit; the timeout kills the wrapper
  and leaves children holding the stdout pipe. The case that *fails* exits
  immediately and looks fine. Write to a file and reap.
- ⚠ **Shell scripts must be LF.** A CR becomes `$'...\r'` and yields "not found"
  errors that name the wrong thing.
- ⚠ **A test whose success condition is "a renderer string appeared" passes a
  broken shim.** The probes in `tests/` clear to a known colour and read the
  pixel back for exactly this reason. Keep that property in anything new.
- ⚠ **`podman machine ssh` drops a file called `NUL` in the working directory**
  on Windows. `git add` then fails the whole commit with `short read while
  indexing NUL`, which reads like repository corruption and is a stray 99-byte
  SSH host key. `rm -f ./NUL`.
- ⭐ **A SKIP names a missing capability and stops there.** It may say "this host
  has no X". It may not say "and therefore nothing can be done" -- that is a
  claim about the design space, it needs its own evidence, and welded to a
  measured fact it inherits the measured fact's authority. One such sentence
  kept OpenGL broken on every musl distro for an entire session.
