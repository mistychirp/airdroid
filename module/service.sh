MODDIR=${0%/*}

# The watcher runs for as long as the phone is on; it waits for boot and for
# the first unlock by itself.
sh "$MODDIR/watcher.sh" > /dev/null 2>&1 &
