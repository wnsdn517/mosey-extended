#!/system/bin/sh
# post-fs-data: runs in the global mount namespace before servicemanager and Wi-Fi start.
#
# 1. Bind the nexmon-patched BCM4375B1 STA firmware over the stock image (same 18.41.117 build,
#    with monitor/injection patches). Skipped if $MODDIR/no_nexmon exists.
# 2. Mount only what mosey needs, read-only, keeping the original SELinux labels: the VINTF
#    fragment, vendor_service_contexts, and MoseyApp's priv-app and XML config. KernelSU Next runs
#    in "Meta" mount mode without a metamodule, so module partition files are not mounted by it.
#    libmosey_daemon_ffi.so is redirected by the moseynex shim, not overlaid. Skipped if
#    $MODDIR/no_overlay exists.
# 3. Spoof ro.build.version.sep so Samsung Quick Share offers "Share with Apple devices"
#    (FEATURE_PROTOCOL_X needs SEM_PLATFORM_INT >= 170500). Must run before zygote. Value from
#    $MODDIR/sep_value (default 180500); skipped if $MODDIR/no_sep_spoof exists.
MODDIR=${0%/*}
LOG=/data/local/tmp/mosey-s10e.log
NEX=$MODDIR/firmware/bcmdhd_nex.bin_b1
FW=/vendor/firmware/bcmdhd_sta.bin_b1
# nexmon image is built from this exact stock image (BCM4375B1 18.41.117 STA)
STOCK_SHA=1676f46ce56b96f58dc70de08beaab4ab3362ee6dd751465a8d6a0023c3c54ad

[ -f $LOG ] && cp $LOG $LOG.prev
echo "$(date) post-fs-data" > $LOG

# bootloop guard: counted here, cleared by service.sh after a completed boot. On the third start in
# a row without one, the module disables itself and changes nothing.
STATE=/data/adb/mosey-extended
mkdir -p $STATE
n=$(tr -cd 0-9 2>/dev/null < $STATE/boot_count)
n=$(( ${n:-0} + 1 ))
if [ $n -ge 3 ]; then
  touch $MODDIR/disable
  rm -f $STATE/boot_count
  echo "$((n - 1)) boots in a row did not complete: module disabled" | tee $STATE/bootloop >> $LOG
  # the module is now off: report once the phone is up and unlocked
  nohup sh -c "until [ \"\$(getprop sys.user.0.ce_available)\" = true ]; do sleep 10; done
    sleep 30; sh $MODDIR/report.sh bootloop" >/dev/null 2>&1 &
  exit 0
fi
echo $n > $STATE/boot_count
sync

# resetprop (KernelSU / KernelSU Next / APatch / Magisk)
RP=""
for c in /data/adb/ksu/bin/resetprop /data/adb/ap/bin/resetprop /data/adb/magisk/resetprop; do
  [ -x "$c" ] && { RP=$c; break; }
done
[ -z "$RP" ] && command -v resetprop >/dev/null 2>&1 && RP=resetprop

if [ -f "$MODDIR/no_sep_spoof" ]; then
  echo "no_sep_spoof set, sep=$(getprop ro.build.version.sep)" >> $LOG
else
  want=$(cat "$MODDIR/sep_value" 2>/dev/null | tr -cd 0-9); [ -n "$want" ] || want=180500
  cur=$(getprop ro.build.version.sep)
  if [ -z "$cur" ]; then
    echo "ro.build.version.sep missing (not One UI?), not spoofing" >> $LOG
  elif [ "$cur" -ge "$want" ] 2>/dev/null; then
    echo "sep=$cur already >= $want" >> $LOG
  elif [ -n "$RP" ]; then
    getprop ro.build.version.sep > "$MODDIR/sep_orig" 2>/dev/null
    "$RP" ro.build.version.sep "$want" >> $LOG 2>&1
    echo "sep $cur -> $(getprop ro.build.version.sep) via $RP" >> $LOG
  else
    echo "WARNING: no resetprop found, sep stays $cur (no 'Share with Apple devices')" >> $LOG
  fi
fi

# nexmon firmware, only over the exact stock image it was built from
if [ -f "$MODDIR/no_nexmon" ]; then
  echo "no_nexmon set, stock firmware" >> $LOG
elif grep -q " $FW " /proc/mounts; then
  echo "$FW already bind-mounted" >> $LOG
elif [ -f "$NEX" ] && [ -f "$FW" ]; then
  sha=$(sha256sum "$FW" 2>/dev/null | cut -c1-64)
  echo "stock $FW sha256=$sha" >> $LOG
  case "$sha" in
    "$STOCK_SHA")
      chcon u:object_r:vendor_fw_file:s0 "$NEX"
      mount -o bind "$NEX" "$FW" >> $LOG 2>&1 && echo "nexmon firmware bound over $FW" >> $LOG ;;
    *)
      echo "WARNING: $FW is not BCM4375B1 18.41.117 stock; nexmon NOT loaded (AirDrop radio disabled)" >> $LOG
      touch "$MODDIR/.fw_mismatch" ;;
  esac
else
  echo "WARNING: $FW not found (not a BCM4375 Exynos device?); nexmon NOT loaded" >> $LOG
fi

[ -f "$MODDIR/no_overlay" ] && { echo "no_overlay set, skipping overlays" >> $LOG; exit 0; }

label_of() { ls -Zd "$1" 2>/dev/null | awk '{print $1}'; }

# Overlay lower dirs are copied to tmpfs first: an overlayfs whose lowerdir lives on /data keeps
# /data busy at shutdown (init only unmounts block-device mounts), which forces "Shutdown /data"
# -> sudden-power-off -> a long full fsck.f2fs on the next boot.
STAGE=/dev/mosey-s10e
mkdir -p $STAGE && chmod 0755 $STAGE

# ovl <module dir> <target dir>: read-only overlay, module files on top of the original directory
ovl() {
  src=$1; dst=$2
  [ -d "$src" ] && [ -d "$dst" ] || { echo "skip $dst (missing)" >> $LOG; return; }
  tmp=$STAGE$(echo "$dst" | tr / _)
  # already ours? another module's overlay on the same directory must not make us skip it
  grep -q "lowerdir=$tmp:$dst" /proc/mounts && { echo "already $dst" >> $LOG; return; }
  rm -rf "$tmp"; cp -r "$src" "$tmp" || { echo "FAILED copy $src" >> $LOG; return; }
  lbl=$(label_of "$dst")
  [ -n "$lbl" ] && chcon -R "$lbl" "$tmp"
  if mount -t overlay overlay -o "ro,lowerdir=$tmp:$dst" "$dst" >> $LOG 2>&1; then
    echo "overlay $dst ($lbl)" >> $LOG
  else
    echo "FAILED overlay $dst" >> $LOG
  fi
}

# bindf <module file> <existing target file>
bindf() {
  src=$1; dst=$2
  [ -f "$src" ] && [ -f "$dst" ] || { echo "skip $dst (missing)" >> $LOG; return; }
  lbl=$(label_of "$dst")
  [ -n "$lbl" ] && chcon "$lbl" "$src"
  mount -o bind "$src" "$dst" >> $LOG 2>&1 && echo "bind $dst ($lbl)" >> $LOG || echo "FAILED bind $dst" >> $LOG
}

# KernelSU moves system/vendor -> $MODDIR/vendor and system/product -> $MODDIR/product at install
V=$MODDIR/vendor; [ -d "$V" ] || V=$MODDIR/system/vendor
P=$MODDIR/product; [ -d "$P" ] || P=$MODDIR/system/product

ovl "$V/etc/vintf/manifest" /vendor/etc/vintf/manifest
# rebuilt from the running ROM's file on every boot: a ROM update must not get the old copy
VSC=/vendor/etc/selinux/vendor_service_contexts
if [ -f $VSC ] && ! grep -q " $VSC " /proc/mounts; then
  new=$V/etc/selinux/vendor_service_contexts
  mkdir -p "${new%/*}"
  { grep -v moseyservice $VSC
    echo "com.google.pixel.moseyservice.IMoseyService/default u:object_r:mosey_service:s0"
  } > "$new"
  chmod 0644 "$new"
fi
bindf "$V/etc/selinux/vendor_service_contexts" /vendor/etc/selinux/vendor_service_contexts
ovl "$P/priv-app" /product/priv-app
ovl "$P/etc/permissions" /product/etc/permissions
ovl "$P/etc/default-permissions" /product/etc/default-permissions
ovl "$P/etc/sysconfig" /product/etc/sysconfig
