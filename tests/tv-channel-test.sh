#!/bin/sh
# Plays test-media/channel-id-5.1.mkv (see tests/make-media.sh) through the
# TV's own media pipeline, the path apps such as Plex use, for a listening
# test. Each channel beeps on its own as many times as its number: front left
# 1, front right 2, centre 3, LFE 4, surround left 5, surround right 6. With
# the default stereo downmix, TV speakers should give: left, right, both,
# nothing, left, right.
#
# The TV only plays sound for the app in the foreground, so first open an app
# such as Plex on the TV (not live TV or the home screen) and leave it idle.
# This makes sound (18 s at -20 dBFS).
#
# usage: tests/tv-channel-test.sh <ssh-destination>
set -eu

cd "$(dirname "$0")/.."
[ $# -eq 1 ] || {
    echo "usage: $0 <ssh-destination>" >&2
    exit 2
}
clip=test-media/channel-id-5.1.mkv
[ -f "$clip" ] || {
    echo "$clip not found; run tests/make-media.sh" >&2
    exit 1
}

# private directory, but readable by the media pipeline
dir=$(ssh "$1" 'd=$(mktemp -d /tmp/dtsdec-channel-test.XXXXXX) && chmod 755 "$d" && echo "$d"')
trap 'ssh "$1" "rm -rf $dir" || true' EXIT
ssh "$1" "cat >$dir/channel-id.mkv && chmod 644 $dir/channel-id.mkv" <"$clip"
ssh "$1" sh -s -- "$dir" <<'REMOTE'
dir=$1
trap 'rm -rf "$dir"' EXIT
call() {
    luna-send -n 1 "luna://com.webos.media/$1" "$2" </dev/null
}
app=$(luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}' \
    </dev/null | tr -d '\n' | sed -n 's/.*"appId" *: *"\([^"]*\)".*/\1/p')
case $app in
"" | com.webos.app.livetv | com.webos.app.home*)
    echo "error: open an app such as Plex on the TV first (foreground: ${app:-none})" >&2
    exit 1
    ;;
esac
id=$(call load "{\"uri\":\"file://$dir/channel-id.mkv\",\"type\":\"media\",\"payload\":{\"option\":{\"appId\":\"$app\"}}}" |
    tr -d '\n' | sed -n 's/.*"mediaId" *: *"\([^"]*\)".*/\1/p')
[ -n "$id" ] || {
    echo "error: the media server did not load the clip" >&2
    exit 1
}
sleep 3
echo "playing (18 s)..."
call play "{\"mediaId\":\"$id\"}" >/dev/null
sleep 20
call unload "{\"mediaId\":\"$id\"}" >/dev/null
echo "done"
REMOTE
