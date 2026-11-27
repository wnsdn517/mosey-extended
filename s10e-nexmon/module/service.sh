#!/system/bin/sh
# mosey-s10e late start (after boot_completed): MoseyApp hardening, a supervised mosey_server
# on the radiotap0 backend (moseynex LD_PRELOAD shim), and the GMS phenotype flag.
MODDIR=${0%/*}
LOGDIR=/data/adb/mosey-extended
LOGFILE=$LOGDIR/service.log
RUNLOG=$LOGDIR/mosey_server.log
PIDFILE=$LOGDIR/mosey_server.pid
mkdir -p $LOGDIR; chmod 0700 $LOGDIR
# disabled by the bootloop guard in post-fs-data.sh during this boot: start nothing
[ -f $MODDIR/disable ] && exit 0
# the boot completed and stayed up: reset the guard's counter
( until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 5; done
  sleep 120; rm -f $LOGDIR/boot_count ) &
# keep logs bounded across boots
for f in $LOGFILE $RUNLOG $LOGDIR/moseynex.log; do
  [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 1048576 ] && mv -f "$f" "$f.old"
done

log() { echo "$(date '+%m-%d %H:%M:%S') [mosey-s10e] $*" >> $LOGFILE; }

# KernelSU moves system/vendor -> vendor, Magisk keeps system/vendor
V=$MODDIR/vendor; [ -d "$V" ] || V=$MODDIR/system/vendor
BIN=$V/bin/mosey_server
SQ=$V/bin/sqlite3_mosey
FFI=$V/lib64/libmosey_daemon_ffi.so
SHIM=$MODDIR/moseynex/libmoseynex.so
WL=$MODDIR/tools/wlmon

log "start (MODDIR=$MODDIR sep=$(getprop ro.build.version.sep))"

i=0
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  i=$((i + 1)); [ $i -gt 300 ] && break; sleep 1
done
log "boot_completed after ${i}s"

# nexmon status
if grep -q " /vendor/firmware/bcmdhd_sta.bin_b1 " /proc/mounts; then
  log "nexmon firmware active: $("$WL" var ver 2>&1 | head -1)"
  RADIO=1
else
  log "WARNING: nexmon firmware not mounted (see /data/local/tmp/mosey-s10e.log) - AirDrop radio unavailable"
  RADIO=0
fi
# mosey_dhd.ko enables fast transfer (data-path injection). It is tried on any kernel; the kernel
# refuses an incompatible module. After a load error, or if the phone went down while the fast path
# was in use (dp_busy left behind), slow mode is pinned for this kernel release. Delete
# $LOGDIR/slow_mode to try again.
DPLINK=/sys/module/mosey_dhd/parameters/link
KO=$MODDIR/kmod/mosey_dhd.ko
SLOW=$LOGDIR/slow_mode
# the previous boot ended in a crash: save a report (with the kernel log) after the first unlock
if [ -f $LOGDIR/dp_busy ] ||
   grep -a -q -F "Kernel panic" /proc/last_kmsg /sys/fs/pstore/*ramoops* 2>/dev/null; then
  log "previous boot ended in a crash, report after unlock"
  ( until [ "$(getprop sys.user.0.ce_available)" = "true" ]; do sleep 10; done
    sleep 30; sh $MODDIR/report.sh "previous boot crashed" >/dev/null ) &
fi
if [ -f $LOGDIR/dp_busy ]; then
  uname -r > $SLOW; rm -f $LOGDIR/dp_busy
  echo off > $LOGDIR/amsdu    # frame bundling stays off until the module is installed again
  log "WARNING: last boot ended while fast transfer was in use - slow mode pinned, A-MSDU off"
fi
if [ "$RADIO" = 1 ] && [ -f "$KO" ] && [ ! -e $DPLINK ] &&
   [ "$(cat $SLOW 2>/dev/null)" != "$(uname -r)" ]; then
  touch $LOGDIR/dp_busy; sync
  if insmod "$KO" 2>>$LOGFILE; then
    log "mosey_dhd loaded"
  else
    uname -r > $SLOW
    log "WARNING: mosey_dhd insmod failed - slow mode pinned"
  fi
  rm -f $LOGDIR/dp_busy
fi

# MoseyApp: must stay enabled and out of app standby; never cleared or reinstalled here
if pm path com.google.android.mosey >/dev/null 2>&1; then
  pm enable com.google.android.mosey >/dev/null 2>&1
  # signature|role on Android 16 (the privapp allowlist cannot grant it); without it
  # NetworkAgent.register() throws and MoseyApp reports INITIALIZATION_FAILED. Root only.
  pm grant com.google.android.mosey android.permission.NETWORK_FACTORY 2>>$LOGFILE ||
    log "WARNING: NETWORK_FACTORY grant failed"
  cmd apphibernation set-state --global com.google.android.mosey false >/dev/null 2>&1
  cmd apphibernation set-state com.google.android.mosey false >/dev/null 2>&1
  am set-standby-bucket com.google.android.mosey active >/dev/null 2>&1
  cmd deviceidle whitelist +com.google.android.mosey >/dev/null 2>&1
  log "MoseyApp $(dumpsys package com.google.android.mosey | grep -m1 versionCode | tr -s ' ')"
else
  log "ERROR: com.google.android.mosey not installed (priv-app overlay failed?)"
fi

# mosey_server supervisor
supervise() {
  fails=0
  while :; do
    if ! ip link show radiotap0 >/dev/null 2>&1; then
      ip link add radiotap0 type dummy && ip link set radiotap0 up
    fi
    t0=$(date +%s)
    LD_PRELOAD=$SHIM MOSEYNEX_IF=wlan0 MOSEYNEX_ALIAS=radiotap0 MOSEYNEX_LOG=$LOGDIR/moseynex.log \
      MOSEYNEX_FFI=$FFI "$BIN" >> $RUNLOG 2>&1 &
    pid=$!
    echo $pid > $PIDFILE
    log "mosey_server started pid=$pid ctx=$(cat /proc/$pid/attr/current 2>/dev/null)"
    wait $pid
    rc=$?
    up=$(( $(date +%s) - t0 ))
    # moseynex disarms itself only while it runs. If it died, monitor mode stays armed and blocks
    # 5 GHz STA, and Wi-Fi scans stay suppressed, so undo both here.
    [ "$RADIO" = 1 ] && "$WL" set 0x613 0 >/dev/null 2>&1 && "$WL" set 116 0 >/dev/null 2>&1
    [ -e $DPLINK ] && echo 0 > $DPLINK
    rm -f $LOGDIR/dp_busy     # only the daemon exited, the phone did not go down
    log "mosey_server exited rc=$rc after ${up}s"
    tail -n 20 $RUNLOG >> $LOGFILE 2>/dev/null
    # back off on crash loops, reset after a healthy run
    [ $up -gt 300 ] && fails=0 || fails=$((fails + 1))
    [ $fails -eq 3 ] && sh $MODDIR/report.sh "daemon exits repeatedly rc=$rc" >/dev/null
    d=$((fails * fails * 5)); [ $d -gt 600 ] && d=600
    sleep $d
  done
}

old=$(cat $PIDFILE 2>/dev/null)
if [ -n "$old" ] && [ "$(cat /proc/$old/comm 2>/dev/null)" = "mosey_server" ]; then
  log "mosey_server already running (pid=$old)"
elif [ -x "$BIN" ]; then
  # start 30 s after the first unlock, when the boot receivers are no longer all starting at once
  ( n=0
    until [ "$(getprop sys.user.0.ce_available)" = "true" ] || [ $n -gt 3600 ]; do n=$((n + 5)); sleep 5; done
    sleep 30
    supervise ) &
else
  log "ERROR: $BIN missing"
fi

# module list status (status.sh)
rm -f $LOGDIR/flag_ok
[ -f $MODDIR/status.sh ] && sh $MODDIR/status.sh &

# GMS phenotype flag (needs the CE-encrypted GMS data: wait for first unlock)
(
  n=0
  until [ -f /data/data/com.google.android.gms/databases/phenotype.db ]; do
    n=$((n + 1)); [ $n -gt 720 ] && { log "phenotype.db never appeared"; exit 0; }; sleep 5
  done
  sleep 60   # let GMS finish its own boot-time phenotype sync first
  while :; do
    sh $MODDIR/mosey_flag.sh "$SQ" $LOGFILE
    rc=$?
    case $rc in
      0) touch $LOGDIR/flag_ok; [ -z "$flag_ok" ] && log "phenotype flag 45704091 active"; flag_ok=1; sleep 21600 ;;  # recheck every 6h
      1) rm -f $LOGDIR/flag_ok; log "phenotype flag applied, rechecking in 2 min"; flag_ok=""; sleep 120 ;;
      *) rm -f $LOGDIR/flag_ok; sleep 300 ;;
    esac
  done
) &

# stale monitor watchdog (monwatch.sh)
[ "$RADIO" = 1 ] && [ -f $MODDIR/monwatch.sh ] && sh $MODDIR/monwatch.sh &

log "service.sh done (radio=$RADIO)"
exit 0
