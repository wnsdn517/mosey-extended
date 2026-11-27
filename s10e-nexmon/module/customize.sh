# KernelSU / APatch / Magisk installer hook (files are already extracted to $MODPATH).
# Every check runs before anything on the device or in $MODPATH is changed.
# The nexmon image is built from exactly this stock firmware (BCM4375B1 18.41.117 STA).
STOCK_SHA=1676f46ce56b96f58dc70de08beaab4ab3362ee6dd751465a8d6a0023c3c54ad
FW=/vendor/firmware/bcmdhd_sta.bin_b1
OLD=/data/adb/modules/mosey-s10e

# abort, after saving a diagnostic report
fail() {
  r=$(sh $MODPATH/report.sh "install: $1" 2>/dev/null) && ui_print "Report saved: $r"
  abort "$1"
}

ui_print "[1/4] Checking device"
[ "$ARCH" = "arm64" ] || fail "Abort: arm64 only"
case "$(getprop ro.product.manufacturer)" in
  samsung|Samsung) ;;
  *) fail "Abort: Samsung Galaxy only" ;;
esac
soc="$(getprop ro.hardware.chipname) $(getprop ro.product.board) $(getprop ro.board.platform)"
case "$soc" in
  *9820*) ;;
  *) fail "Abort: Exynos 9820 (Galaxy S10 series) only, found: $soc" ;;
esac
[ "$API" -ge 36 ] || fail "Abort: Android 16+ required (API $API)"
[ "$API" -gt 36 ] && ui_print "  Warning: tested on Android 16 only (API $API)"
[ -n "$(getprop ro.build.version.sep)" ] || fail "Abort: One UI required"
pm path com.samsung.android.app.sharelive >/dev/null 2>&1 || fail "Abort: Samsung Quick Share not installed"
# "Share with Apple devices" is only in newer builds (13.9.10.5 has it, 13.8.00.41 does not)
qs=$(pm path com.samsung.android.app.sharelive | sed -n 's/^package://p' | grep -E 'base[.]apk|ShareLive[.]apk' | head -n 1)
unzip -p "$qs" 'classes*.dex' 2>/dev/null | grep -a -q protocol_x_supported ||
  fail "Abort: this Quick Share has no Apple sharing. Update Quick Share (tested: 13.9.10.5)"
pm path com.google.android.gms >/dev/null 2>&1 || fail "Abort: Google Play services not installed"
[ "$KSU" = "true" ] || ui_print "  Warning: tested on KernelSU Next only"

ui_print "[2/4] Verifying files"
[ -f $MODPATH/sha256sums ] || fail "Abort: sha256sums missing"
while read -r sum path; do
  f=$MODPATH/$path
  [ -f "$f" ] || f=$MODPATH/${path#system/}     # some managers move system/<partition> up
  [ -f "$f" ] || fail "Abort: missing file $path"
  [ "$(sha256sum "$f" | cut -c1-64)" = "$sum" ] || fail "Abort: corrupted file $path (download again)"
done < $MODPATH/sha256sums

ui_print "[3/4] Checking conflicts and Wi-Fi firmware"
for m in /data/adb/modules/*; do
  [ -d "$m" ] && [ "$m" != "$OLD" ] && [ ! -f "$m/disable" ] && [ ! -f "$m/remove" ] || continue
  for f in vendor/bin/mosey_server system/vendor/bin/mosey_server \
           vendor/firmware/bcmdhd_sta.bin_b1 system/vendor/firmware/bcmdhd_sta.bin_b1 \
           firmware/bcmdhd_nex.bin_b1; do
    [ -e "$m/$f" ] && fail "Abort: conflicts with module '${m##*/}'. Disable it and reboot first"
  done
  [ "${m##*/}" = "mosey-extended" ] && fail "Abort: disable the mosey-extended module and reboot first"
done
[ -f "$FW" ] || fail "Abort: $FW not found (not a BCM4375B1 device)"
sha=$(sha256sum "$FW" | cut -c1-64)
if grep -q " $FW " /proc/mounts; then
  # replaced already: only acceptable if it is the image of the installed version of this module
  [ -f $OLD/firmware/bcmdhd_nex.bin_b1 ] &&
    [ "$sha" = "$(sha256sum $OLD/firmware/bcmdhd_nex.bin_b1 | cut -c1-64)" ] ||
    fail "Abort: Wi-Fi firmware was replaced by something else. Reboot and retry"
  ui_print "  Updating installed version"
else
  [ "$sha" = "$STOCK_SHA" ] ||
    fail "Abort: unsupported Wi-Fi firmware ($sha), stock BCM4375B1 18.41.117 required"
  ui_print "  Wi-Fi firmware OK"
fi
ui_print "[4/4] Installing"
rm -f /data/adb/mosey-extended/boot_count /data/adb/mosey-extended/bootloop
# try fast mode and frame bundling again
rm -f /data/adb/mosey-extended/slow_mode /data/adb/mosey-extended/dp_busy /data/adb/mosey-extended/amsdu
# SELinux: mosey_server runs in the root manager's own domain (ksu / magisk / ...)
dom=$(cut -d: -f3 /proc/self/attr/current 2>/dev/null)
[ -n "$dom" ] || dom=ksu
sed -i "s/@ROOTDOM@/$dom/g" $MODPATH/sepolicy.rule

# service_contexts: extend THIS device's file by one line (never ship the Pixel copy)
VSC=/vendor/etc/selinux/vendor_service_contexts
for base in $MODPATH/system/vendor $MODPATH/vendor; do
  [ -d "$base" ] || continue
  OUT=$base/etc/selinux/vendor_service_contexts
  mkdir -p "${OUT%/*}"
  if [ -f "$VSC" ]; then grep -v "moseyservice" "$VSC" > "$OUT"; else : > "$OUT"; fi
  echo "com.google.pixel.moseyservice.IMoseyService/default u:object_r:mosey_service:s0" >> "$OUT"
done

# post-fs-data.sh mounts exactly what is needed (read-only, original labels); keep the root
# manager from magic-/overlay-mounting the module's system/ tree on top.
touch $MODPATH/skip_mount

set_perm_recursive $MODPATH 0 0 0755 0644
for base in $MODPATH/system/vendor $MODPATH/vendor; do
  [ -d "$base" ] || continue
  set_perm $base/bin/mosey_server 0 2000 0755 u:object_r:vendor_file:s0
  set_perm $base/bin/sqlite3_mosey 0 2000 0755 u:object_r:vendor_file:s0
  set_perm $base/lib64/libmosey_daemon_ffi.so 0 0 0644 u:object_r:vendor_file:s0
  set_perm $base/etc/vintf/manifest/manifest_mosey.xml 0 0 0644 u:object_r:vendor_configs_file:s0
  set_perm $base/etc/selinux/vendor_service_contexts 0 0 0644 u:object_r:vendor_service_contexts_file:s0
done
set_perm $MODPATH/tools/wlmon 0 0 0755
set_perm $MODPATH/firmware/bcmdhd_nex.bin_b1 0 0 0644 u:object_r:vendor_fw_file:s0
for f in service.sh post-fs-data.sh mosey_flag.sh monwatch.sh status.sh report.sh action.sh uninstall.sh; do
  set_perm $MODPATH/$f 0 0 0755
done

ui_print "Done. Reboot, then enable 'Share with Apple devices' in Quick Share."
