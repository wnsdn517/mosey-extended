#!/system/bin/sh
# Monitor mode left armed with nobody capturing breaks normal Wi-Fi: the data-path firmware hook
# drops ordinary host frames, including the EAPOL handshake, and Android then reports a wrong
# password. Disarm when the station is not up, monitor is armed and no root capture socket is open
# on wlan0, twice in a row. Touch /data/adb/mosey-extended/no_monwatch to pause (manual monitor).
MODDIR=${0%/*}
LOGDIR=/data/adb/mosey-extended
WL=$MODDIR/tools/wlmon
DPLINK=/sys/module/mosey_dhd/parameters/link

# /proc/net/packet columns: sk RefCnt Type Proto Iface R Rmem User Inode. mosey_server (and a
# manual tcpdump) capture as root with a non-zero protocol; system sockets have other uids.
capturing() {
  idx=$(cat /sys/class/net/wlan0/ifindex 2>/dev/null)
  while read -r sk ref type proto iface r rmem uid inode; do
    [ "$iface" = "$idx" ] && [ "$uid" = 0 ] && [ "$proto" != 0000 ] && return 0
  done < /proc/net/packet
  return 1
}

# Battery: the firmware is asked only while Wi-Fi is on but unconnected ("dormant", where the
# problem shows), less often while nothing is wrong. sleep does not wake a suspended phone.
stale=0 d=15
while :; do
  sleep ${INTERVAL:-$d}
  op=
  read -r op 2>/dev/null < /sys/class/net/wlan0/operstate
  if [ "$op" != dormant ] || [ -f $LOGDIR/no_monwatch ]; then stale=0 d=60; continue; fi
  v=$("$WL" get 107 2>/dev/null) || { stale=0 d=60; continue; }
  if [ $((v & 255)) -eq 0 ] || capturing; then
    stale=0 d=$((d * 2))
    [ $d -gt 60 ] && d=60
    continue
  fi
  stale=$((stale + 1)) d=15
  [ $stale -lt 2 ] && continue
  "$WL" set 0x613 0 >/dev/null 2>&1
  [ -e $DPLINK ] && echo 0 > $DPLINK
  echo "$(date '+%m-%d %H:%M:%S') [mosey-s10e] monwatch: disarmed stale monitor (was $((v & 255)), now $("$WL" get 107))" >> $LOGDIR/service.log
  sh $MODDIR/report.sh "stale monitor mode disarmed" >/dev/null
  stale=0
done
