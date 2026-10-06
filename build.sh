#!/data/data/com.termux/files/usr/bin/bash
# Builds and installs the AirPlay receiver in Termux: the pthread cancellation
# shim, shairport-sync with the Android patches, and the airplay-* scripts.
set -euo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)
SPS_VERSION=5.5.2
WORK=$HOME/shairport-sync

pkg install -y clang make autoconf automake libtool pkg-config git \
  openssl libconfig libpopt pulseaudio

echo "== cancellation shim"
cd "$SRC/cancel_shim"
clang -O2 -Wall -U_FORTIFY_SOURCE -c cancel_shim.c -o cancel_shim.o
# shellcheck disable=SC2046 # the flags file is a list of separate arguments
clang -O2 -Wall -U_FORTIFY_SOURCE -include cancel_shim.h test_cancel.c cancel_shim.o \
  $(cat cancel_shim.ldflags) -o test_cancel
./test_cancel

echo "== shairport-sync $SPS_VERSION"
rm -rf "$WORK"
git clone -q --depth 1 --branch "$SPS_VERSION" https://github.com/mikebrady/shairport-sync.git "$WORK"
cd "$WORK"
for patch in "$SRC"/patches/*.patch; do
  git apply "$patch"
done
autoreconf -fi
# The shim header is force-included only at build time: configure's own test
# programs redeclare libc functions and would collide with it. No soxr: Termux's
# libsoxr crashes both during playback corrections and at exit.
./configure --prefix="$PREFIX" --sysconfdir="$PREFIX/etc" \
  --with-ssl=openssl --with-tinysvcmdns --with-pulseaudio \
  CFLAGS="-O2 -U_FORTIFY_SOURCE" \
  LDFLAGS="$(cat "$SRC/cancel_shim/cancel_shim.ldflags")" \
  LIBS="$SRC/cancel_shim/cancel_shim.o"
make -j"$(nproc)" CFLAGS="-O2 -U_FORTIFY_SOURCE -include $SRC/cancel_shim/cancel_shim.h"
install -m 755 shairport-sync "$PREFIX/bin/"

echo "== scripts and config"
install -m 755 "$SRC"/termux/airplay-* "$PREFIX/bin/"
mkdir -p "$HOME/.config"
if [ -f "$HOME/.config/shairport-sync.conf" ]; then
  echo "Keeping the existing ~/.config/shairport-sync.conf"
else
  cp "$SRC/termux/shairport-sync.conf" "$HOME/.config/"
fi

shairport-sync -V
echo "Done. Run airplay-start, or install the airplay_receiver module."
