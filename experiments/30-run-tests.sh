#!/bin/bash
# Stage 3 -- runs in Debian bullseye (glibc 2.31), standing in for "an AppImage that
# bundles an older glibc". Every experiment states its PREDICTION; the harness reports
# MATCH / MISMATCH against it. A MISMATCH is a finding, not a failure of the harness.
set -u
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq gcc binutils python3 >/dev/null 2>&1
cd /work

BASE_GLIBC=$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')
PASS=0; FAIL=0

# ---------------------------------------------------------------- helpers
cat > strip_ver.py <<'PEOF'
# Neutralise DT_VERSYM/VERNEED/VERDEF/VERDEFNUM by retagging them to an unknown tag
# ld.so ignores -- exactly what foreign-dlopen.c does. All four must go together:
# a verdef without its versym segfaults ld.so.
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
phoff, = struct.unpack_from('<Q', d, 0x20)
phnum, = struct.unpack_from('<H', d, 0x38)
for i in range(phnum):
    t, _, off, _, _, fsz, _, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i*56)
    if t != 2:            # PT_DYNAMIC
        continue
    for j in range(fsz // 16):
        tag, = struct.unpack_from('<q', d, off + j*16)
        if tag == 0:
            break
        if tag in (0x6ffffff0, 0x6ffffffe, 0x6ffffffc, 0x6ffffffd):
            struct.pack_into('<q', d, off + j*16, 0x414e594c)   # 'ANYL'
open(sys.argv[2], 'wb').write(d)
PEOF

cat > loader.c <<'CEOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
/* argv[1]=library  argv[2]=symbol  argv[3]="m" for dlmopen(LM_ID_NEWLM) */
int main(int argc, char **argv) {
    void *h;
    if (argc > 3 && argv[3][0] == 'm') h = dlmopen(LM_ID_NEWLM, argv[1], RTLD_NOW);
    else                               h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("FAILED: %s\n", dlerror()); return 1; }
    int (*f)(void) = dlsym(h, argv[2]);
    if (!f) { printf("FAILED: no symbol %s\n", argv[2]); return 1; }
    printf("OK: %s()=%d\n", argv[2], f());
    return 0;
}
CEOF
gcc -O2 loader.c -o loader     -ldl
gcc -O2 loader.c -o loader_pth -ldl -Wl,--no-as-needed -lpthread

cat > shim_atexit.c <<'CEOF'
/* musl exports atexit dynamically; glibc keeps it in the static libc_nonshared.a,
   so it is absent from libc.so.6 and unresolvable for a musl-built guest. */
#include <stddef.h>
extern int __cxa_atexit(void (*)(void *), void *, void *);
__attribute__((visibility("default")))
int atexit(void (*fn)(void)) { return __cxa_atexit((void (*)(void *))fn, NULL, NULL); }
CEOF
gcc -shared -fPIC -O2 shim_atexit.c -o shim_atexit.so

cat > shim_forward.c <<'CEOF'
/* Forward-compatibility shim: implements symbols that exist only in NEWER glibc,
   over the older glibc we are actually running on. Generatable from glibc's own
   symbol tables -- this hand-written sample covers the two the test needs. */
#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#define VIS __attribute__((visibility("default")))
VIS size_t strlcpy(char *d, const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n - 1 ? sl : n - 1; memcpy(d, s, c); d[c] = 0; }
    return sl;
}
VIS size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
VIS void arc4random_buf(void *b, size_t n) {
    while (n) { long r = syscall(SYS_getrandom, b, n, 0); if (r <= 0) break;
                b = (char *)b + r; n -= (size_t)r; }
}
VIS unsigned arc4random(void) { unsigned v = 0; arc4random_buf(&v, sizeof v); return v; }
VIS unsigned arc4random_uniform(unsigned u) { return u ? arc4random() % u : 0; }
CEOF
gcc -shared -fPIC -O2 shim_forward.c -o shim_forward.so

python3 strip_ver.py libprobe_nomusl.so libprobe_s.so 2>/dev/null || true
python3 strip_ver.py libnew.so  libnew_s.so
python3 strip_ver.py libthr.so  libthr_s.so

# ---------------------------------------------------------------- harness
run() {                       # run <id> <expect: OK|FAIL> <expect-substring> <cmd...>
    local id="$1" want="$2" needle="$3"; shift 3
    local out rc
    out=$("$@" 2>&1); rc=$?
    local got="OK"; [ $rc -ne 0 ] && got="FAIL"
    local verdict="MISMATCH"
    if [ "$got" = "$want" ] && printf '%s' "$out" | grep -qF "$needle"; then
        verdict="MATCH"; PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
    fi
    printf '  %-6s %-4s predicted=%-4s  %s\n' "$id" "$verdict" "$want" "$(printf '%s' "$out" | head -1)"
}

echo "================================================================"
echo " Cross-libc dlopen evidence  --  base process glibc $BASE_GLIBC"
echo "================================================================"

echo
echo "-- A. musl host library into a glibc process ------------------"
run E1 FAIL "undefined symbol: atexit" \
    ./loader /work/libprobe_nomusl.so probe_answer
run E2 OK   "probe_answer()=42" \
    env LD_PRELOAD=/work/shim_atexit.so ./loader /work/libprobe_nomusl.so probe_answer

echo
echo "-- B. NEWER-glibc host library into an OLDER glibc process ----"
run E3 FAIL "GLIBC_2.38' not found" \
    ./loader /work/libnew.so newlib_answer
run E4 FAIL "undefined symbol: arc4random" \
    ./loader /work/libnew_s.so newlib_answer
run E5 OK   "newlib_answer()=99" \
    env LD_PRELOAD=/work/shim_forward.so ./loader /work/libnew_s.so newlib_answer

echo
echo "-- C. re-homed symbols (glibc 2.34 library consolidation) -----"
run E6 FAIL "undefined symbol: pthread_create" \
    ./loader /work/libthr_s.so thr_answer
run E7 OK   "thr_answer()=77" \
    ./loader_pth /work/libthr_s.so thr_answer

echo
echo "-- D. can a SECOND libc be loaded in-process? -----------------"
run E8 FAIL "GLIBC_2.35' not found" \
    ./loader /work/newglibc/libc.so.6 __libc_start_main
cp libnew.so libnew_rp.so
if command -v patchelf >/dev/null 2>&1; then patchelf --set-rpath /work/newglibc libnew_rp.so; fi
run E9 FAIL "GLIBC_2.35' not found" \
    ./loader /work/newglibc/libc.so.6 __libc_start_main m

echo
echo "-- E. exec-time whole-runtime switch: the answer for symbols we cannot predict"
printf '#include <stdio.h>\nint main(void){puts("hello from switched runtime");return 0;}\n' > h.c
gcc -O2 h.c -o h
run E10 OK "hello from switched runtime" \
    /work/newglibc/ld-linux-x86-64.so.2 --library-path /work/newglibc ./h

echo "  E11    (informational) mixing an OLD libdl with a NEW libc:"
/work/newglibc/ld-linux-x86-64.so.2 --library-path /work/newglibc:/lib/x86_64-linux-gnu \
    ./loader /work/libnew.so newlib_answer >/dev/null 2>&1
echo "         exit=$?  (139 = SIGSEGV: the runtime set must be switched WHOLE)"

# E12 answers "how do you guarantee forward compat for a symbol that does not exist yet?".
# You do not shim it. You run under the host's own runtime, so the symbol resolves natively.
# Note NO shim is preloaded here -- contrast with E5.
run E12 OK "newlib_answer()=99" \
    /work/hostrt/ld-linux-x86-64.so.2 --library-path /work/hostrt ./loader /work/libnew.so newlib_answer

echo
echo "-- F. library search path: --library-path vs /etc/ld.so.cache ---"
# Anylinux patches ld-linux.so to skip /etc/ld.so.cache (it segfaults on some hosts),
# so --library-path becomes the ONLY discovery mechanism. --inhibit-cache reproduces
# that patched loader exactly. /usr/local/lib is a real dir on every distro surveyed
# and is absent from sharun's hardcoded list -- so this is a live gap, not a hypothetical.
echo "int foo_answer(void){return 55;}" > foo.c
mkdir -p /usr/local/lib
gcc -shared -fPIC -Wl,-soname,libfoo.so.1 foo.c -o /usr/local/lib/libfoo.so.1
ldconfig
cat > byname.c <<'CEOF'
#include <stdio.h>
#include <dlfcn.h>
int main(void){ void *h = dlopen("libfoo.so.1", RTLD_NOW);
    if(!h){ printf("FAILED: %s\n", dlerror()); return 1; }
    int (*f)(void) = dlsym(h,"foo_answer"); printf("OK: %d\n", f?f():-1); return 0; }
CEOF
gcc -O2 byname.c -o byname -ldl
LD=/lib64/ld-linux-x86-64.so.2
SHARUN_LIKE="/work:/usr/lib:/lib:/usr/lib64:/lib64:/usr/lib/x86_64-linux-gnu"   # no /usr/local/lib

run E13a OK   "OK: 55" $LD --library-path "$SHARUN_LIKE" ./byname
run E13b FAIL "cannot open shared object file" $LD --library-path "$SHARUN_LIKE" --inhibit-cache ./byname
run E13c OK   "OK: 55" $LD --library-path "/usr/local/lib:$SHARUN_LIKE" --inhibit-cache ./byname

# ===================================================================
#  G. THE FIX -- one case per change in ../src (one case per fix)
#
#  Everything above measures the problem. Everything below measures a
#  specific fix, and each one is written so it FAILS without that fix.
# ===================================================================
echo
echo "-- G. the fix ---------------------------------------------------"

if [ ! -f /repo/src/runtime-select.c ]; then
    echo "  E14..E20  SKIPPED - the repo is not mounted at /repo"
else
    # ---- Tier 0: the ELF rewriting, tested against the REAL implementation --
    # tests/elf-selftest.c #includes foreign-dlopen.c, so T0.4/T0.5/T0.7/T0.8
    # exercise the shipped predicates rather than a model of them.
    gcc -O2 -Wno-format-truncation /repo/tests/elf-selftest.c \
        -o elf-selftest -ldl 2>/dev/null
    run E14 OK "ELF SELFTEST PASSED" ./elf-selftest /lib/x86_64-linux-gnu/libz.so.1

    # ---- Design B: the GENERATED shim ----
    # Generated here, in the container, for THIS process's glibc 2.31 floor --
    # so the test covers the generator, not a checked-in snapshot of its output.
    if command -v python3 >/dev/null 2>&1; then
        python3 /repo/tools/gen_forward_shim.py \
            --floor /repo/inventories/glibc-2.31.json \
            --target /repo/inventories/glibc-2.44.json \
            --out gen-shim.c --manifest gen-shim.json --quiet 2>/dev/null

        # E15: it compiles clean against the floor it claims to target.
        run E15 OK "compiled" sh -c \
            'gcc -shared -fPIC -O2 -Wall -Werror gen-shim.c -o gen-shim.so 2>&1 && echo compiled'

        # E16: and the implementations are CORRECT, not merely present --
        #      ~40 documented behaviours, on a glibc that really lacks them.
        gcc -O2 /repo/tests/shim-selftest.c -o shimtest \
            -L"$PWD" -l:gen-shim.so -Wl,-rpath,"$PWD" >/dev/null 2>&1
        run E16 OK "SHIM TEST PASSED" ./shimtest
    else
        echo "  E15    SKIPPED - no python3 in this image"
        echo "  E16    SKIPPED - depends on E15"
    fi

    # ---- Design R: the host-runtime selector ----
    gcc -O2 -Wno-format-truncation /repo/src/runtime-select.c \
        -o runtime-select -ldl 2>/dev/null

    # An AppDir bundling glibc 2.41 -- NEWER than this container's 2.31 host.
    rm -rf app_new && mkdir -p app_new/lib && cp -L /work/hostrt/* app_new/lib/ 2>/dev/null

    # E17: host OLDER than bundled -> keep bundled, and say why.
    run E17 OK "is not newer than bundled" \
        env APPDIR="$PWD/app_new" ./runtime-select --probe

    # An AppDir bundling this host's own 2.31: equal, so still bundled.
    # Bundle exactly the members hostrt stages, so "incomplete" can never be
    # the reason a later test refuses -- E20 has to fail for the RIGHT reason.
    rm -rf app_old && mkdir -p app_old/lib
    for f in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
             libutil.so.1 libanl.so.1 libresolv.so.2; do
        cp -L "/lib/x86_64-linux-gnu/$f" app_old/lib/ 2>/dev/null || true
    done
    cp -L /lib64/ld-linux-x86-64.so.2 app_old/lib/ 2>/dev/null || true
    run E18 OK "runtime      : bundled" env APPDIR="$PWD/app_old" ./runtime-select --probe

    # E19: the override is real. Forcing bundled must be honoured verbatim.
    run E19 OK "forced by the user" \
        env APPDIR="$PWD/app_old" ANYLINUX_RUNTIME=bundled ./runtime-select --probe

    # E20: THE E11 GUARD, and the reason Design R is not just "pick the newer
    #      one". Hand the selector a MIXED set -- 2.41's ld.so and libc beside
    #      2.31's libdl and libpthread -- as if it were the host. E11 proved
    #      that combination segfaults on contact, so the only right answer is
    #      to refuse it. A selector that accepts it is worse than none.
    # Every member present -- so "incomplete" cannot be the reason -- but
    # libdl/libpthread/librt/libutil come from the OLD glibc.
    rm -rf mixedhost && mkdir -p mixedhost
    cp -L /work/hostrt/* mixedhost/ 2>/dev/null
    for f in libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1; do
        cp -L "/lib/x86_64-linux-gnu/$f" mixedhost/ 2>/dev/null || true
    done
    run E20 OK "NOT internally consistent" \
        env APPDIR="$PWD/app_old" ./runtime-select --probe --host-dir "$PWD/mixedhost"

    # E21: the control for E20. The SAME newer glibc, unmixed, must be
    #      ACCEPTED -- otherwise E20 would pass by refusing everything, which
    #      is not a guard, it is a broken selector.
    rm -rf goodhost && mkdir -p goodhost && cp -L /work/hostrt/* goodhost/ 2>/dev/null
    run E21 OK "runtime      : host" \
        env APPDIR="$PWD/app_old" ./runtime-select --probe --host-dir "$PWD/goodhost"
fi

echo
echo "================================================================"
echo " predictions matched: $PASS   mismatched: $FAIL"
echo "================================================================"
[ "$FAIL" -eq 0 ]
