## 0.9
- ROMs with their own MoseyApp: it is hidden while the module is active, so the bundled app gets its Wi-Fi permissions
- If the app still cannot take the phone off Wi-Fi, the module does it for the session
- Google Play services flag: waits for the database and retries
- Boot protection: a watchdog for boots that never complete, and the app hiding is undone after an unfinished boot
- Files mounted at boot are copied to memory first, so `/data` is not held at shutdown
- Report and logs now show what the radio received and why a session could fail; automatic reports run at low priority

## 0.8
- Updates show up in the root manager
- Report is now `mosey-report.tar.gz` and includes the Wi-Fi firmware and driver details, to help support other models
- Better on other custom ROMs: the SELinux service list is rebuilt at every boot, and an unexpected SoC name only warns (the firmware check stays)

## 0.7
- No kernel module any more: fast transfer is no longer tied to one kernel build
- Uninstall script undoes the changes made outside the module
- README explains how the module works
