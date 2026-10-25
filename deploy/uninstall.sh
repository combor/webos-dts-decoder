#!/bin/sh
# Removes the DTS decoder from the TV and restores the stock GStreamer
# registry straight away, no reboot needed.
#
# usage: deploy/uninstall.sh <ssh-destination>
set -eu

[ $# -eq 1 ] || {
    echo "usage: $0 <ssh-destination>" >&2
    exit 2
}

ssh "$1" '
rm -f /var/lib/webosbrew/init.d/60-dtsdec
if [ -x /var/lib/dtsdec/dtsdec ]; then
    /var/lib/dtsdec/dtsdec stop || exit 1
fi
rm -rf /var/lib/dtsdec
echo "uninstalled"
'
