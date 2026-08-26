#!/bin/sh
# musl host. The AppImage bundles glibc; the drivers are Alpine's, built
# against musl. This is the configuration the original complaint is about.
#
# mesa-gl and mesa-dri-gallium are here for section J. Alpine's Mesa is CLASSIC
# Mesa -- built without libglvnd -- so it ships no libGLX_<vendor>.so.0 for the
# AppImage's bundled glvnd dispatcher to dlopen. Without those packages the
# OpenGL cases would skip for want of a host GL driver, which is a different
# reason from the one they are there to measure, and the two would be
# indistinguishable in the output.
set -u
apk add --no-cache mesa-vulkan-swrast vulkan-loader vulkan-tools \
                   mesa-gl mesa-dri-gallium mesa-egl mesa-gles \
                   python3 procps \
                   xvfb xvfb-run >/dev/null 2>&1
exec sh /scripts/40-appimage.sh
