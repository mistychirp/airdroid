MODDIR=${0%/*}
. "$MODDIR/common.sh"

if is_enabled; then
  rm -f $ENABLED_FILE
  stop_receiver
  update_description "[Off] Action button turns the AirPlay receiver on."
  log "action: turned off"
  echo "AirPlay receiver off."
else
  touch $ENABLED_FILE
  update_description "[On] Starting the AirPlay receiver. Action button turns it off."
  log "action: turned on"
  echo "AirPlay receiver on."
  echo "It starts within a few seconds once Wi-Fi or the hotspot is up."
fi
