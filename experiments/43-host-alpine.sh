#!/bin/sh
# musl host. The AppImage bundles glibc; the drivers are Alpine's, built
# against musl. This is the configuration the original complaint is about.
set -u
apk add --no-cache mesa-vulkan-swrast vulkan-loader vulkan-tools \
                   xvfb xvfb-run >/dev/null 2>&1
exec sh /scripts/40-appimage.sh
