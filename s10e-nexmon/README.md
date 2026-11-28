# Quick Share AirDrop for S10

Send files from a Galaxy S10 to Apple devices with the stock Samsung Quick Share.

Forked from [lok1s/mosey-extended](https://github.com/thelok1s/mosey-extended).

## Requirements

| | |
|---|---|
| Phone | Galaxy S10 series with Exynos 9820 (other models get a warning; the firmware must still match) |
| System | Android 16 or newer with One UI |
| Wi-Fi firmware | Stock BCM4375B1 18.41.117 (the installer checks the exact file) |
| Root | KernelSU (tested with KernelSU Next) |
| Apps | Google Play services and a Samsung Quick Share with Apple sharing (tested: 13.9.10.5) |

Tested on one Galaxy S10e (SM-G970N) with ArtisanKRNL 3.1.5. Other S10 models and kernels are
untested.

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

## How it works

### The problem

AirDrop runs on AWDL, Apple's peer-to-peer protocol on top of Wi-Fi. Google's `mosey` stack
implements it for Quick Share, but expects a Wi-Fi driver that can send and receive raw 802.11 frames.
The Exynos S10's Broadcom driver and firmware cannot.

### The pieces

```
Samsung Quick Share ─ Google Play services ─ MoseyApp ─ mosey_server (AWDL, mDNS, transfer)
                                                            │  raw 802.11 frames
                                                   libmoseynex.so (preloaded into mosey_server)
                                                            │
                                            stock Samsung Wi-Fi driver (unchanged)
                                                            │
                                         BCM4375B1 firmware with nexmon patches ─ radio
```

| Part | Role |
|---|---|
| Module overlays | Add MoseyApp, its permissions and the service declaration; report a newer One UI version so Quick Share offers Apple sharing; enable one Google Play services flag |
| `mosey_server` | The AirDrop protocol itself. Patched only to use its "radiotap" backend and to run on the S10's CPU |
| `libmoseynex.so` | Gives `mosey_server` the interface it expects: presents `wlan0` as `radiotap0`, answers its channel and rate commands, and sends its frames |
| Firmware patches | Monitor mode and frame injection ([nexmon](https://github.com/seemoo-lab/nexmon)), plus the additions below |

### A transfer, step by step

1. Choosing a file in Quick Share makes MoseyApp leave the Wi-Fi network and start `mosey_server`.
2. The library tunes the radio to channel 149, stops Android's Wi-Fi scans and arms monitor mode.
   Scans would pull the radio off the channel: the Apple device would not show up, or a transfer would
   stall every second.
3. **Receiving:** the firmware passes every frame on the channel to the host, and `mosey_server` picks
   out the AWDL ones.
4. **Sending:** frames go out through the normal network path of the stock driver. The driver only
   queues packets for a connected interface, so the firmware reports a link to it when monitor mode is
   armed (an event Android itself ignores). A hook in the firmware takes these packets and injects
   them. No kernel module or kernel change is needed.
5. The AirDrop connection (discovery, accept, upload) runs over IPv6 on a virtual interface that
   `mosey_server` creates.
6. When sharing ends, the library disarms monitor mode, allows scans again and restores the MAC
   address; the phone rejoins its network.

### Why it is fast

- **A-MSDU:** up to seven packets share one transmission and one acknowledgement. The driver rejects
  packets over 1600 bytes, so the library cuts the bundle into normal-sized pieces and the firmware
  chains them back together.
- **Automatic rate and size:** every 0.3 s the library compares the acknowledgements received with the
  peer's replies to transmit requests. While nearly everything arrives it raises the data rate (up to
  VHT MCS 9) and the bundle size; when delivery drops it lowers them.

### Limits

- The Apple device decides when it listens on the AirDrop channel (see [Speed](#speed)).
- Frames are not aggregated the way Apple devices do it (A-MPDU), so speeds between two Apple devices
  are out of reach.

### Safety

The installer checks the device and every file before changing anything. A bootloop guard disables the
module, a watchdog clears a leftover monitor mode, and after a crash during a transfer the module
falls back to a slow sending method with bundling off until it is installed again. The firmware is
bind-mounted at boot; no system partition or kernel is written.

## Uninstall

Remove the module in your root manager and reboot. After the first unlock it also removes the Google
Play services flag it set (with the database backups), the battery exemption it gave MoseyApp, and its
own files. MoseyApp's data, and an updated copy of the app if there is one, are left alone.

## Settings

Optional files in `/data/adb/mosey-extended/`:

| File | Effect |
|---|---|
| `amsdu` | `auto` (default), `off`, or a fixed number of frames per bundle (`2`-`7`) |
| `tx_rate` | Fixed rate such as `VHT 7 2` (used when the automatic control is not active) |
| `slow_mode` | Written after a crash during fast transfer; delete it to try again |
| `no_monwatch` | Pauses the monitor-mode watchdog |

## Troubleshooting

- Tap the module's **Action** button to save `Download/mosey-report.tar.gz`. It is also saved after
  a failed install, other failures and kernel crashes. It holds `report.txt` (device, Wi-Fi driver and
  logs) and the phone's Wi-Fi firmware files, which are needed to support other models. It stays on the
  phone; check it for personal information before sharing.
- **Apple device not listed:** set AirDrop on it to receive from everyone, keep its screen on, and
  open a new share sheet.
- **Transfer fails right after "connecting":** try again from a new share sheet with another copy of
  the file.
- **Wi-Fi does not reconnect:** close Quick Share completely, then toggle Wi-Fi.

## Build

| Step | Where | Command |
|---|---|---|
| Firmware | Linux / WSL | `bash nexmon/build_fw.sh` |
| `tools/wlmon` | included | source: `tools/wlmon.c` |
| Module zip | PowerShell | `pwsh build.ps1` |

The zip is written to `out/mosey-s10e.zip`.

Root managers look for updates in `update.json`. To publish one, raise `version` and `versionCode` in
`module/module.prop`, release the zip, then point `update.json` and `CHANGELOG.md` at it.

| Path | Contents |
|---|---|
| `moseynex.c` | The preloaded library: interface mapping, injection, bundling, rate control |
| `nexmon/` | Firmware patches (injection rate, data path, A-MSDU, link report) |
| `module/` | Installer, uninstaller and boot, watchdog, status and report scripts |
