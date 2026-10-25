#!/bin/sh
# Generates DTS test files and reference decodes in test-media/. The generated
# clips are made afresh on every run (with new references); clips you add are
# left alone and get references only once.
#
# Needs an ffmpeg with the (experimental) DTS encoder; set FFMPEG to use a
# specific one. Extra DTS files, e.g. short DTS-HD MA clips cut with
# `ffmpeg -ss 600 -t 10 -i movie.mkv -map 0:a:0 -c copy test-media/hdma.mkv`,
# can be added to test-media/ by hand; reference decodes are made for every
# test-media/*.{mkv,dts} that lacks one. (LG's MPEG-TS and MP4 demuxers do not
# expose DTS tracks at all, so there is no point testing those containers.)
set -eu

cd "$(dirname "$0")/.."
FFMPEG=${FFMPEG:-ffmpeg}
out=test-media
mkdir -p "$out"

ff() {
    "$FFMPEG" -hide_banner -loglevel error -y "$@"
}

# Generates a clip, the last argument. References made from an earlier
# version of it (another FFmpeg, say) would no longer match, so drop them.
clip() {
    for last; do :; done
    rm -f "$last".ref.*
    ff "$@"
}

tone() { # tone <frequency> <seconds> <label>
    echo "sine=frequency=$1:sample_rate=48000:duration=$2,volume=0.25[$3]"
}

# 5.1: a different tone on each channel (FL FR FC LFE SL SR), so a channel
# order mistake shows up in the comparison.
clip -filter_complex "$(tone 300 5 c0);$(tone 400 5 c1);$(tone 500 5 c2);$(tone 60 5 c3);$(tone 700 5 c4);$(tone 800 5 c5);[c0][c1][c2][c3][c4][c5]join=inputs=6:channel_layout=5.1(side)[a]" \
    -map '[a]' -c:a dca -strict experimental "$out/tones-5.1.mkv"
# Unframed, to exercise the decoder's own frame splitting.
clip -i "$out/tones-5.1.mkv" -c copy -f dts "$out/tones-5.1.dts"
# The same with four zero bytes after every frame, which some streams have
rm -f "$out/padded-5.1.dts".ref.*
python3 - "$out/tones-5.1.dts" "$out/padded-5.1.dts" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
out, i = bytearray(), 0
while i + 8 <= len(data):
    size = (((data[i + 5] & 3) << 12) | (data[i + 6] << 4) | (data[i + 7] >> 4)) + 1
    out += data[i:i + size] + b"\0" * 4
    i += size
open(sys.argv[2], "wb").write(out)
PY

clip -filter_complex "$(tone 440 5 l);$(tone 660 5 r);[l][r]join=inputs=2:channel_layout=stereo[a]" \
    -map '[a]' -c:a dca -strict experimental "$out/tones-stereo.mkv"

# Channel identification for listening tests (tests/tv-channel-test.sh): each
# 5.1 channel beeps on its own, in order, as many times as its number
# (FL 1, FR 2, FC 3, LFE 4 as a low hum, SL 5, SR 6), at -20 dBFS.
expr=
for k in 0 1 2 3 4 5; do
    f=700
    [ $k -eq 3 ] && f=80
    beep="0.1*sin(2*PI*$f*t)*between(t,$((k * 3)),$((k * 3))+0.4*$((k + 1))-0.2)*lt(mod(t-$((k * 3)),0.4),0.2)"
    expr=${expr:+$expr|}$beep
done
clip -f lavfi -i "aevalsrc='$expr':s=48000:d=18:c=5.1(side)" \
    -c:a dca -strict experimental "$out/channel-id-5.1.mkv"

# For listening tests: 2 s of 440 Hz at -40 dBFS on both channels.
clip -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2,volume=0.01" \
    -ac 2 -c:a dca -strict experimental "$out/quiet-stereo.mkv"

# Reference decodes, interleaved S32LE in FFmpeg's channel order. They are
# made from the bare DTS stream: FFmpeg would otherwise trim encoder padding
# using Matroska metadata that GStreamer's matroskademux ignores.
for f in "$out"/*.mkv "$out"/*.dts; do
    [ -f "$f" ] || continue
    ref=$f.ref.s32
    [ -f "$ref" ] && continue
    # no pipefail in sh: only keep a non-empty result
    "$FFMPEG" -hide_banner -loglevel error -i "$f" -map 0:a:0 -c copy -f dts - |
        ff -f dts -i - -f s32le -c:a pcm_s32le "$ref.tmp" || true
    if [ -s "$ref.tmp" ]; then
        mv "$ref.tmp" "$ref"
        echo "made $ref"
    else
        rm -f "$ref.tmp"
        echo "could not decode $f" >&2
    fi
done

# IEC 61937 references for the passthrough modes (streams with a DTS core)
for f in "$out"/*.mkv "$out"/*.dts; do
    [ -f "$f" ] && [ ! -f "$f.ref.spdif" ] || continue
    if "$FFMPEG" -hide_banner -loglevel quiet -i "$f" -map 0:a:0 -c copy \
        -f spdif "$f.ref.spdif.tmp" && [ -s "$f.ref.spdif.tmp" ]; then
        mv "$f.ref.spdif.tmp" "$f.ref.spdif"
        echo "made $f.ref.spdif"
    else
        rm -f "$f.ref.spdif.tmp"
    fi
done
