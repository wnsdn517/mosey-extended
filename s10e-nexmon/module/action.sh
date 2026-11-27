#!/system/bin/sh
# Root manager "Action" button: save a diagnostic report to Download.
MODDIR=${0%/*}
echo "Collecting report..."
if out=$(sh $MODDIR/report.sh manual); then
  echo "Saved: $out"
  echo "Check it for personal information before sharing."
else
  echo "Failed (unlock the phone and retry)"
fi
