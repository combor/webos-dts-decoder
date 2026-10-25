#!/usr/bin/env python3
"""Compares a decode with a reference decode (both interleaved S32LE).

usage: compare.py <decoded> <reference> <channels> [--exact | --downmix]

Prints the length difference, the largest sample difference and the level of
each channel. Exits with 3 if the decode is silent, with 2 if the lengths
differ, and with 1 if the samples differ by more than float rounding (or at
all, with --exact).

With --downmix, <decoded> is the element's stereo downmix of a 5.1(side)
(FL FR FC LFE SL SR) reference, and is compared with the same Lo/Ro matrix
applied to the reference. Standard library only.
"""

import math
import sys
from array import array


def load(path):
    samples = array("i")
    with open(path, "rb") as f:
        samples.frombytes(f.read())
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def level_db(values):
    if not values:
        return float("-inf")
    rms = math.sqrt(sum(float(v) * v for v in values) / len(values))
    return 20 * math.log10(rms / 2**31) if rms else float("-inf")


def downmix_51(ref):
    """Lo/Ro of FL FR FC LFE SL SR: centre and surrounds at -3 dB, no LFE,
    scaled so it cannot clip (as the element does)."""
    m3db = 0.70710678
    scale = 1 / (1 + 2 * m3db)
    out = array("i")
    for i in range(0, len(ref) - 5, 6):
        fl, fr, fc, _, sl, sr = (v / 2**31 for v in ref[i:i + 6])
        for v in (fl + m3db * (fc + sl), fr + m3db * (fc + sr)):
            v = round(v * scale * 2**31)
            out.append(max(-2**31, min(2**31 - 1, v)))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    exact = "--exact" in sys.argv
    if len(args) != 3:
        sys.exit(__doc__)
    out, ref, channels = load(args[0]), load(args[1]), int(args[2])
    if "--downmix" in sys.argv:
        if channels != 6:
            sys.exit("--downmix needs a 6-channel reference")
        ref, channels = downmix_51(ref), 2

    frames_out, frames_ref = len(out) // channels, len(ref) // channels
    n = min(frames_out, frames_ref) * channels
    max_diff = max((abs(out[i] - ref[i]) for i in range(n)), default=0)
    diff_db = 20 * math.log10(max_diff / 2**31) if max_diff else float("-inf")

    print(f"  frames: decoded {frames_out}, reference {frames_ref}")
    print(f"  max difference: {max_diff} ({diff_db:.1f} dBFS)")
    print("  channel levels (dBFS): " + " ".join(
        f"{level_db(out[c:n:channels]):.1f}" for c in range(channels)))

    if not frames_out or not frames_ref:
        print("  MISMATCH: empty")
        sys.exit(2)
    if not any(out):
        print("  MISMATCH: silent")
        sys.exit(3)
    if frames_out != frames_ref:
        print("  MISMATCH: different length")
        sys.exit(2)
    ok = max_diff == 0 if exact else diff_db < -100
    print("  " + ("OK" if ok else "MISMATCH"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
