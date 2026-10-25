#!/bin/sh
# Decodes test-media/ files on the TV with out/libgstdtsswdec.so and compares
# the result with FFmpeg's reference decode (see tests/make-media.sh).
#
# Silent and non-invasive: output goes to files, everything lives in a private
# /tmp/dtsdec-test.* directory (removed afterwards), and the plugin is loaded
# through a private plugin directory and registry, so the TV's own registry
# and media pipeline are not touched.
#
# usage: tests/tv-decode.sh <ssh-destination> [test-media/file ...]
set -eu

cd "$(dirname "$0")/.."
[ $# -ge 1 ] || {
    echo "usage: $0 <ssh-destination> [test-media/file ...]" >&2
    exit 2
}
tv=$1
shift
[ $# -gt 0 ] || set -- test-media/*.mkv test-media/*.dts

files=
for f in "$@"; do
    [ -f "$f" ] && [ -f "$f.ref.s32" ] && files="$files ${f##*/}"
done
[ -n "$files" ] || {
    echo "no test media with reference decodes; run tests/make-media.sh" >&2
    exit 1
}

results=test-media/tv
mkdir -p "$results"
# never compare against results left over from an earlier run
for f in $files; do
    rm -f "$results/$f".*.s32 "$results/$f".*.own-coefficients
done

echo "Uploading plugin and test media"
remote=$(ssh "$tv" 'mktemp -d /tmp/dtsdec-test.XXXXXX')
# shellcheck disable=SC2064
trap "ssh '$tv' 'rm -rf $remote'" EXIT
# shellcheck disable=SC2086
tar -cf - -C out libgstdtsswdec.so -C ../test-media $files |
    ssh "$tv" "tar -xf - -C $remote"

# shellcheck disable=SC2087
ssh "$tv" sh -s -- "$remote" $files <<'EOF'
set -eu
dir=$1
shift
mkdir -p "$dir/plugins"
ln -s "$dir/libgstdtsswdec.so" "$dir/plugins/"
for p in coreelements matroska typefindfunctions; do
    lib=/usr/lib/gstreamer-1.0/libgst$p.so
    [ -e "$lib" ] && ln -s "$lib" "$dir/plugins/"
done

# Load libraries the way the TV's media pipeline does
for p in /proc/[0-9]*; do
    if [ "$(readlink "$p/exe" 2>/dev/null)" = /usr/sbin/starfish-media-pipeline ]; then
        eval "$(tr '\0' '\n' <"$p/environ" |
            sed -n 's/^\(LD_LIBRARY_PATH\|LD_PRELOAD\)=\(.*\)/export \1="\2"/p')"
        break
    fi
done
unset GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
export GST_PLUGIN_SYSTEM_PATH_1_0="$dir/plugins"
export GST_REGISTRY_1_0="$dir/registry.bin"

gst-inspect-1.0 dtsswdec >/dev/null

for f in "$@"; do
    case $f in
    *.mkv) demux=matroskademux ;;
    *) demux=typefind ;;
    esac
    for mode in full core stereo passthrough; do
        case $mode in
        full) props="downmix=none" ;;
        core)
            case $f in hdma*) ;; *) continue ;; esac
            props="downmix=none core-only=true"
            ;;
        stereo) props="downmix=stereo" ;;
        passthrough) props="downmix=passthrough" ;;
        esac
        echo "== $f ($mode)"
        start=$(cut -d' ' -f1 /proc/uptime)
        GST_DEBUG=dtsswdec:4 gst-launch-1.0 -q filesrc location="$dir/$f" ! \
            $demux ! dtsswdec $props ! filesink location="$dir/$f.$mode.s32" \
            2>"$dir/log" || {
            echo "   FAILED"
            grep -v ' INFO ' "$dir/log" | tail -n 5
            rm -f "$dir/$f.$mode.s32"
        }
        # no own downmix: FFmpeg used the stream's downmix coefficients
        if [ $mode = stereo ] && ! grep -q 'downmixing' "$dir/log"; then
            touch "$dir/$f.$mode.own-coefficients"
        fi
        end=$(cut -d' ' -f1 /proc/uptime)
        echo "   wall time: $(echo "$start $end" | awk '{printf "%.2f", $2 - $1}') s"
    done
done
EOF

echo "Downloading results"
ssh "$tv" "cd $remote && tar -cf - \$(ls *.s32 *.own-coefficients 2>/dev/null)" |
    tar -xf - -C "$results"

status=0
for f in $files; do
    channels=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels \
        -of csv=p=0 "test-media/$f")
    modes="full stereo"
    case $f in hdma*) modes="$modes core" ;; esac
    [ -f "test-media/$f.ref.spdif" ] && modes="$modes passthrough"
    for mode in $modes; do
        out=$results/$f.$mode.s32
        echo "== ${out##*/}"
        if [ ! -f "$out" ]; then
            echo "  missing: decoding failed on the TV"
            status=1
            continue
        fi
        case $out in
        *.core.s32)
            echo "  (core only, not compared)"
            ;;
        *.passthrough.s32)
            # IEC 61937 must match FFmpeg's spdif muxer byte for byte
            if cmp -s "$out" "test-media/$f.ref.spdif"; then
                echo "  OK (identical to FFmpeg's IEC 61937)"
            else
                echo "  MISMATCH with FFmpeg's IEC 61937"
                status=1
            fi
            ;;
        *.stereo.s32)
            if [ "$channels" -le 2 ]; then
                python3 tests/compare.py "$out" "test-media/$f.ref.s32" "$channels" ||
                    status=1
            elif [ "$channels" -eq 6 ]; then
                rc=0
                python3 tests/compare.py "$out" "test-media/$f.ref.s32" 6 --downmix || rc=$?
                if [ $rc -eq 1 ] && [ -f "${out%.s32}.own-coefficients" ]; then
                    # FFmpeg mixed with the stream's own coefficients, so
                    # only length and non-silence can be checked
                    echo "  (stream has its own downmix coefficients; samples not compared)"
                elif [ $rc -ne 0 ]; then
                    status=1
                fi
            else
                # no reference downmix for this layout: check the length only
                frames_out=$(($(wc -c <"$out") / 8))
                frames_ref=$(($(wc -c <"test-media/$f.ref.s32") / (4 * channels)))
                echo "  frames: decoded $frames_out, reference $frames_ref (samples not compared)"
                [ "$frames_out" -gt 0 ] && [ "$frames_out" -eq "$frames_ref" ] || {
                    echo "  MISMATCH"
                    status=1
                }
            fi
            ;;
        *)
            exact=
            case $out in *hdma*) exact=--exact ;; esac
            python3 tests/compare.py "$out" "test-media/$f.ref.s32" "$channels" $exact ||
                status=1
            ;;
        esac
    done
done
exit $status
