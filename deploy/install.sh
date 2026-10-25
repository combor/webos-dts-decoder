#!/bin/sh
# Installs the DTS decoder on a rooted LG webOS TV and activates it, no
# reboot needed. Re-run it to update.
#
# usage: deploy/install.sh <ssh-destination>     (e.g. root@<tv-ip>)
set -eu

cd "$(dirname "$0")/.."
[ $# -eq 1 ] || {
    echo "usage: $0 <ssh-destination>" >&2
    exit 2
}
tv=$1
[ -f out/libgstdtsswdec.so ] || {
    echo "error: out/libgstdtsswdec.so not found; run ./build.sh first" >&2
    exit 1
}

echo "Checking the TV"
ssh "$tv" sh -s <<'EOF'
fail() {
    echo "error: $*" >&2
    exit 1
}
[ "$(id -u)" = 0 ] || fail "log in as root"
[ -e /lib/ld-linux.so.3 ] ||
    fail "the TV's userspace is not 32-bit soft-float ARM; this build cannot load"
[ -x /usr/sbin/umediaserver ] && [ -x /usr/sbin/starfish-media-pipeline ] ||
    fail "the webOS media pipeline (umediaserver) was not found"
[ -d /var/lib/webosbrew ] ||
    fail "Homebrew Channel not found (no /var/lib/webosbrew)"
minor=$(gst-inspect-1.0 --version | sed -n 's/^GStreamer 1\.\([0-9]*\).*/\1/p')
[ -n "$minor" ] && [ "$minor" -ge 14 ] || fail "GStreamer 1.14 or newer is needed"
EOF

echo "Copying files"
staging=$(ssh "$tv" 'mktemp -d /tmp/dtsdec-install.XXXXXX')
trap 'ssh "$tv" "rm -rf $staging" || true' EXIT
tar -cf - -C out libgstdtsswdec.so -C ../deploy dtsdec 60-dtsdec |
    ssh "$tv" "tar -xf - -C $staging"

echo "Checking that the plugin loads"
# With the media pipeline's environment and a private registry, before
# anything is installed (this also catches an incompatible glibc)
ssh "$tv" sh -s -- "$staging" <<'EOF'
staging=$1
for p in /proc/[0-9]*; do
    if [ "$(readlink "$p/exe" 2>/dev/null)" = /usr/sbin/umediaserver ]; then
        eval "$(tr '\0' '\n' <"$p/environ" |
            sed -n "s/^\(LD_LIBRARY_PATH\|LD_PRELOAD\)=\(.*\)/export \1='\2'/p")"
        break
    fi
done
mkdir "$staging/plugins"
cp "$staging/libgstdtsswdec.so" "$staging/plugins/"
if ! out=$(GST_PLUGIN_PATH_1_0= GST_PLUGIN_SYSTEM_PATH_1_0="$staging/plugins" \
    GST_REGISTRY_1_0="$staging/registry.bin" GST_REGISTRY_UPDATE=yes \
    gst-inspect-1.0 dtsswdec 2>&1); then
    echo "error: the plugin does not load on this TV:" >&2
    echo "$out" | tail -n 5 >&2
    exit 1
fi
EOF

echo "Installing"
ssh "$tv" sh -s -- "$staging" <<'EOF'
set -e
staging=$1
mkdir -p /var/lib/dtsdec/plugins /var/lib/webosbrew/init.d
# Copy next to the destination, then rename: /tmp is another filesystem, and
# a rename replaces the file in one step, while a running media pipeline
# keeps the plugin it has loaded.
put() {
    cp "$staging/$1" "$2.new"
    chown 0:0 "$2.new"
    chmod "$3" "$2.new"
    mv -f "$2.new" "$2"
}
put libgstdtsswdec.so /var/lib/dtsdec/plugins/libgstdtsswdec.so 644
put dtsdec /var/lib/dtsdec/dtsdec 755
put 60-dtsdec /var/lib/webosbrew/init.d/60-dtsdec 755
EOF

echo "Activating"
ssh "$tv" '
if [ -e /var/lib/dtsdec/disabled ]; then
    rm /var/lib/dtsdec/disabled
    echo "(removed the kill switch /var/lib/dtsdec/disabled)"
fi
/var/lib/dtsdec/dtsdec start && /var/lib/dtsdec/dtsdec status
'
