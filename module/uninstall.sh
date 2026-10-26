pkill -x shairport-sync

# This runs early in boot, before the system services are up. Undo the battery
# exemption (only if this module added it) once they are.
(
  until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
  if [ -f /data/adb/airplay_receiver/added_battery_exemption ]; then
    dumpsys deviceidle whitelist -com.termux > /dev/null
  fi
  rm -rf /data/adb/airplay_receiver
) &
