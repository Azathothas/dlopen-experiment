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
echo "================================================================"
echo " predictions matched: $PASS   mismatched: $FAIL"
echo "================================================================"
[ "$FAIL" -eq 0 ]
