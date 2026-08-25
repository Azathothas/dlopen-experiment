#!/bin/sh
# glibc host, OLDER than the AppImage's bundled glibc, so no host object NEEDS
# rewriting. Turning the feature on must not break what already worked.
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq --no-install-recommends \
    mesa-vulkan-drivers libvulkan1 vulkan-tools mesa-utils \
    libgl1-mesa-dri libglx-mesa0 xvfb xauth >/dev/null 2>&1
exec sh /scripts/40-appimage.sh
