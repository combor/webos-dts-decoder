# webos-dts-decoder

LG removed DTS support from its 2020 webOS TVs, so apps like Plex cannot
play DTS audio on them. This brings it back: Plex plays DTS and DTS-HD MA
directly, with sound on the TV speakers, or as real DTS 5.1 on a soundbar
that decodes DTS.

You need a rooted TV with the Homebrew Channel and root SSH access. Rooting
is not covered here; [SlopBro](https://github.com/throwaway96/slopbro) does
it, and [webosbrew.org](https://www.webosbrew.org/rooting/) lists other
methods.

## Install

On a Linux PC with Docker:

```sh
./build.sh
./deploy/install.sh root@<tv-ip>
```

No reboot needed. It stays on after the TV restarts.

## Uninstall

```sh
./deploy/uninstall.sh root@<tv-ip>
```

## Setup

In the Plex app on the TV, open **Settings → Audio** and turn on **DTS**.
You now get DTS as stereo, on the TV speakers or a soundbar.

To send real DTS 5.1 to a soundbar that decodes DTS:

1. In the TV's sound settings, set the output to the HDMI ARC/eARC device
   and **Digital Sound Out** to **Pass Through**.
2. Run this on the TV:

   ```sh
   echo arc-passthrough > /var/lib/dtsdec/downmix
   ```

Whenever the TV's sound goes anywhere else, such as its own speakers, you get
stereo again automatically. Only turn this on with a soundbar that decodes
DTS: anything else plays it as loud noise. To turn it off:

```sh
echo stereo > /var/lib/dtsdec/downmix
```
