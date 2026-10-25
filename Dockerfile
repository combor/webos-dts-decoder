# Build environment for the plugin. Debian 10 "buster" armel matches the
# userspace of LG's webOS 5 TVs (glibc 2.28, GLib 2.58, GStreamer 1.14.4,
# soft-float ABI). Every input is pinned so the same commit always produces
# the same binary. Run ./build.sh rather than building this directly.

# debian:buster-20220912-slim, built from Debian 10.13. That is the last
# release with armel packages (buster LTS dropped armel), and multiarch needs
# the amd64 and armel libc6 to be the same version.
FROM debian@sha256:7cd178f900adf3fe5659861895c9690750df784ca96839fa756c379af645747d

# The archive the base image was built from. No buster-security: it has no
# armel packages, and a newer amd64 libc6 would break the version match.
ARG DEBIAN_SNAPSHOT=20220912T000000Z

RUN set -eux; \
    { \
        echo "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} buster main"; \
        echo "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} buster-updates main"; \
    } > /etc/apt/sources.list; \
    dpkg --add-architecture armel; \
    apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Retries=5 update; \
    apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        crossbuild-essential-armel gcc libc6-dev make pkg-config xz-utils \
        libgstreamer1.0-dev:armel libgstreamer-plugins-base1.0-dev:armel; \
    rm -rf /var/lib/apt/lists/*

# build.sh downloads the tarball into third_party/ and reads these two lines.
ARG FFMPEG_VERSION=9.0.2
ARG FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

ENV SOURCE_DATE_EPOCH=0

COPY third_party/ffmpeg-${FFMPEG_VERSION}.tar.xz /build/
COPY scripts/flags.sh scripts/build-ffmpeg.sh /build/scripts/
RUN echo "${FFMPEG_SHA256}  /build/ffmpeg-${FFMPEG_VERSION}.tar.xz" | sha256sum -c - \
    && /build/scripts/build-ffmpeg.sh "${FFMPEG_VERSION}"

COPY src /build/src
COPY scripts/build-plugin.sh /build/scripts/
RUN /build/scripts/build-plugin.sh
