ui_print "- AirPlay Receiver"

# First install: on by default. Updates keep the current on/off choice.
if [ ! -d /data/adb/airplay_receiver ]; then
  mkdir -p /data/adb/airplay_receiver
  touch /data/adb/airplay_receiver/enabled
fi

if [ ! -x /data/data/com.termux/files/usr/bin/airplay-run ]; then
  ui_print "! The AirPlay build isn't installed in Termux yet."
  ui_print "! The receiver starts once it is."
fi
ui_print "- The Action button turns the receiver on and off."
ui_print "- Termux is made battery-unrestricted so it can run in the background."
