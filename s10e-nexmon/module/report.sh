#!/system/bin/sh
# report.sh [reason]: collects diagnostics into one archive in Download: report.txt and the
# phone's Wi-Fi firmware (needed to port to another model or firmware). Runs automatically when the
# installer aborts or something fails, and from the module's Action button. The file stays on the
# phone; nothing is sent anywhere. In report.txt MAC and local IP addresses, Wi-Fi names, device
# names and mail addresses are masked, but check it before sharing.
MODDIR=${0%/*}
LOGDIR=/data/adb/mosey-extended
OUT=${OUT:-/sdcard/Download/mosey-report.tar.gz}
TMP=${RTMP:-/data/local/tmp}/mosey-report.$$
INSTALLED=/data/adb/modules/mosey-s10e
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
    -e 's/(connected to )"[^"]*"/\1"***"/g' \
    -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z]{2,}/***@***/g'
}
sec() { echo; echo "== $*"; }
tailf() { [ -f "$1" ] && { sec "$1"; tail -n ${2:-150} "$1"; }; }

# Wi-Fi firmware images (the location differs between models and ROMs)
fw_files() { ls /vendor/firmware/bcmdhd*.bin* /vendor/etc/wifi/bcmdhd*.bin* 2>/dev/null; }
sha_of() { sha256sum "$1" 2>/dev/null | cut -c1-64; }

wifi_info() {
  sec firmware
  for f in $(fw_files); do
    echo "$(sha_of $f) $(wc -c < $f) $f"
    tail -c 256 $f | tr -c '[:print:]' '\n' | grep -a -F Version
  done
  grep -E "bcmdhd|/vendor/firmware" /proc/mounts | cut -c1-120
  sec "wifi driver"
  for p in /sys/module/dhd*/parameters/firmware_path /sys/module/dhd*/parameters/nvram_path \
           /sys/module/firmware_class/parameters/path; do
    [ -r $p ] && echo "$p=$(cat $p)"
  done
  # chip firmware actually running
  [ -x $INSTALLED/tools/wlmon ] && $INSTALLED/tools/wlmon var ver 2>&1 | head -n 3
  ls -l /vendor/etc/wifi 2>/dev/null
}

# the radio right now (meaningful while a share sheet is open)
radio_now() {
  sec "wifi state"
  echo "operstate: $(cat /sys/class/net/wlan0/operstate 2>&1)"
  # picked lines only: the full output repeats the network name in places the mask does not know
  cmd wifi status 2>&1 | grep -o -E "^Wifi .*|Supplicant state: [A-Z_]+|Frequency: [0-9]+MHz"
  echo "location_mode: $(settings get secure location_mode 2>&1)"
  WL=$INSTALLED/tools/wlmon
  [ -x $WL ] || return
  echo "monitor (ioctl 107): $($WL get 107 2>&1)"
  echo "scan suppress (ioctl 116): $($WL get 116 2>&1)"
  a=$(cat /sys/class/net/wlan0/statistics/rx_packets 2>/dev/null)
  sleep 2
  b=$(cat /sys/class/net/wlan0/statistics/rx_packets 2>/dev/null)
  echo "rx_packets: $a, 2 s later: $b"
}

# MoseyApp and anything mosey the ROM ships itself: a ROM copy changes which permission list
# applies, and without its permissions the app cannot take the phone off Wi-Fi
mosey_pkg() {
  sec "mosey files in the ROM"
  for d in /system /system_ext /product /vendor; do
    ls -d $d/priv-app/*[Mm]osey* $d/app/*[Mm]osey* $d/bin/*mosey* $d/etc/init/*mosey* \
      $d/etc/permissions/*mosey* $d/etc/default-permissions/*mosey* $d/etc/sysconfig/*mosey* 2>/dev/null
  done
  for f in /system/etc/permissions/*.xml /system_ext/etc/permissions/*.xml /product/etc/permissions/*.xml; do
    grep -q com.google.android.mosey "$f" 2>/dev/null && { sec "$f"; sed -n '/<!--/,/-->/!p' "$f" |
      sed -n '/com.google.android.mosey/,/<\/privapp-permissions>/p' | head -n 40; }
  done
  sec "mosey package"
  pm path com.google.android.mosey 2>&1
  dumpsys package com.google.android.mosey 2>/dev/null |
    sed -n '/^Packages:/,/^Queries:/p;/^Hidden system packages:/,/^Queries:/p' | head -n 250
}

# the GMS flag that lets Quick Share use mosey, as stored and as GMS publishes it
flag_state() {
  sec "gms flag 45704091"
  G=/data/data/com.google.android.gms
  pb=$G/files/phenotype/shared/com.google.android.gms.nearby.pb
  echo "published: $([ -f $pb ] && grep -q 45704091 $pb && echo yes || echo no)"
  SQ=$MODDIR/vendor/bin/sqlite3_mosey
  [ -x $SQ ] || SQ=$INSTALLED/vendor/bin/sqlite3_mosey
  [ -x $SQ ] && $SQ -readonly -cmd ".timeout 5000" $G/databases/phenotype.db \
    "select 'override', config_package_id, account_id, value, type from flag_overrides where name='45704091';" 2>&1
}

# adds the firmware images to the archive; this module's own patched image is left out
copy_fw() {
  own="$(sha_of $INSTALLED/firmware/bcmdhd_nex.bin_b1) $(sha_of $MODDIR/firmware/bcmdhd_nex.bin_b1)"
  for f in $(fw_files); do
    s=$(sha_of $f)
    case " $own " in *" $s "*) echo "not included (patched by this module): $f"; continue ;; esac
    cp $f "$TMP/mosey-report/$(echo ${f#/} | tr / _)" && echo "included: $f" || echo "copy failed: $f"
  done
}

# what is needed to port to another kernel, or to see why something was refused
kernel_env() {
  sec kernel
  cat /proc/version
  echo "boot reason: $(getprop sys.boot.reason) $(cat /proc/reset_reason 2>/dev/null)"
  echo "selinux: $(getenforce 2>/dev/null)"
  echo "cpu: $(grep -m1 Features /proc/cpuinfo 2>/dev/null)"
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
  for p in ro.product.model ro.product.device ro.hardware.chipname ro.board.platform \
           ro.build.version.sdk ro.build.version.sep ro.build.version.oneui ro.build.display.id; do
    echo "$p=$(getprop $p)"
  done
  uname -r
  echo "root: ksu=$(/data/adb/ksud -V 2>/dev/null)"
  kernel_env
  radio_now      # first: the rest takes long enough for a session to end
  wifi_info
  mosey_pkg
  flag_state
  sec apps
  for p in com.samsung.android.app.sharelive com.google.android.mosey com.google.android.gms; do
    echo "$p $(dumpsys package $p 2>/dev/null | grep -m2 versionName | tr -s ' ' | tr '\n' ' ')"
  done
  sec modules
  for m in /data/adb/modules/*; do
    echo "${m##*/} $([ -f $m/disable ] && echo disabled) $([ -f $m/update ] && echo update-pending)"
  done
  grep -E "^(version|description)=" $INSTALLED/module.prop 2>/dev/null
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
  # lines repeated many times a second are dropped first, or they push everything else out
  logcat -d -b main -b system -b crash -v time 2>/dev/null |
    grep -F -e Mosey.A -e mosey -e AndroidRuntime -e FATAL -e ShareLive -e NearbySharing \
      -e ExternalSharing -e "External sharing provider" |  # fixed strings: a regex takes half a minute here
    grep -v -F -e "without RSSI" -e "A-MSDU subframe" -e "Transceiver task channel is full" \
      -e StackLog -e I/AndroidRuntime -e D/AndroidRuntime |
    tail -n 2000
  case "$*" in
    *bootloop*)
      sec "system crashes"
      dumpsys dropbox --print system_server_crash 2>/dev/null | tail -n 80 ;;
  esac
  crash_dump
  sec "kernel config"
  zcat /proc/config.gz 2>/dev/null | grep -v "^#" | grep .
}

# a report that starts by itself runs behind everything else (less heat and stutter, takes longer)
if [ "$*" != manual ]; then
  renice -n 19 -p $$ >/dev/null 2>&1
  ionice -c 3 -p $$ >/dev/null 2>&1
fi

mkdir -p "${OUT%/*}" 2>/dev/null
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/mosey-report" || exit 1
collect "$*" 2>&1 | mask > "$TMP/mosey-report/report.txt" 2>/dev/null || exit 1
{ sec "files in this archive"; copy_fw; } >> "$TMP/mosey-report/report.txt" 2>&1
tar -czf "$OUT" -C "$TMP" mosey-report || exit 1
chmod 0660 "$OUT" 2>/dev/null
# make it show up in the Files app
case $OUT in /sdcard/*)
  content call --uri content://media/ --method scan_file \
    --arg "/storage/emulated/0/${OUT#/sdcard/}" >/dev/null 2>&1 ;;
esac
echo "$OUT"
