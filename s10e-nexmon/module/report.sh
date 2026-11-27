#!/system/bin/sh
# report.sh [reason]: collects diagnostics into one file in Download. Runs automatically when the
# installer aborts or something fails, and from the module's Action button. The file stays on the
# phone; nothing is sent anywhere. MAC and local IP addresses, Wi-Fi names, device names and mail
# addresses are masked, but check the file before sharing it.
MODDIR=${0%/*}
LOGDIR=/data/adb/mosey-extended
OUT=${OUT:-/sdcard/Download/mosey-report.txt}
FW=/vendor/firmware/bcmdhd_sta.bin_b1
LASTK=${LASTK:-/proc/last_kmsg}      # Samsung: kernel log of the previous boot

mask() {
  sed -E \
    -e 's/(androidboot\.(serialno|ap_serial|em\.did)|ap_serial)([=: ]+)[^ ]+/\1\3***/g' \
    -e 's/(^|[^0-9])[0-9]{15}([^0-9]|$)/\1***************\2/g' \
    -e 's/([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g' \
    -e 's/(^|[^0-9.])(192\.168|10\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01])|100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7]))\.[0-9]{1,3}\.[0-9]{1,3}/\1x.x.x.x/g' \
    -e 's/fe80::[0-9a-fA-F:]+/fe80::x/g' \
    -e 's/(ssid|SSID)([=: ]+)"[^"]*"/\1\2"***"/g' \
    -e "s/(ssid|SSID)=('[^']*'|[^,\") ]+)/\1=***/g" \
    -e 's/([dD]eviceName)=[^,)]*/\1=***/g' \
    -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z]{2,}/***@***/g'
}
sec() { echo; echo "== $*"; }
tailf() { [ -f "$1" ] && { sec "$1"; tail -n ${2:-150} "$1"; }; }

# what is needed to rebuild the kernel module for another kernel, or to see why it was refused
kernel_env() {
  sec kernel
  cat /proc/version
  echo "module: $(strings $MODDIR/kmod/mosey_dhd.ko 2>/dev/null | grep -m1 '^vermagic=')"
  echo "boot reason: $(getprop sys.boot.reason) $(cat /proc/reset_reason 2>/dev/null)"
  echo "symbols used by the module (names only):"
  grep -wE "module_layout|kallsyms_lookup_name|dhd_update_interface_link_status|g_dhd_pub" \
    /proc/kallsyms 2>/dev/null | cut -d' ' -f2-
  lsmod 2>/dev/null
}

# kernel crash records of the previous boot (panic, oops); pmsg (app logs) is left out
crash_dump() {
  sec "crash summary"
  cat /proc/auto_comment 2>/dev/null | tr -d '\000' | head -n 60
  if [ -r $LASTK ]; then
    sec "previous boot: panic lines"
    grep -a -F -A30 -e "Kernel panic" -e "Unable to handle" -e "Internal error: Oops" $LASTK |
      head -n 160
    sec "previous boot: mosey and Wi-Fi driver"
    grep -a -F -e mosey -e dhd_ -e "version of symbol" -e "version magic" $LASTK | tail -n 60
    sec "previous boot: last lines"
    tail -n 150 $LASTK
  fi
  for f in /sys/fs/pstore/*ramoops*; do
    case $f in *pmsg*) ;; *) tailf "$f" 200 ;; esac
  done
}

collect() {
  echo "mosey-s10e report: $* ($(date '+%Y-%m-%d %H:%M:%S'))"
  sec device
  for p in ro.product.model ro.product.device ro.hardware.chipname ro.build.version.sdk \
           ro.build.version.sep ro.build.display.id; do echo "$p=$(getprop $p)"; done
  uname -r
  echo "root: ksu=$(/data/adb/ksud -V 2>/dev/null)"
  kernel_env
  sec firmware
  sha256sum $FW 2>&1
  grep " $FW " /proc/mounts | cut -c1-120
  sec apps
  for p in com.samsung.android.app.sharelive com.google.android.mosey com.google.android.gms; do
    echo "$p $(dumpsys package $p 2>/dev/null | grep -m2 versionName | tr -s ' ' | tr '\n' ' ')"
  done
  sec modules
  for m in /data/adb/modules/*; do
    echo "${m##*/} $([ -f $m/disable ] && echo disabled) $([ -f $m/update ] && echo update-pending)"
  done
  grep -E "^(version|description)=" /data/adb/modules/mosey-s10e/module.prop 2>/dev/null
  sec state
  ls $LOGDIR 2>/dev/null | tr '\n' ' '; echo
  echo "slow_mode=$(cat $LOGDIR/slow_mode 2>/dev/null) bootloop=$(cat $LOGDIR/bootloop 2>/dev/null)"
  lsmod 2>/dev/null | grep mosey
  ps -A -o PID,ARGS 2>/dev/null | grep -E "[m]osey|[m]onwatch|[s]tatus.sh"
  grep -E "mosey|priv-app" /proc/mounts | cut -c1-160
  tailf /data/local/tmp/mosey-s10e.log
  tailf /data/local/tmp/mosey-s10e.log.prev
  tailf $LOGDIR/service.log
  tailf $LOGDIR/moseynex.log
  tailf $LOGDIR/mosey_server.log 60
  sec dmesg
  dmesg 2>/dev/null | grep -iE "mosey|dhd_|bcmdhd|firmware|version of symbol|version magic|avc: .*(mosey|ksu)|Kernel panic|Unable to handle" | tail -n 150
  sec logcat
  logcat -d -b main -b system -b crash -v time 2>/dev/null |
    grep -F -e Mosey.A -e mosey -e AndroidRuntime -e FATAL -e ExternalSharing \
      -e "External sharing provider" |    # fixed strings: a regex takes half a minute here
    tail -n 250
  case "$*" in
    *bootloop*)
      sec "system crashes"
      dumpsys dropbox --print system_server_crash 2>/dev/null | tail -n 80 ;;
  esac
  crash_dump
  sec "kernel config"
  zcat /proc/config.gz 2>/dev/null | grep -v "^#" | grep .
}

mkdir -p "${OUT%/*}" 2>/dev/null
collect "$*" 2>&1 | mask > "$OUT" 2>/dev/null || exit 1
chmod 0660 "$OUT" 2>/dev/null
# make it show up in the Files app
case $OUT in /sdcard/*)
  content call --uri content://media/ --method scan_file \
    --arg "/storage/emulated/0/${OUT#/sdcard/}" >/dev/null 2>&1 ;;
esac
echo "$OUT"
