# Shared helpers, sourced by the module scripts. Expects MODDIR to be set.

DATA=/data/adb/airplay_receiver
ENABLED_FILE=$DATA/enabled
BATTERY_FLAG=$DATA/added_battery_exemption
LOG=$DATA/watcher.log

TERMUX_PKG=com.termux
TERMUX=/data/data/com.termux/files
PREFIX=$TERMUX/usr

mkdir -p $DATA

log() {
  echo "$(date '+%F %T') $*" >> $LOG
  if [ "$(wc -l < $LOG)" -gt 500 ]; then
    tail -n 250 $LOG > $LOG.tmp && mv $LOG.tmp $LOG
  fi
}

is_enabled() {
  [ -f $ENABLED_FILE ]
}

# Termux's files are in credential-encrypted storage, so they only become
# readable after the first unlock.
termux_ready() {
  [ -x $PREFIX/bin/airplay-run ] && [ -x $PREFIX/bin/shairport-sync ]
}

termux_uid() {
  stat -c %u $TERMUX
}

update_description() {
  sed -i "s|^description=.*|description=$1|" "$MODDIR/module.prop"
}

# Interfaces with an IPv4 address, e.g. "wlan0=192.168.1.152/24".
net_sig() {
  ip -4 -o addr show 2>/dev/null | awk '$2 != "lo" {print $2 "=" $4}' | sort | tr '\n' ' '
}

# Sound cards: card0 is the phone's own, a USB DAC shows up as card1.
audio_sig() {
  ls -d /proc/asound/card[0-9]* 2>/dev/null | tr '\n' ' '
}

receiver_pid() {
  pidof shairport-sync
}

# RUN_COMMAND runs airplay-run inside the Termux app, where Android's audio
# and DNS work and files get the right SELinux label. As a background task it
# keeps Termux's foreground service up while the receiver runs.
start_receiver() {
  am start-foreground-service --user 0 -n $TERMUX_PKG/com.termux.app.RunCommandService \
    -a com.termux.RUN_COMMAND \
    --es com.termux.RUN_COMMAND_PATH $PREFIX/bin/airplay-run \
    --ez com.termux.RUN_COMMAND_BACKGROUND true > /dev/null 2>&1
}

stop_receiver() {
  pkill -x shairport-sync
  pkill -x pulseaudio
  i=0
  while { pidof shairport-sync || pidof pulseaudio; } > /dev/null && [ $i -lt 10 ]; do
    sleep 0.5
    i=$((i + 1))
  done
  pkill -9 -x shairport-sync
  pkill -9 -x pulseaudio
  return 0
}

# Runs one of the Termux scripts as the Termux user but outside the app,
# which is enough to talk to PulseAudio.
as_termux() {
  su "$(termux_uid)" -c "env HOME=$TERMUX/home PREFIX=$PREFIX TMPDIR=$PREFIX/tmp \
    PATH=$PREFIX/bin LANG=en_US.UTF-8 $PREFIX/bin/bash $PREFIX/bin/$1"
}

# Android throttles Termux's processes when it isn't on screen, while the
# receiver has to refill the audio buffer every few milliseconds. Give a
# process foreground CPUs, a higher priority (new threads inherit it) and
# protection from the low-memory killer. renice is relative: once per process.
boost_pid() {
  echo "$1" > /dev/cpuset/top-app/cgroup.procs 2> /dev/null
  echo -800 > "/proc/$1/oom_score_adj" 2> /dev/null
  for task in /proc/"$1"/task/*; do
    renice -n -10 -p "${task##*/}" > /dev/null 2>&1
  done
}

# RUN_COMMAND only works with allow-external-apps, and Android won't let
# Termux start its foreground service from the background unless Termux is
# exempt from battery optimization.
ensure_termux_setup() {
  dir=$TERMUX/home/.termux
  props=$dir/termux.properties
  if ! grep -q '^allow-external-apps *= *true' $props 2> /dev/null; then
    mkdir -p $dir
    echo 'allow-external-apps = true' >> $props
    uid=$(termux_uid)
    chown -R "$uid:$uid" $dir
    chcon -R "$(ls -Zd $TERMUX | cut -d' ' -f1)" $dir
    log "enabled allow-external-apps in Termux"
  fi
  if ! dumpsys deviceidle whitelist | grep -q ",$TERMUX_PKG,"; then
    dumpsys deviceidle whitelist +$TERMUX_PKG > /dev/null && touch $BATTERY_FLAG
    log "made Termux battery-unrestricted"
  fi
}
