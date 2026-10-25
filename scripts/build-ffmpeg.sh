#!/bin/sh
# Builds a static, minimal FFmpeg containing only the DTS decoder.
# Runs inside the build container; usage: build-ffmpeg.sh <version>
set -eu

. /build/scripts/flags.sh

version=$1
cd /build
tar -xf "ffmpeg-$version.tar.xz"
cd "ffmpeg-$version"

# LGPL only: no --enable-gpl, no --enable-nonfree.
./configure \
    --prefix=/opt/ffmpeg \
    --enable-cross-compile \
    --cross-prefix="$CROSS" \
    --arch=arm \
    --cpu=armv7-a \
    --target-os=linux \
    --extra-cflags="$ARCH_CFLAGS $COMMON_CFLAGS" \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-autodetect \
    --disable-everything \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-network \
    --disable-avdevice \
    --disable-avformat \
    --disable-avfilter \
    --disable-swscale \
    --disable-swresample \
    --enable-decoder=dca

make -j"$(nproc)"
make install
