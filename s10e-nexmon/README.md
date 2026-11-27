# Quick Share AirDrop for S10

Send files from a Galaxy S10 to Apple devices with the stock Samsung Quick Share.

The Exynos S10 has no Wi-Fi driver for Google's `mosey` (AirDrop) stack. This module supplies it with
a [nexmon](https://github.com/seemoo-lab/nexmon)-patched BCM4375B1 firmware and a small library that
adapts `mosey_server` to it. Forked from [lok1s/mosey-extended](https://github.com/thelok1s/mosey-extended).

## Requirements

| | |
|---|---|
| Phone | Galaxy S10 series with Exynos 9820 |
| System | Android 16 or newer with One UI |
| Wi-Fi firmware | Stock BCM4375B1 18.41.117 (the installer checks the exact file) |
| Root | KernelSU (tested with KernelSU Next) |
| Apps | Google Play services and a Samsung Quick Share with Apple sharing (tested: 13.9.10.5) |

Tested on one Galaxy S10e (SM-G970N) with ArtisanKRNL 3.1.5. Other S10 models are untested.

## Install

1. Flash the zip in your root manager and reboot.
2. In Quick Share settings, turn on **Share with Apple devices**.
3. Keep Wi-Fi on. The phone leaves its network while sharing and reconnects afterwards.

## Speed

Sending from an S10e to an iPhone, measured on one pair of devices:

| iPhone | Speed |
|---|---|
| Not connected to a Wi-Fi network | about 9 MB/s |
| Connected to a 5 GHz router | about 3.5 MB/s, less on some runs |

**Tip:** for large files, tap the Wi-Fi button in the iPhone's Control Center to leave the router (do
not turn Wi-Fi off in Settings). While the iPhone is on a network it visits the AirDrop channel only
part of the time, and the phone has to wait for it. Distance, interference and the Apple device also
matter. Receiving speed has not been measured.

## What the module does

- **Fast transfer:** frames go to the firmware through the normal network path (needs the bundled
  kernel module; a kernel that refuses it runs in slow mode).
- **Frame bundling (A-MSDU):** up to seven frames per transmission, with the bundle size and the data
  rate adjusted from the peer's acknowledgements. Fast transfer only.
- **Steady reception:** Android's Wi-Fi scans are suppressed during a session. Without this the radio
  drifts off the channel: the Apple device does not show up, or a transfer stalls every second.
- **Safety:** installer checks, bootloop protection, a watchdog for a leftover monitor mode, and
  fallback after a crash (slow mode, bundling off) until the module is installed again.

## Settings

Optional files in `/data/adb/mosey-extended/`:

| File | Effect |
|---|---|
| `amsdu` | `auto` (default), `off`, or a fixed number of frames per bundle (`2`-`7`) |
| `tx_rate` | Fixed rate such as `VHT 7 2` (used when the automatic control is not active) |
| `slow_mode` | Written after fast transfer failed; delete it to try again |
| `no_monwatch` | Pauses the monitor-mode watchdog |

## Troubleshooting

- Tap the module's **Action** button to save `Download/mosey-report.txt`. It is also saved after
  failures and kernel crashes. It stays on the phone; check it for personal information before sharing.
- **Apple device not listed:** set AirDrop on it to receive from everyone, keep its screen on, and
  open a new share sheet.
- **Transfer fails right after "connecting":** try again from a new share sheet with another copy of
  the file.
- **Wi-Fi does not reconnect:** close Quick Share completely, then toggle Wi-Fi.

## Build

| Step | Where | Command |
|---|---|---|
| Firmware | Linux / WSL | `bash nexmon/build_fw.sh` |
| Kernel module (optional) | Linux / WSL, your kernel tree | see `kmod/Makefile` |
| `tools/wlmon` | included | source: `tools/wlmon.c` |
| Module zip | PowerShell | `pwsh build.ps1` |

The zip is written to `out/mosey-s10e.zip`.

| Path | Contents |
|---|---|
| `moseynex.c` | Library preloaded into `mosey_server`: interface mapping, injection, bundling, rate control |
| `nexmon/` | Firmware patches (injection rate, data path, A-MSDU) |
| `kmod/` | Kernel module that keeps the transmit path open while unconnected |
| `module/` | Installer and boot, watchdog, status and report scripts |
