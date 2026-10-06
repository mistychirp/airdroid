# airdroid

An AirPlay receiver for a rooted Android phone. An iPhone (or anything else
that does AirPlay) plays to the phone, and the phone plays the audio through
its speaker or a USB DAC.

It runs [shairport-sync](https://github.com/mikebrady/shairport-sync) in
[Termux](https://github.com/termux/termux-app), with a few Android fixes, and a
KernelSU module that keeps it running in the background.

Tested on a Pixel 9a with Android 17, KernelSU 3.3.0 and Termux 0.118.3.

## What's in here

| Path | What it is |
| --- | --- |
| `build.sh` | Builds and installs everything on the Termux side. |
| `cancel_shim/` | `pthread_cancel` and friends for Android, which has none. shairport-sync relies on them to stop its threads. Includes a test. |
| `patches/` | Android fixes for shairport-sync 5.5.2 (see below). |
| `termux/` | The `airplay-*` scripts and the shairport-sync config. |
| `module/` | The `airplay_receiver` KernelSU module. |

## Install

Needs a rooted phone with KernelSU (Magisk-style modules should work too) and
Termux from GitHub or F-Droid (the Play Store build is outdated).

1. In Termux, clone this repository and run the build:

   ```sh
   pkg install git
   git clone https://github.com/mistychirp/airdroid.git
   cd airdroid
   ./build.sh
   ```

   This installs the build tools, builds the shim and runs its test, builds
   shairport-sync with the patches, and installs `airplay-run`,
   `airplay-start`, `airplay-stop`, `airplay-switch-output` and
   `~/.config/shairport-sync.conf`. An existing config is kept.

2. Zip the module, install it, and reboot. Termux's home folder isn't visible
   to the KernelSU manager's file picker, so install it with `ksud` (Termux
   needs root access in the manager for `su`):

   ```sh
   pkg install zip
   (cd module && zip -r ../airplay-receiver.zip .)
   su -c "ksud module install $PWD/airplay-receiver.zip"
   ```

After the reboot and the first unlock, the receiver starts by itself and shows
up on the iPhone as "Pixel 9a" (change `name` in the config).

## The module

A watcher checks the phone every 3 seconds:

- It starts the receiver when there's a network (Wi-Fi or the phone's own
  hotspot) and stops it when there isn't.
- It restarts the receiver when the network changes, because the mDNS
  responder reads the phone's addresses only at startup.
- When a USB DAC is plugged in or out, it moves playback to a fresh output
  without ending the AirPlay session.
- It restarts the receiver if it dies, backing off if it keeps dying.
- It raises the receiver's priority, because Android throttles Termux when
  it isn't on screen.

The receiver runs inside the Termux app through Termux's `RUN_COMMAND`, so the
module turns on `allow-external-apps` in Termux and makes Termux
battery-unrestricted. The Action button in the KernelSU manager turns the
receiver on and off. The watcher logs to `/data/adb/airplay_receiver/watcher.log`
and the receiver to `~/shairport.log` in Termux (the previous run is in
`~/shairport.log.1`).

Without the module, run `airplay-start` and `airplay-stop` in Termux.

## Android fixes

- **Thread cancellation** (`cancel_shim/`): Android's C library has no
  `pthread_cancel`. The shim emulates deferred cancellation with a signal and
  the linker's `--wrap` option on the blocking calls shairport-sync uses, so
  cleanup handlers run as on Linux.
- **Device ID** (`patches/0001`): Android hides MAC addresses from apps, so
  shairport-sync waited ten seconds at every start and then used all zeros.
  `airplay-run` passes a random, stable address in `SPS_DEVICE_MAC` instead.
- **mDNS on the hotspot** (`patches/0002`): with only the hotspot up there's
  no default route, so joining the mDNS group "on any interface" fails. The
  patch joins and sends on each interface.
- **Audio output**: PulseAudio plays through Android's AAudio output. Its
  timing is precise, while the OpenSL ES output's jitters by about 50 ms,
  which makes shairport-sync correct constantly and crackle.
- **No soxr**: Termux's libsoxr crashed during sync corrections and at exit,
  so the build leaves it out and the config uses `basic` interpolation.

## Limitations

- Classic AirPlay only (no AirPlay 2: no multi-room, no buffered streaming).
  AirPlay 2 needs Avahi, which Termux doesn't package, and `nqptp` on ports
  319/320.
- On a Wi-Fi network (not the hotspot), Android drops incoming multicast while
  the screen is off, so the iPhone only finds the receiver while the phone's
  screen is on. Over the phone's hotspot this doesn't happen.
- Switching networks or restarting the receiver ends the current AirPlay
  session; pick the receiver again on the iPhone.
