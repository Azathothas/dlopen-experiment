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
        "$(printf '%s' "$out" | grep -m1 -iE 'GPU|GL_RENDERER|device|libc|load|SOAK|OK:|FAIL|zero|Error' | cut -c1-96)"
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

echo
echo "================================================================"
echo " predictions matched: $PASS   mismatched: $FAIL   skipped: $SKIP"
echo "================================================================"
[ "$FAIL" -eq 0 ]
