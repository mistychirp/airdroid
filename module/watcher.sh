MODDIR=${0%/*}
. "$MODDIR/common.sh"

# Keeps the receiver running and in step with the phone:
#  - restarts it when the network changes (its mDNS responder reads the
#    interface addresses only once, at startup),
#  - moves playback to a fresh output when the DAC is plugged in or out,
#  - restarts it when it dies, backing off if it keeps dying,
#  - restarts it when PulseAudio dies or hangs (shairport-sync keeps running
#    then, but nothing plays),
#  - stops it when there's no network or the module is turned off.

POLL=3
HEALTH_EVERY=10 # polls between asking Termux to check PulseAudio (~30 s)
HUNG_AFTER=60   # seconds without an answer before PulseAudio counts as hung

until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
until termux_ready; do sleep 10; done
ensure_termux_setup
log "watcher started"

state=
set_state() {
  [ "$state" = "$1" ] && return
  state=$1
  case $1 in
    on) update_description "[On] AirPlay receiver is running. Action button turns it off." ;;
    waiting) update_description "[On] Waiting for Wi-Fi or the hotspot. Action button turns it off." ;;
    off) update_description "[Off] Action button turns the AirPlay receiver on." ;;
  esac
}

restart() {
  stop_receiver
  start_receiver
  started_at=$now
  next_start=$((now + 10))
}

last_net=
last_audio=
pending_net=
pending_audio=
next_start=0
started_at=0
quick_deaths=0
boosted=
polls=0

# Adopt a receiver that's already running (the watcher itself was restarted)
# instead of restarting it for a "network change" from nothing.
if [ -n "$(receiver_pid)" ]; then
  last_net=$(net_sig)
  last_audio=$(audio_sig)
  started_at=$(date +%s)
  log "adopted the running receiver"
fi

while true; do
  now=$(date +%s)
  pid=$(receiver_pid)

  if ! is_enabled; then
    if [ -n "$pid" ]; then
      log "turned off: stopping"
      stop_receiver
    fi
    set_state off

  else
    net=$(net_sig)
    audio=$(audio_sig)

    if [ -z "$net" ]; then
      if [ -n "$pid" ]; then
        log "no network: stopping"
        stop_receiver
      fi
      last_net=
      set_state waiting

    elif [ -z "$pid" ]; then
      # Starting takes a few seconds; next_start keeps it from starting twice.
      if [ "$now" -ge "$next_start" ]; then
        if [ "$started_at" -gt 0 ] && [ $((now - started_at)) -lt 60 ]; then
          quick_deaths=$((quick_deaths + 1))
        else
          quick_deaths=0
        fi
        log "starting receiver (network: $net)"
        start_receiver
        started_at=$now
        last_net=$net
        last_audio=$audio
        pending_net=
        pending_audio=
        case $quick_deaths in
          0 | 1 | 2) next_start=$((now + 10)) ;;
          3 | 4) next_start=$((now + 60)) ;;
          *) next_start=$((now + 300)) ;;
        esac
      fi

    else
      set_state on
      polls=$((polls + 1))
      [ $((polls % HEALTH_EVERY)) -eq 0 ] && run_in_termux airplay-check
      answered=$(last_health)
      [ "$answered" -lt "$started_at" ] && answered=$started_at

      if [ -z "$(pidof pulseaudio)" ] && [ $((now - started_at)) -ge 10 ]; then
        log "PulseAudio died: restarting"
        restart
      elif [ $((now - answered)) -ge $HUNG_AFTER ]; then
        save_pulseaudio_threads
        log "PulseAudio stopped answering (threads in pulseaudio-hang.txt): restarting"
        restart
      # Act on a change once it has held for a poll, so a hotspot that's still
      # coming up doesn't cause two restarts.
      elif [ "$net" != "$last_net" ]; then
        if [ "$net" = "$pending_net" ]; then
          log "network changed ($last_net-> $net): restarting"
          restart
          last_net=$net
          last_audio=$audio
          pending_net=
        else
          pending_net=$net
        fi
      elif [ "$audio" != "$last_audio" ]; then
        if [ "$audio" = "$pending_audio" ]; then
          # Runs in Termux and restarts the receiver itself if it fails.
          log "sound cards changed ($last_audio-> $audio): switching output"
          run_in_termux airplay-switch-output
          last_audio=$audio
          pending_audio=
        else
          pending_audio=$audio
        fi
      fi

      kept=
      for p in $boosted; do
        [ -d /proc/$p ] && kept="$kept $p"
      done
      boosted=$kept
      for p in $(pidof shairport-sync) $(pidof pulseaudio); do
        case " $boosted " in
          *" $p "*) ;;
          *)
            boost_pid "$p"
            boosted="$boosted $p"
            ;;
        esac
      done
    fi
  fi

  sleep $POLL
done
