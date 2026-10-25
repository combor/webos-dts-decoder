#!/bin/sh
# Builds and checks the plugin; results go to /out.
# Runs inside the build container, after build-ffmpeg.sh.
set -eu

. /build/scripts/flags.sh

export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig

so=libgstdtsswdec.so
mkdir -p /build/plugin /out
cd /build/plugin

# shellcheck disable=SC2046,SC2086
"${CROSS}gcc" $ARCH_CFLAGS $COMMON_CFLAGS \
    -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I/opt/ffmpeg/include $(pkg-config --cflags gstreamer-audio-1.0) \
    -shared -o "$so.unstripped" /build/src/gstdtsswdec.c \
    -Wl,--as-needed \
    /opt/ffmpeg/lib/libavcodec.a /opt/ffmpeg/lib/libavutil.a \
    $(pkg-config --libs gstreamer-audio-1.0) -lm -pthread -static-libgcc \
    -Wl,--version-script=/build/src/dtsswdec.map \
    -Wl,--exclude-libs,ALL \
    -Wl,--gc-sections \
    -Wl,--no-undefined \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
    -Wl,--build-id=sha1

"${CROSS}strip" --strip-unneeded -o "$so" "$so.unstripped"

fail() {
    echo "check failed: $*" >&2
    exit 1
}

# The only exported symbols are GStreamer's plugin entry points.
exports=$("${CROSS}readelf" --dyn-syms -W "$so" |
    awk '$5 == "GLOBAL" && $7 != "UND" { print $8 }' | sort)
echo "exported symbols:"
echo "$exports" | sed 's/^/  /'
echo "$exports" | grep -qx 'gst_plugin_dtsswdec_get_desc' ||
    fail "no plugin descriptor exported"
unexpected=$(echo "$exports" | grep -vx 'gst_plugin_dtsswdec_get_desc\|gst_plugin_dtsswdec_register' || true)
[ -z "$unexpected" ] || fail "unexpected exports: $unexpected"

# Libraries the TV is known to provide.
needed=$("${CROSS}readelf" -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
echo "needed libraries:"
echo "$needed" | sed 's/^/  /'
for lib in $needed; do
    case $lib in
    libgstaudio-1.0.so.0 | libgstbase-1.0.so.0 | libgstreamer-1.0.so.0 | \
        libgobject-2.0.so.0 | libglib-2.0.so.0 | libm.so.6 | libc.so.6 | \
        libpthread.so.0) ;;
    *) fail "unexpected library dependency $lib" ;;
    esac
done

"${CROSS}readelf" -d "$so" | grep -q TEXTREL && fail "text relocations"

# Oldest glibc that can load the plugin.
glibc=$("${CROSS}readelf" -V -W "$so" | grep -o 'GLIBC_[0-9.]*' |
    sed 's/GLIBC_//' | sort -u -t. -k1,1n -k2,2n -k3,3n | tail -n 1)
echo "requires glibc >= $glibc"
case $glibc in
2.[0-9] | 2.[0-9].* | 2.1[0-9] | 2.1[0-9].* | 2.2[0-8] | 2.2[0-8].*) ;;
*) fail "needs glibc $glibc, newer than webOS 5's 2.28" ;;
esac

cp "$so" /out/
cd /out
sha256sum "$so" >SHA256SUMS
