#!/bin/sh
# Extract the demo AppImage. Done in a container because the payload is DwarFS
# and --appimage-extract runs the AppImage's own ELF runtime.
set -eu
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq --no-install-recommends file >/dev/null 2>&1
cd /w
rm -rf AppDir squashfs-root
chmod +x demo.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 ./demo.AppImage --appimage-extract >/dev/null 2>&1 || true
[ -d squashfs-root ] && mv squashfs-root AppDir
[ -d AppDir ] || { echo "extraction produced no AppDir"; exit 1; }
# Keep upstream's shim beside ours so the A/B can switch between them.
cp AppDir/lib/foreign-dlopen.so AppDir/lib/foreign-dlopen.upstream.so
echo "AppDir: $(ls AppDir/lib | wc -l) libraries, bundled glibc $(strings AppDir/lib/libc.so.6 2>/dev/null | sed -n 's/.*stable release version \([0-9.]*\)\..*/\1/p' | head -1)"
