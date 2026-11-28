## 0.8
- Updates show up in the root manager
- Report is now `mosey-report.tar.gz` and includes the Wi-Fi firmware and driver details, to help support other models
- Better on other custom ROMs: the SELinux service list is rebuilt at every boot, and an unexpected SoC name only warns (the firmware check stays)

## 0.7
- No kernel module any more: fast transfer is no longer tied to one kernel build
- Uninstall script undoes the changes made outside the module
- README explains how the module works
