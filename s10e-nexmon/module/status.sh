#!/system/bin/sh
# Shows the current state in the module description (root manager's module list). module.prop is
# rewritten only when the state changes.
MODDIR=${0%/*}
LOGDIR=/data/adb/mosey-extended
BASE="Send files to Apple devices with Quick Share"

state() {
  grep -q " /vendor/firmware/bcmdhd_sta.bin_b1 " /proc/mounts || { echo "error:firmware"; return; }
  [ "$(getprop ro.build.version.sep)" -ge 170500 ] 2>/dev/null || { echo "error:oneui-version"; return; }
  [ "$(getprop sys.user.0.ce_available)" = "true" ] || { echo "waiting:unlock"; return; }
  [ -d /data/data/com.google.android.mosey ] || { echo "error:no-mosey-app"; return; }
  p=$(cat $LOGDIR/mosey_server.pid 2>/dev/null)
  [ -n "$p" ] && [ "$(cat /proc/$p/comm 2>/dev/null)" = "mosey_server" ] || { echo "error:server-not-running"; return; }
  [ -f $LOGDIR/flag_ok ] || { echo "waiting:gms-flag"; return; }
  [ -e /sys/module/mosey_dhd/parameters/link ] || { echo "working:slow"; return; }
  echo "working"
}

bad=0 reported=
while :; do
  s=$(state)
  case $s in error:*|working:slow) bad=$((bad + 1)) ;; *) bad=0 ;; esac
  if [ $bad -ge 4 ] && [ "$s" != "$reported" ] &&
     [ "$(getprop sys.user.0.ce_available)" = "true" ]; then
    sh $MODDIR/report.sh "$s" >/dev/null && reported=$s
  fi
  line="description=[$s] $BASE"
  grep -qxF "$line" $MODDIR/module.prop || sed -i "s|^description=.*|$line|" $MODDIR/module.prop
  # look rarely once the state is settled (battery), every 30 s while it is still changing
  d=30
  { [ "$s" = working ] || [ "$s" = "$reported" ]; } && d=300
  sleep ${INTERVAL:-$d}
done
