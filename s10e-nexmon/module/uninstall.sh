#!/system/bin/sh
# Undoes what the module changed outside its own directory. The root manager runs this while the
# module is being removed, usually early in boot, so the work waits in the background for the first
# unlock (Google Play services' data is encrypted until then).
#   MOSEY_DB=<copy of phenotype.db> sh uninstall.sh db    removes the flag from that file only
MODDIR=${0%/*}
FLAG=45704091
PKG=com.google.android.gms.nearby
G=/data/data/com.google.android.gms
WORK=/data/local/tmp/mosey-uninstall

# remove_flag <sqlite3> <db>
remove_flag() {
  "$1" -batch "$2" "PRAGMA foreign_keys=ON;
CREATE TEMP TABLE gone AS SELECT o.override_id FROM flag_overrides o
  JOIN config_packages c USING(config_package_id) WHERE c.name='$PKG' AND o.name='$FLAG' AND o.account_id=0;
DELETE FROM experiment_states_to_overrides WHERE override_id IN (SELECT override_id FROM gone);
DELETE FROM flag_overrides_to_commit WHERE override_id IN (SELECT override_id FROM gone);
DELETE FROM flag_overrides WHERE override_id IN (SELECT override_id FROM gone);"
}

SQ=$MODDIR/vendor/bin/sqlite3_mosey; [ -f "$SQ" ] || SQ=$MODDIR/system/vendor/bin/sqlite3_mosey
if [ "$1" = db ]; then remove_flag "$SQ" "$MOSEY_DB"; exit; fi
if [ "$1" != run ]; then
  # the module directory is deleted right after this script: keep copies
  mkdir -p $WORK && cp "$0" $WORK/uninstall.sh && cp "$SQ" $WORK/sqlite3 && chmod 755 $WORK/sqlite3
  nohup setsid sh $WORK/uninstall.sh run >/dev/null 2>&1 </dev/null &
  exit 0
fi

n=0
until [ "$(getprop sys.boot_completed)" = 1 ] && [ "$(getprop sys.user.0.ce_available)" = true ]; do
  n=$((n + 1)); [ $n -gt 8640 ] && exit 1; sleep 10
done
sleep 60
DB=$G/databases/phenotype.db
if [ -f $DB ]; then
  own=$(stat -c %u:%g $DB); lbl=$(ls -Z $DB | awk '{print $1}')
  am force-stop com.google.android.gms; sleep 2
  remove_flag $WORK/sqlite3 $DB
  for f in $DB $DB-wal $DB-shm $DB-journal; do
    [ -e $f ] && { chown $own $f; chcon $lbl $f; }
  done
  rm -f $G/files/phenotype/shared/$PKG.pb      # rebuilt without the flag
  am force-stop com.google.android.gms
fi
cmd deviceidle whitelist -com.google.android.mosey >/dev/null 2>&1
rm -f /data/local/tmp/phenotype-backup-*.db /data/local/tmp/mosey-s10e.log /data/local/tmp/mosey-s10e.log.prev
rm -f /sdcard/Download/mosey-report.txt /sdcard/Download/mosey-report.tar.gz
rm -rf /data/adb/mosey-extended $WORK
