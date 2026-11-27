#!/system/bin/sh
# Enables GMS Nearby's external sharing providers (Quick Share -> MoseyApp ExternalSharingService):
# phenotype flag "45704091" (bool) in config package com.google.android.gms.nearby
# (FlagSnapshot.enableExternalProviders, GMS 26.45.x).
#
# FLAG_OVERRIDE broadcasts are ignored on release GMS, so the override is written into GMS's CE
# phenotype.db and linked to the committed experiment state; GMS then rebuilds
# files/phenotype/shared/com.google.android.gms.nearby.pb with the flag.
# Idempotent: does nothing (and does not stop GMS) when the .pb already carries the flag.
#
#   mosey_flag.sh <sqlite3> [log]      exit 0 = flag active, 1 = applied (GMS restarted), 2 = not possible yet
SQ=$1
LOG=${2:-/dev/null}
FLAG=45704091
PKG=com.google.android.gms.nearby
G=/data/data/com.google.android.gms
DB=$G/databases/phenotype.db
PB=$G/files/phenotype/shared/$PKG.pb

flog() { echo "$(date '+%H:%M:%S') [flag] $*" >> "$LOG"; }

[ -x "$SQ" ] || { flog "no sqlite3 ($SQ)"; exit 2; }
[ -f "$DB" ] || { flog "phenotype.db not available yet (CE locked?)"; exit 2; }

q() { "$SQ" -batch -noheader "$DB" "$1" 2>>"$LOG"; }

nid=$(q "SELECT config_package_id FROM config_packages WHERE name='$PKG';")
[ -n "$nid" ] || { flog "$PKG not registered in phenotype.db yet"; exit 2; }
oid=$(q "SELECT override_id FROM flag_overrides WHERE config_package_id=$nid AND account_id=0 AND name='$FLAG' AND value=1;")
es=$(q "SELECT committed_experiment_state_id FROM config_packages WHERE config_package_id=$nid;")
linked=""
[ -n "$oid" ] && [ -n "$es" ] && linked=$(q "SELECT 1 FROM experiment_states_to_overrides WHERE experiment_state_id=$es AND override_id=$oid;")

if [ -n "$oid" ] && [ -n "$linked" ] && [ -f "$PB" ] && grep -q "$FLAG" "$PB" 2>/dev/null; then
  exit 0
fi
flog "applying (nid=$nid oid=${oid:-none} es=${es:-none} linked=${linked:-no})"

own=$(stat -c %u:%g "$DB"); lbl=$(ls -Z "$DB" | awk '{print $1}')
cp -p "$DB" "/data/local/tmp/phenotype-backup-$(date +%Y%m%d).db" 2>/dev/null
am force-stop com.google.android.gms
sleep 2

q "PRAGMA foreign_keys=ON;
INSERT OR IGNORE INTO accounts(account_id, name) SELECT 0, '' WHERE NOT EXISTS (SELECT 1 FROM accounts WHERE account_id=0);
INSERT OR IGNORE INTO flag_overrides(config_package_id, account_id, active, name, value, type, source)
  VALUES($nid, 0, 1, '$FLAG', 1, 2, 0);
UPDATE flag_overrides SET value=1, type=2 WHERE config_package_id=$nid AND account_id=0 AND active=1 AND name='$FLAG';"
oid=$(q "SELECT override_id FROM flag_overrides WHERE config_package_id=$nid AND account_id=0 AND name='$FLAG';")
[ -n "$oid" ] || { flog "insert failed"; exit 2; }
q "INSERT OR IGNORE INTO flag_overrides_to_commit(override_id, config_package_id, account_id) VALUES($oid, $nid, 0);"
[ -n "$es" ] && q "INSERT OR IGNORE INTO experiment_states_to_overrides(experiment_state_id, override_id) VALUES($es, $oid);"

for f in "$DB" "$DB-wal" "$DB-shm" "$DB-journal"; do
  [ -e "$f" ] && { chown "$own" "$f"; [ -n "$lbl" ] && chcon "$lbl" "$f"; }
done
rm -f "$PB"
am force-stop com.google.android.gms
flog "override_id=$oid linked to es=${es:-none}; GMS restarted, $PB will be rebuilt"
exit 1
