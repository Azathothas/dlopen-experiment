#!/bin/sh
# The end-to-end suite: a real AppImage, a real host driver, on a host whose
# libc is not the AppImage's.  Runs INSIDE the host container.
#
#   /w/AppDir   the extracted demo AppImage (upstream's foreign-dlopen.so kept
#               beside it as foreign-dlopen.upstream.so)
#   /w/build    foreign-dlopen.so and the test binaries, built on the glibc
#               FLOOR so they only need old symbols
#
# Every case states its PREDICTION and the harness reports MATCH / MISMATCH.
# A MISMATCH is a finding. Nothing here is single-sided: the feature is always
# measured off and on, because "it rendered" on its own cannot tell a working
# fix from a fallback that was already happening.
set -u

APPDIR=/w/AppDir
LP="$APPDIR/lib"
LD="$LP/ld-linux-x86-64.so.2"
export APPDIR
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg}
mkdir -p "$XDG_RUNTIME_DIR"
PASS=0; FAIL=0; SKIP=0
XA='-screen 0 1024x768x24 +extension GLX +extension RANDR +render' 

# 41-extract.sh keeps upstream's shim beside the AppDir at extraction time. If it
# is missing, the AppDir has been used before and lib/foreign-dlopen.so is
# whatever the last run left there -- copying that as "upstream" would quietly
# run the entire A/B against the patched build twice and report it as a pass.
if [ ! -f "$LP/foreign-dlopen.upstream.so" ]; then
    echo "  FATAL: $LP/foreign-dlopen.upstream.so is missing."
    echo "  The AppDir is stale. Delete .tmp/AppDir and re-run so extraction"
    echo "  can preserve the shipped shim; without it the 'as shipped' cases"
    echo "  would silently measure the patched build."
    exit 2
fi

use_preload() {                # upstream | patched
    case "$1" in
        upstream) cp "$LP/foreign-dlopen.upstream.so" "$LP/foreign-dlopen.so" ;;
        patched)  cp /w/build/foreign-dlopen.so       "$LP/foreign-dlopen.so" ;;
    esac
}

# One line out of a probe's output, for the report column. A probe that states
# its own verdict on a line of its own gets that line; anything else falls back
# to the first interesting-looking line. Without the first pass the fallback
# pattern wins on an incidental match -- "provenance: .../libcuda.so.1" contains
# "libc" -- and the report shows a neutral line for a case that failed loudly.
summarise() {                  # summarise <text>
    printf '%s' "$1" | grep -m1 -E '^(OK|FAILED|BINDINGS|SOAK|INVARIANTS|ABI)' ||
    printf '%s' "$1" | grep -m1 -iE 'GPU|GL_RENDERER|device|libc|load|SOAK|OK:|FAIL|zero|Error' ||
    printf '%s' "$1" | head -1
}

run() {                        # run <id> <expect: OK|FAIL> <needle> <cmd...>
    id="$1"; want="$2"; needle="$3"; shift 3
    out=$("$@" 2>&1); rc=$?
    got=OK; [ $rc -ne 0 ] && got=FAIL
    verdict=MISMATCH
    if [ "$got" = "$want" ] && printf '%s' "$out" | grep -qF "$needle"; then
        verdict=MATCH; PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
    fi
    printf '  %-6s %-8s predicted=%-4s  %s\n' "$id" "$verdict" "$want" \
        "$(summarise "$out" | cut -c1-96)"
}

skip() { SKIP=$((SKIP+1)); printf '  %-6s %-8s %s\n' "$1" "SKIPPED" "$2"; }

# For cases whose measurement is a NUMBER rather than a string in some output.
# The verdict is computed here rather than inside $( ), because incrementing
# PASS inside a command substitution increments it in a subshell and the totals
# then silently disagree with the per-line verdicts.
verdict() {                    # verdict <id> <0|1 ok> <text>
    if [ "$2" = 1 ]; then PASS=$((PASS+1)); v=MATCH; else FAIL=$((FAIL+1)); v=MISMATCH; fi
    printf '  %-6s %-8s %s\n' "$1" "$v" "$3"
}

# Run something under the BUNDLED loader with the preload, feature forced.
under() {                      # under <0|1> <prog> [args...]
    mode="$1"; shift
    env ANYLINUX_LIB_FOREIGN_DLOPEN="$mode" APPDIR="$APPDIR" \
        "$LD" --library-path "$LP" --preload "$LP/foreign-dlopen.so" "$@"
}

# The same, plus one host directory appended AFTER the bundled ones, for the
# vendor-driver cases. A proprietary driver dlopens the rest of its own stack by
# BARE SONAME, which foreign-dlopen deliberately never intercepts, so ld.so has
# to be able to find it -- and ld.so here is the patched one with the cache
# inhibited, so --library-path is the only mechanism left (E13b). Bundled
# directories stay FIRST so bundled libraries keep winning (section 7).
under_at() {                   # under_at <0|1> <extra-dirs> <prog> [args...]
    mode="$1"; extra="$2"; shift 2
    env ANYLINUX_LIB_FOREIGN_DLOPEN="$mode" APPDIR="$APPDIR" \
        "$LD" --library-path "$LP${extra:+:$extra}" \
        --preload "$LP/foreign-dlopen.so" "$@"
}

# vkprobe reduced to one word, because "it did not work" arrives in three
# different shapes -- an error code, a refusal to load, or a segfault -- and a
# harness that only recognises one of them scores a crash as a MISMATCH and
# hides what actually happened.
probe_verdict() {              # probe_verdict <0|1>
    out=$(under "$1" /w/build/vkprobe 2>&1)
    case "$out" in
        *"OK: 1 physical device"*)
            echo "DEVICES  ($(printf '%s' "$out" | grep -m1 'device\['))" ;;
        "") echo "NO-DEVICES  (crashed with no output)" ;;
        *)  echo "NO-DEVICES  ($(printf '%s' "$out" | tr '\n' ' ' | cut -c1-70))" ;;
    esac
}

# Likewise for rendering: vkcube exits 0 whether or not it found a GPU, so the
# exit code carries no information and the OUTPUT is the whole measurement.
render_verdict() {             # render_verdict <binary> [args...]
    bin="$1"; shift
    out=$(env ANYLINUX_LIB_FOREIGN_DLOPEN=1 APPDIR="$APPDIR" LIBGL_ALWAYS_SOFTWARE=1 \
          timeout 90 xvfb-run -a -s "$XA" "$APPDIR"/AppRun.sh "$bin" "$@" 2>&1)
    printf '%s' "$out" | grep -m1 -E 'Selected GPU|GL_RENDERER|zero accessible|rror' \
        || echo "no recognisable output: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-70)"
}

# The same, on HARDWARE. Mesa's d3d12 gallium driver turns /dev/dxg into a real
# GL device; the adapter is named rather than left to chance so the result says
# which of the two GPUs in this machine actually drew the frames.
#   $1 = 0|1 feature   $2 = extra library dirs for sharun ("" for none)
#
# timeout 25, not 90: glxgears never exits on its own, so the timeout IS the
# runtime of the case. GL_RENDERER is printed before the first frame and the
# first FPS line lands at 5 s, so 25 s is five times the margin and saves three
# minutes a run over the 90 s the software cases need for 20 vkcube frames.
render_verdict_hw() {          # render_verdict_hw <0|1> <extra-dirs> <bin> [args...]
    mode="$1"; extra="$2"; bin="$3"; shift 3
    out=$(env ANYLINUX_LIB_FOREIGN_DLOPEN="$mode" APPDIR="$APPDIR" \
          SHARUN_FALLBACK_LIBRARY_PATH="$extra" \
          GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
          LIBGL_ALWAYS_SOFTWARE=0 \
          timeout 25 xvfb-run -a -s "$XA" "$APPDIR"/AppRun.sh "$bin" "$@" 2>&1)
    printf '%s' "$out" | grep -m1 -E 'GL_RENDERER|rror' \
        || echo "no recognisable output: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-70)"
}

# The directories the DISTRO itself names, read out of the plain-text
# /etc/ld.so.conf. This is the same computation as host_library_dirs() in
# patches/sharun-library-path.patch and as rs_conf_dirs() in
# src/runtime-select.c; a shell stand-in here so the cases can show what the
# path is missing without building sharun.
conf_dirs() {
    awk '{sub(/#.*/,"")} NF' /etc/ld.so.conf 2>/dev/null | while read -r kw rest; do
        if [ "$kw" = include ]; then
            for f in $rest; do
                awk '{sub(/#.*/,"")} NF && $1!="include" {print $1}' $f 2>/dev/null
            done
        else printf '%s\n' "$kw"; fi
    done | sort -u | tr '\n' ':' | sed 's/:$//'
}

# ---------------------------------------------------------------- discovery
ICD=$(ls /usr/share/vulkan/icd.d/*lvp*.json 2>/dev/null | head -1)
if [ -z "$ICD" ]; then
    echo "  no software Vulkan ICD on this host: install mesa-vulkan-swrast"
    echo "  (or mesa-vulkan-drivers) and re-run"
    exit 2
fi
export VK_DRIVER_FILES="$ICD"
# The library the manifest actually names. Alpine and Gentoo use an absolute
# path here, Debian a bare soname; foreign-dlopen only ever intercepts absolute
# paths, so a bare soname means the feature is a no-op on that host.
LVP=$(sed -n 's/.*"library_path"[^"]*"\([^"]*\)".*/\1/p' "$ICD" | head -1)
case "$LVP" in
    /*) ABS=yes ;;
    *)  ABS=no; LVP=$(ls /usr/lib/"$LVP" /usr/lib/*/"$LVP" 2>/dev/null | head -1) ;;
esac
HOSTLIBC=musl; [ -e /lib/libc.musl-x86_64.so.1 ] || HOSTLIBC=glibc

echo "================================================================"
echo " AppImage end-to-end   host libc=$HOSTLIBC   ICD=$LVP"
echo "   manifest library_path is $( [ $ABS = yes ] && echo 'absolute (feature can engage)' || echo 'a bare soname (feature never engages -- forcing an absolute manifest)' )"
echo "================================================================"

if [ "$ABS" = no ]; then
    printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s","api_version":"1.3.0"}}\n' \
        "$LVP" > /tmp/lvp_abs.json
    export VK_DRIVER_FILES=/tmp/lvp_abs.json
fi

echo
echo "-- reference: is the host driver healthy at all? (ladder rung 1) --"
if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary 2>&1 | grep -m1 -E 'deviceName|ERROR' | sed 's/^/  /'
else
    echo "  vulkaninfo absent, skipping the native reference"
fi

# ---------------------------------------------------------------- the A/B
echo
echo "-- A. the host ICD, through the bundled glibc runtime ------------"
# E30: the AppImage exactly as it ships. This is the reported bug.
use_preload upstream
run E30 OK "NO-DEVICES" probe_verdict 1
use_preload patched
# E31: the control. With the feature off the host driver is simply unusable,
#      which is what makes E32 a measurement rather than a coincidence.
run E31 OK "NO-DEVICES" probe_verdict 0
# E32: the fix.
run E32 OK "DEVICES  " probe_verdict 1

echo
echo "-- A2. how much did it have to rewrite? --------------------------"
# Rewriting is not free: every rewritten object is a private copy loaded from a
# path the application did not ask for, and the Vulkan loader says so out loud.
# It should happen when it is NEEDED and not otherwise. On a glibc host older
# than the bundled glibc, nothing can be missing, so the right number is zero.
rm -f "$XDG_RUNTIME_DIR"/.anylinux-fgn-* 2>/dev/null
trace=$(env ANYLINUX_LIB_FOREIGN_DLOPEN=1 APPDIR="$APPDIR" ANYLINUX_LIB_DEBUG=1 \
        "$LD" --library-path "$LP" --preload "$LP/foreign-dlopen.so" \
        /w/build/vkprobe 2>&1)
rw=$(printf '%s' "$trace" | grep -c 'foreign: rewriting')
kept=$(printf '%s' "$trace" | grep -c 'needs no rewrite')
if [ "$HOSTLIBC" = musl ]; then
    [ "$rw" -gt 0 ] && r39=1 || r39=0
    verdict E39 "$r39" "musl host: $rw object(s) rewritten, $kept left unchanged (rewriting is unavoidable here)"
else
    [ "$rw" -eq 0 ] && r39=1 || r39=0
    verdict E39 "$r39" "glibc host: $rw object(s) rewritten, $kept left unchanged (zero is the right answer)"
fi

echo
echo "-- B. how much of the host's /usr/lib is loadable ----------------"
# Where the host actually keeps its libraries: the directory the ICD lives in,
# not a guess. Debian puts them under /usr/lib/<triplet>, Alpine in /usr/lib.
CORPUS=$(dirname "$LVP")
under 1 /w/build/corpus "$CORPUS" 2>/dev/null > /tmp/corpus_on.txt
under 0 /w/build/corpus "$CORPUS" 2>/dev/null > /tmp/corpus_off.txt
total=$(grep -cE '^(OK|FAIL)' /tmp/corpus_on.txt)
off=$(grep -c '^OK' /tmp/corpus_off.txt)
on=$(grep -c '^OK' /tmp/corpus_on.txt)
echo "  corpus directory: $CORPUS  ($total libraries)"

# The verdicts are computed OUTSIDE a command substitution. Incrementing PASS
if [ "$HOSTLIBC" = musl ]; then
    # Nothing built against musl can load without the fix, and essentially
    # everything should with it.
    [ "$off" -lt $((total / 10)) ] && r33=1 || r33=0
    [ "$on" -gt $((total * 9 / 10)) ] && r34=1 || r34=0
else
    # A glibc host can already load its own libraries, so the bar here is that
    # the feature does not make things WORSE. That is the regression this
    # whole case exists to catch.
    r33=1
    [ "$on" -ge "$off" ] && r34=1 || r34=0
fi
verdict E33 "$r33" "feature off: $off / $total load"
verdict E34 "$r34" "feature on : $on / $total load"
# Name what did not load, so a number that looks fine cannot hide a regression.
grep '^FAIL' /tmp/corpus_on.txt | head -3 | sed 's/^/         still failing: /'

echo
echo "-- C. invariants: exactly one libc family, bundled sonames win ---"
run E35 OK "T4.1 PASS" under 1 /w/build/invariants "$LVP"

echo
echo "-- D. does it keep working ---------------------------------------"
run E36 OK "SOAK PASSED" under 1 /w/build/soak "$LVP" 100

# ---------------------------------------------------------------- rendering
echo
echo "-- E. rendering ---------------------------------------------------"
if ! command -v xvfb-run >/dev/null 2>&1; then
    skip E37 "no xvfb-run on this host: install xvfb"
    skip E38 "depends on E37"
else
    # E37a: vkcube exactly as the AppImage ships. This is the complaint.
    use_preload upstream
    run E37a OK "zero accessible devices" render_verdict vkcube --c 20
    # E37: the same command with the preload built from src/.
    use_preload patched
    run E37 OK "Selected GPU" render_verdict vkcube --c 20

    # E40: the whole thing stated the way a user would.
    #
    # Replace exactly one file inside the AppDir, lib/foreign-dlopen.so, and run
    # it. No ANYLINUX_* variables and no VK_DRIVER_FILES: the AppDir already
    # carries .foreign-dlopen-enabled, so the feature turns itself on, and the
    # Vulkan loader finds the host's ICD by itself.
    #
    # Every other case here forces something -- the feature, the ICD, the
    # loader. This one forces nothing, which is the only version of the claim
    # that matches what was actually asked.
    run E40 OK "Selected GPU" env -u VK_DRIVER_FILES APPDIR="$APPDIR" \
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" sh -c \
        "timeout 90 xvfb-run -a -s '$XA' $APPDIR/AppRun.sh vkcube --c 20 2>&1 | tail -4"

    # The OpenGL path needs a libglvnd VENDOR library on the host, because the
    # AppImage bundles libglvnd's libGL/libGLX/libGLdispatch and those dlopen
    # libGLX_<vendor>.so.0. Alpine's mesa-gl is classic Mesa, not glvnd, so
    # there is nothing for them to load: that gap is host packaging, not libc.
    # Each glob is tested separately, because one `ls` over both fails as a
    # whole when either pattern misses, which silently skipped this on Debian.
    if ls /usr/lib/libGLX_*.so.0 >/dev/null 2>&1 || ls /usr/lib/*/libGLX_*.so.0 >/dev/null 2>&1; then
        run E38 OK "GL_RENDERER" render_verdict glxgears -info
    else
        skip E38 "no libGLX_<vendor>.so.0 on this host; its Mesa is not libglvnd, so the bundled libglvnd has no vendor to dlopen"
    fi
fi

# ------------------------------------ F. a CLOSED-SOURCE driver, real silicon
#
# Everything above runs against open-source Mesa: inspectable, rebuildable, and
# when it broke, its __FILE__ strings and a -dbgsym package said where. This
# section is the one class of host library none of that is true for.
#
# The target is NVIDIA's WSL CUDA userspace, reachable through /dev/dxg. Read
# the results in this order, because the headline is not the one you expect:
#
#   E41/E41b  it works -- AND SO DOES THE CONTROL. A vendor ships against the
#             oldest floor it can (this one is GLIBC_2.2.5), so a proprietary
#             driver is the LEAST likely host library to need this fix. The
#             claim these two cases support is the regression claim.
#   E42       zero rewrites, for the same reason, from a real vendor binary.
#   E43a/E43  what the vendor stack DOES need. As shipped, two objects in one
#             driver stack bind two different implementations of five condvar
#             entry points. This turns that into one.
#   E44/E45   and what it needs more: on a real WSL host /usr/lib/wsl/lib is
#             reachable ONLY through /etc/ld.so.cache, which this ld.so is
#             patched to ignore. The symptom is not "cannot open library", it
#             is CUDA reporting no device at all.
#   E46       the vendor's own binary, driving the whole path itself.
echo
echo "-- F. a closed-source vendor driver, on real hardware -------------"
GPU_CASES="E41 E41b E41c E42 E43a E43 E44 E45 E46 E46a"
gpu_skip() { for id in $GPU_CASES; do skip "$id" "$1"; done; }

CUDA=/usr/lib/wsl/lib/libcuda.so.1
if [ ! -e /dev/dxg ]; then
    gpu_skip "no /dev/dxg: this host publishes no WSL GPU paravirtualisation device"
elif [ ! -f "$CUDA" ]; then
    gpu_skip "no $CUDA: the WSL driver userspace is not bind-mounted into this container"
else
    WSLLIB=$(dirname "$CUDA")
    # Read the floor out of the file rather than restating it. This greps the
    # whole binary, not DT_VERNEED, so it is informational only: the verdict
    # about what actually had to be rewritten is E42, which asks the loader.
    # Alpine's base image has neither binutils nor python, so grep it is.
    echo "  vendor driver : $CUDA"
    echo "  GLIBC version names anywhere in the file: $(grep -ao 'GLIBC_[0-9.]*' "$CUDA" | sort -u | tr '\n' ' ')"

    # E41: the whole point. Load a closed-source vendor blob under the
    # AppImage's own glibc, then push 4 KiB to the GPU and read it back. A
    # handle would only prove ld.so was satisfied.
    use_preload patched
    run E41  OK "round-tripped through the GPU and verified" \
        under_at 1 "$WSLLIB" /w/build/cudaprobe "$CUDA"

    # E41b: the control, and the finding. Everything else in this suite has a
    # control that FAILS. This one does not, and that is the answer rather than
    # a defect in the test: a GLIBC_2.2.5 floor cannot be missing anything, so
    # the feature has nothing to do. What is being measured here is that
    # turning it on does not break a driver that already worked.
    #
    # Note what =0 does NOT switch off. The preload is still loaded, so
    # version-compat.c is still interposing (E23), and this pair therefore says
    # nothing about the version-binding half. E43a is the control for that
    # half, and it uses upstream's shim, which has no forwarders at all.
    run E41b OK "round-tripped through the GPU and verified" \
        under_at 0 "$WSLLIB" /w/build/cudaprobe "$CUDA"

    # E41c: the strongest form of the same finding, and the one E41b cannot
    # make. NO preload in the process at all -- not this repo's, not
    # upstream's -- so no interception, no shim and no version-compat
    # forwarders. The vendor blob still drives the GPU. That is what "it never
    # needed the fix" means, stated so that nothing has to be taken on trust.
    run E41c OK "round-tripped through the GPU and verified" \
        env APPDIR="$APPDIR" "$LD" --library-path "$LP:$WSLLIB" \
        /w/build/cudaprobe "$CUDA"

    # E42: and it did not rewrite anything to get there. Same rule as E39,
    # arriving from a vendor binary instead of a synthetic probe.
    rm -f "$XDG_RUNTIME_DIR"/.anylinux-fgn-* 2>/dev/null
    ctrace=$(env ANYLINUX_LIB_FOREIGN_DLOPEN=1 APPDIR="$APPDIR" ANYLINUX_LIB_DEBUG=1 \
             "$LD" --library-path "$LP:$WSLLIB" --preload "$LP/foreign-dlopen.so" \
             /w/build/cudaprobe "$CUDA" 2>&1)
    crw=$(printf '%s' "$ctrace" | grep -c 'foreign: rewriting')
    ckept=$(printf '%s' "$ctrace" | grep -c 'needs no rewrite')
    [ "$crw" -eq 0 ] && [ "$ckept" -gt 0 ] && r42=1 || r42=0
    verdict E42 "$r42" "vendor blob: $crw object(s) rewritten, $ckept left unchanged (zero is the right answer)"

    # E43a/E43: which DEFINITION each object bound, read out of the process
    # rather than inferred. LD_BIND_NOW because a lazily-bound slot still holds
    # the PLT stub; it changes when the choice is made, never which definition
    # is chosen.
    CONDS="pthread_cond_wait pthread_cond_init pthread_cond_signal"
    CONDS="$CONDS pthread_cond_broadcast pthread_cond_destroy pthread_cond_timedwait"
    use_preload upstream
    run E43a FAIL "BINDINGS MIXED" env LD_BIND_NOW=1 ANYLINUX_LIB_FOREIGN_DLOPEN=1 \
        APPDIR="$APPDIR" "$LD" --library-path "$LP:$WSLLIB" \
        --preload "$LP/foreign-dlopen.so" \
        /w/build/bindprobe "$CUDA" --init cuInit $CONDS
    use_preload patched
    run E43  OK "BINDINGS UNIFORM" env LD_BIND_NOW=1 ANYLINUX_LIB_FOREIGN_DLOPEN=1 \
        APPDIR="$APPDIR" "$LD" --library-path "$LP:$WSLLIB" \
        --preload "$LP/foreign-dlopen.so" \
        /w/build/bindprobe "$CUDA" --init cuInit $CONDS

    # E44: the discovery gap, with nothing but the AppImage's own library path.
    # libcuda.so.1 opens by absolute path and loads fine; it is the BARE SONAME
    # dlopen("libdxcore.so") from inside it that cannot be resolved, and the
    # error surfaces as CUDA_ERROR_NO_DEVICE (100). A user reads that as "no
    # GPU", not as a missing library, which is what makes it worth a case.
    run E44 FAIL "FAILED: cuInit" under_at 1 "" /w/build/cudaprobe "$CUDA"

    # E45: and the fix for it, which is not in this repository. On a real WSL
    # host the directory IS discoverable -- WSL writes /etc/ld.so.conf.d/
    # ld.wsl.conf itself, verbatim as reproduced below -- so a launcher that
    # reads the plain-text conf finds it without touching the binary cache that
    # caused the segfault the cache patch exists for. That is exactly what
    # host_library_dirs() in patches/sharun-library-path.patch computes; the
    # derivation here is a shell stand-in for it, in a THROWAWAY container, and
    # no host file is touched (section 7).
    mkdir -p /etc/ld.so.conf.d
    printf '# generated by WSL, reproduced from the real file on this machine\n/usr/lib/wsl/lib\n' \
        > /etc/ld.so.conf.d/ld.wsl.conf
    [ -f /etc/ld.so.conf ] || printf 'include /etc/ld.so.conf.d/*.conf\n' > /etc/ld.so.conf
    CONFDIRS=$(conf_dirs)
    echo "  /etc/ld.so.conf names: $CONFDIRS"
    case ":$CONFDIRS:" in
        *":$WSLLIB:"*)
            run E45 OK "round-tripped through the GPU and verified" \
                under_at 1 "$CONFDIRS" /w/build/cudaprobe "$CUDA" ;;
        *)  skip E45 "the conf-derived path does not contain $WSLLIB, so this would not be measuring the patch" ;;
    esac

    # E46: the vendor's own binary. cudaprobe is ours; nvidia-smi is theirs, it
    # dlopens libnvidia-ml.so.1 by itself, and on Alpine it cannot run at all
    # without the AppImage's runtime -- musl's ld.so will not load a glibc
    # executable. That last part is the control this section otherwise lacks.
    if [ -x "$WSLLIB/nvidia-smi" ]; then
        run E46 OK "GPU 0:" under_at 1 "$WSLLIB" "$WSLLIB/nvidia-smi" -L
        # E46a: the same binary with no AppImage runtime under it. LD_LIBRARY_PATH
        # stands in for the /etc/ld.so.cache entry WSL writes, so this measures
        # the LOADER and not the discovery gap E44 already covers -- without it
        # the glibc host fails for E44's reason and says nothing about libc.
        nat=$(env LD_LIBRARY_PATH="$WSLLIB" "$WSLLIB/nvidia-smi" -L 2>&1); nrc=$?
        natline=$(printf '%s' "$nat" | tr '\n' ' ' | cut -c1-58)
        if [ "$HOSTLIBC" = musl ]; then
            # It must fail, and fail for the RIGHT reason. A missing library is
            # also non-zero, and that would be E44's finding rather than this
            # one, so both loaders' shared-library wording is excluded by name.
            if printf '%s' "$nat" | grep -qiE 'loading shared librar'; then
                why=lib
            else
                why=loader
            fi
            [ "$nrc" -ne 0 ] && [ "$why" = loader ] && r46=1 || r46=0
            verdict E46a "$r46" "musl host: the same binary does not run without the AppImage runtime -- $natline"
        else
            printf '%s' "$nat" | grep -q 'GPU 0:' && r46=1 || r46=0
            verdict E46a "$r46" "glibc host: it also runs natively, as it should -- $natline"
        fi
    else
        skip E46 "no $WSLLIB/nvidia-smi"
        skip E46a "depends on E46"
    fi
fi

# ---------------------------------------- G. the cross-libc ABI, at last
#
# T1.3 - T1.7 were SKIPPED and UNVERIFIED for the whole project. Every other
# result here says a host object LOADS and RUNS; these ask whether the data
# passing between it and the process means the same thing on both sides.
#
# The guest is one source file built twice: by glibc on the floor (E47, the
# control) and by musl on Alpine (E48/E49). Under the bundled glibc 2.44 the
# musl build is loaded through foreign-dlopen itself, which is what drops its
# libc edge -- no patchelf, no stand-in, the real code path.
echo
echo "-- G. cross-libc ABI: does the DATA mean the same on both sides? ---"
if [ ! -x /w/build/abi-host ]; then
    skip E47 "abi-host was not built on the floor"
    skip E48 "depends on E47"; skip E49 "depends on E47"; skip E50 "depends on E47"
else
    # E47: same libc on both sides. Establishes that the 27 checks can pass at
    # all, so a musl-side failure is attributable to the crossing.
    run E47 OK "ABI CROSSING PASSED" under 1 /w/build/abi-host \
        /w/build/libabi_glibc.so glibc

    if [ ! -f /w/build/libabi_musl.so ]; then
        skip E48 "no musl-built guest in /w/build: run 45-build-musl-guest.sh first"
        skip E49 "depends on E48"; skip E50 "depends on E48"
    else
        # E48: the control that FAILS. With the feature off the bundled ld.so
        # goes looking for libc.musl-x86_64.so.1 and does not find it, which is
        # what makes E49 a measurement rather than a coincidence.
        run E48 FAIL "libc.musl-x86_64.so.1" under 0 /w/build/abi-host \
            /w/build/libabi_musl.so musl
        # E49: and with it on, every crossing holds -- allocator, errno, FILE*,
        # mutex and condvar -- with one libc in the process.
        run E49 OK "ABI CROSSING PASSED" under 1 /w/build/abi-host \
            /w/build/libabi_musl.so musl

        # E50: and the part no loader can fix, stated as a number rather than
        # left as a worry. The guest's compiled-in offsets and constants are
        # its own; where they disagree with glibc's, a musl object reads the
        # wrong field out of a struct glibc filled. Two of the six hazards
        # REPORT.md listed turn out to be live and the rest benign, and this
        # case fails if that ever stops being true.
        # The names come out of the same run as the count. Hardcoding them
        # beside a measured number is how a report ends up describing a
        # different result from the one it counted.
        hazout=$(under 1 /w/build/abi-host /w/build/libabi_musl.so musl 2>&1)
        haz=$(printf '%s' "$hazout" | grep -c 'LIVE HAZARD')
        hazwhat=$(printf '%s' "$hazout" | grep -E '^ *DIFF ' |
                  sed 's/^ *DIFF  *//; s/  */ /g; s/ host=.*//' | tr '\n' ';')
        [ "$haz" -eq 2 ] && r50=1 || r50=0
        verdict E50 "$r50" "reading back a glibc-filled struct: $haz live hazard(s) -- ${hazwhat:-none}"
    fi
fi

# ------------------------------- H. Design R, with a real device on the end
#
# The OTHER half of the design, and until now the untested one. E17-E21 show
# the selector CHOOSING correctly on eight distros and refusing the mixed set
# that segfaults; none of them puts a driver on the end of the choice.
#
# Read these beside E31 and E32 on the same host. All three run the same host
# ICD, and they differ only in how the process got a libc it can satisfy:
#
#   E31  bundled runtime, feature off            no devices
#   E32  bundled runtime, feature on             1 device   (the shim half)
#   E51  host runtime, no feature at all         1 device   (the Design R half)
#
# The switch is FORCED here. Auto declines on this host and is right to: the
# bundled glibc is newer than the host's, so there is nothing to gain and a
# switch would only lose. What is being measured is whether the switched
# runtime can drive a real device, not whether it should have been chosen.
echo
echo "-- H. Design R: the host runtime, with a driver on the end --------"
RSEL=/w/build/runtime-select
if [ ! -x "$RSEL" ]; then
    skip E51 "runtime-select was not built on the floor"
    skip E52 "depends on E51"
elif [ "$HOSTLIBC" = musl ]; then
    skip E51 "musl host: there is no host GLIBC runtime set to switch to, which is why Design R declines here and the shim half is the only one available"
    skip E52 "musl host: as E51"
else
    plan=$(env APPDIR="$APPDIR" ANYLINUX_RUNTIME=host "$RSEL" --probe 2>&1)
    if ! printf '%s' "$plan" | grep -q 'runtime      : host'; then
        why=$(printf '%s' "$plan" | sed -n 's/^reason *: //p' | cut -c1-90)
        skip E51 "this host's runtime set was refused, correctly: $why"
        skip E52 "depends on E51"
    else
        printf '  %s\n' "$(printf '%s' "$plan" | sed -n 's/^library-path : //p' | cut -c1-150)"
        # E51: a graphics driver through the switched runtime. No preload, no
        # ANYLINUX_LIB_FOREIGN_DLOPEN: the host ICD's own dependencies resolve
        # because the host library directories are on the path, which is the
        # whole of what Design R does.
        run E51 OK "OK: 1 physical device" \
            env APPDIR="$APPDIR" ANYLINUX_RUNTIME=host "$RSEL" -- /w/build/vkprobe
        # E52: and the same through to real silicon. This needs
        # /usr/lib/wsl/lib on the path, which the selector now derives from
        # /etc/ld.so.conf -- the file E45 put in place, verbatim from a real
        # WSL distro, and the same computation the sharun patch does.
        if [ -e /dev/dxg ] && [ -f "$CUDA" ]; then
            run E52 OK "round-tripped through the GPU and verified" \
                env APPDIR="$APPDIR" ANYLINUX_RUNTIME=host "$RSEL" -- \
                /w/build/cudaprobe "$CUDA"
        else
            skip E52 "no /dev/dxg with a WSL vendor driver on this host"
        fi
    fi
fi

# --------------------------------- I. rendering on hardware, for the first time
#
# Every rendering result above this line is Mesa lavapipe or llvmpipe: software
# rasterisers that exercise the identical dlopen path and draw every pixel on
# the CPU. "No GPU" was the standing caveat of the whole project.
#
# It was wrong for a second reason. WSL2 publishes no DRM render node, so radv,
# anv and radeonsi cannot initialise -- but Mesa's d3d12 GALLIUM driver does not
# need one. It talks to /dev/dxg through Microsoft's libdxcore, and Debian
# packages it as dri/d3d12_dri.so. That makes the host's own OpenGL driver a
# hardware driver, and the AppImage's bundled libglvnd has a real vendor library
# to dlopen at last.
#
# E53a is the third independent sighting of one bug. The AppImage fails on this
# driver with `glXCreateContext failed`, which reads like a display or driver
# fault and is neither: d3d12_dri.so dlopens libd3d12.so by BARE SONAME, sharun
# assembles the only search path there is, and its host-GPU directory list is
# hardcoded -- /run/opengl-driver/lib and /run/current-system/sw/lib are on it,
# /usr/lib/wsl/lib is not. E44 is the same bug in CUDA and E52 is the same bug
# in Design R. All three are what patches/sharun-library-path.patch computes.
echo
echo "-- I. the host's GL driver on REAL hardware ------------------------"
D3D12=$(ls /usr/lib/*/dri/d3d12_dri.so /usr/lib/dri/d3d12_dri.so 2>/dev/null | head -1)
if [ ! -e /dev/dxg ]; then
    skip E53a "no /dev/dxg: nothing here can reach a GPU"
    skip E53  "no /dev/dxg"; skip E53b "no /dev/dxg"
elif [ -z "$D3D12" ]; then
    skip E53a "no dri/d3d12_dri.so on this host: its Mesa has no Vulkan-or-GL-on-D3D12 driver, so /dev/dxg cannot become a GL device"
    skip E53  "as E53a"; skip E53b "as E53a"
elif ! command -v xvfb-run >/dev/null 2>&1; then
    skip E53a "no xvfb-run on this host: install xvfb"
    skip E53  "as E53a"; skip E53b "as E53a"
elif ! ls /usr/lib/libGLX_*.so.0 >/dev/null 2>&1 && ! ls /usr/lib/*/libGLX_*.so.0 >/dev/null 2>&1; then
    skip E53a "no libGLX_<vendor>.so.0 on this host; its Mesa is not libglvnd, so the bundled libglvnd has no vendor to dlopen"
    skip E53  "as E53a"; skip E53b "as E53a"
else
    echo "  host GL driver : $D3D12"
    HOSTDIRS=$(conf_dirs)
    # E53a: the AppImage exactly as it stands, on the hardware driver.
    run E53a OK "glXCreateContext failed" render_verdict_hw 1 "" glxgears -info
    # E53: the same command with the directories the host's own ld.so.conf
    # names handed to sharun through its own fallback knob -- no file edited,
    # nothing patched, exactly what the patch would have computed.
    run E53  OK "GL_RENDERER   = D3D12" render_verdict_hw 1 "$HOSTDIRS" glxgears -info
    # E53b: and the A/B, which does NOT flip. The host GL stack here is
    # glibc-built against an older glibc than the bundle, so there is nothing
    # for the shim to do -- the same result as E41b and for the same reason.
    # What E53 measures is hardware, not the shim; saying otherwise would be
    # claiming a control that did not happen.
    run E53b OK "GL_RENDERER   = D3D12" render_verdict_hw 0 "$HOSTDIRS" glxgears -info
fi

# Leave the AppDir holding the patched preload, so a later run that starts
# mid-suite is not silently measuring upstream's.
use_preload patched

echo
echo "================================================================"
echo " predictions matched: $PASS   mismatched: $FAIL   skipped: $SKIP"
echo "================================================================"
[ "$FAIL" -eq 0 ]
