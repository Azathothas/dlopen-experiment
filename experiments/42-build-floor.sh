#!/bin/sh
# Build the preload and the probes on the OLDEST supported glibc, so they only
# ever need symbols the AppImage's bundled runtime is guaranteed to have.
set -eu
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq --no-install-recommends gcc libc6-dev make python3 binutils >/dev/null 2>&1
mkdir -p /build/src && cd /build/src
cp /repo/src/*.c /repo/src/*.h /repo/src/Makefile .
mkdir -p /build/inventories /build/tools
cp /repo/inventories/* /build/inventories/ 2>/dev/null || true
cp /repo/tools/* /build/tools/ 2>/dev/null || true
cp /repo/elfsym.py /build/ 2>/dev/null || true
make 2>&1 | tail -3
mkdir -p /w/build
cp foreign-dlopen.so runtime-select /w/build/
for t in icd-harness vkprobe corpus invariants soak; do
    gcc -O2 -o /w/build/$t /repo/tests/$t.c -ldl
done
echo "floor build ok. preload needs at most: $(objdump -T foreign-dlopen.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
