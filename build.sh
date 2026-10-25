#!/bin/sh
# Builds out/libgstdtsswdec.so and out/SHA256SUMS in a pinned Debian buster
# container. Arguments are passed to `docker build` (e.g. --no-cache).
#
# Environment:
#   DOCKER  container CLI to use (default: docker), e.g. DOCKER=podman or
#           DOCKER="sudo docker"
#   IMAGE   name for the build image (default: webos-dts-decoder-build)
set -eu

cd "$(dirname "$0")"

DOCKER=${DOCKER:-docker}
IMAGE=${IMAGE:-webos-dts-decoder-build}

ffmpeg_version=$(sed -n 's/^ARG FFMPEG_VERSION=//p' Dockerfile)
ffmpeg_sha256=$(sed -n 's/^ARG FFMPEG_SHA256=//p' Dockerfile)
tarball=third_party/ffmpeg-$ffmpeg_version.tar.xz

if [ ! -f "$tarball" ]; then
    mkdir -p third_party
    echo "Downloading FFmpeg $ffmpeg_version"
    curl -fL --retry 3 -o "$tarball.part" \
        "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz"
    mv "$tarball.part" "$tarball"
fi
if ! echo "$ffmpeg_sha256  $tarball" | sha256sum -c - >/dev/null; then
    echo "error: $tarball does not match the pinned SHA-256; delete it and retry" >&2
    exit 1
fi

# $DOCKER is unquoted on purpose so that DOCKER="sudo docker" works.
# shellcheck disable=SC2086
$DOCKER build "$@" -t "$IMAGE" .

rm -rf out.new
mkdir out.new
# Stream the results out with tar, so the files belong to the current user
# whatever the container runtime runs as. out/ is only replaced on success.
# shellcheck disable=SC2086
$DOCKER run --rm --network none "$IMAGE" tar -C /out -cf - . | tar -C out.new -xf -
[ -f out.new/libgstdtsswdec.so ] && [ -f out.new/SHA256SUMS ] || {
    echo "error: the build produced no plugin" >&2
    rm -rf out.new
    exit 1
}
rm -rf out
mv out.new out

cat out/SHA256SUMS
