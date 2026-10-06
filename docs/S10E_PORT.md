# Porting mosey (AirDrop) to the Galaxy S10 family — BCM4375 / Exynos 9820

Status: **research, radio-layer RX confirmed on real hardware; TX injection
is the open blocker.** This document records what has been verified on a
real device and from static analysis, so the port can be reproduced and
continued.

> Independent interoperability research. Not affiliated with or endorsed by
> Google or Apple. All work is on the owner's own device.

## 1. Target device

| | |
|---|---|
| Device | Samsung Galaxy S10 family (`beyond*lte`), Exynos 9820 |
| Wi-Fi | Broadcom **BCM4375B1**, `bcmdhd` (DHD 101.16.90), PCIe |
| Test unit | S10e, One UI 4.1 / **Android 12 (SDK 31)**, rooted (KernelSU) |
| Kernel | `4.14.113-GoRhanHee_Kernel_S10+` (custom) |

The custom kernel is what makes the port feasible. Its config
(`/proc/config.gz`) shows:

```
CONFIG_MODULES=y          # out-of-tree modules load
# CONFIG_MODULE_SIG is not set   # no module signature required
CONFIG_MODVERSIONS=y      # modules must match this build's symbol CRCs
# CONFIG_UH is not set           # Samsung uH/RKP hypervisor off
# CONFIG_UH_LKMAUTH is not set   # no LKM authentication
CONFIG_KSU=y              # KernelSU in-kernel
CONFIG_CFG80211=y
# CONFIG_MAC80211 is not set      # no mac80211 (so no mac80211 vif module)
CONFIG_BCM4375=y          # uses bcmdhd_101_16
CONFIG_TUN=y
CONFIG_SECURITY_SELINUX_DEVELOP=y   # permissive-capable
```

Implication: a standalone `.ko` built against this exact kernel
(`bcmdhd_101_16` is at
`drivers/net/wireless/broadcom/bcmdhd_101_16` in the GoRhanHee tree)
loads without signing or uH interference. `MODVERSIONS=y` means it must be
built against this kernel's `Module.symvers`. There is **no mac80211**, so
the Pixel approach (`wonder_mosey_wild.ko`, a mac80211 virtual phy) cannot
be reused as-is; the bridge must sit on cfg80211 / bcmdhd directly.

## 2. What mosey_daemon actually requires

Extracted from `system/vendor/lib64/libmosey_daemon_ffi.so` (Rust, source
paths `location/nearby/protocolx/mosey_daemon/...`). The daemon does **not**
speak a bespoke kernel ABI for frames — it uses standard interfaces:

| Need | How the daemon does it | Evidence (strings) |
|------|------------------------|--------------------|
| Monitor netdev | opens `wonder0` (falls back to `radiotap0`) | `wonder0`, `radiotap0` |
| Frame RX | **libpcap** on that iface, radiotap link-type | `radiotap-rx`, `pcap library`, `Radiotap header not supported` |
| Frame TX | **libpcap `send_packet`** (radiotap injection) | `radiotap-tx`, `send_packet` |
| Set channel | vendor cmd `ART_SET_CHAN` | `ART_SET_CHAN` |
| Get iface MAC | vendor cmd `ART_GET_IF_ADDR` | `ART_GET_IF_ADDR` |
| Fixed TX rate | vendor cmd `ART_TX_RATE` | `ART_TX_RATE` |
| BSSID / filter | vendor cmd `ART_BSSID` | `ART_BSSID` |
| Transport for ART | private ioctl (`ioctl_art_cmd`) and/or nl80211 | `ioctl_art_cmd`, `.../ifc/nl.rs`, `.../ifc/ic.rs` |

So the "wonder" interface the daemon wants is simply **a radiotap monitor
netdev it can pcap-RX and pcap-TX on**, plus a handful of **ART vendor
commands** for channel/MAC/rate/bssid. This is the whole contract.

## 3. What stock Samsung firmware already provides

The BCM4375 ships with a **monitor firmware** next to the normal one:

```
/vendor/firmware/bcmdhd_sta.bin_b1    # normal STA
/vendor/firmware/bcmdhd_mon.bin_b1    # MONITOR  <-- Firm_ver: 18.41.117 (B1 Monitor)
```

`bcmdhd_101_16` has `-DWL_MONITOR`. Loading the `_mon` firmware and setting
`WLC_SET_MONITOR` creates a **`radiotap0`** netdev of type
`ARPHRD_IEEE80211_RADIOTAP` — exactly the link-type the daemon's pcap RX
expects.

### Confirmed on hardware (RX)

1. Point the driver at the monitor firmware (filename only; the driver adds
   the `_b1` chip-rev suffix and the `/vendor/firmware/` dir itself — do NOT
   pass an absolute path, that double-prefixes and fails):
   ```sh
   echo -n bcmdhd_mon.bin > /sys/module/dhd/parameters/firmware_path
   svc wifi disable; sleep 2; svc wifi enable
   # dmesg: "firmware path=bcmdhd_mon.bin_b1" + "Monitor mode is enabled in FW cap"
   ```
2. Monitor mode must be entered while the radio is **up and not associated**.
   Stock auto-reconnect must be suppressed first (disassoc, or turn off
   auto-reconnect / forget the AP), otherwise `WLC_SET_MONITOR` returns
   `EPERM` (associated) or `EINVAL` (radio down):
   ```sh
   wlmon down; wlmon on; wlmon up      # or: disassoc then wlmon on
   # radiotap0 appears; dhd: "dhd_add_monitor_if : disable runtime PM in monitor mode"
   ```
3. Channel set + capture works:
   ```sh
   wlmon chanspec 0x1006          # 2.4GHz ch6   (0xd095 = 5GHz ch149)
   tcpdump -i radiotap0 -e -c 30  # real 802.11 frames, radiotap hdr, RSSI, TSFT
   ```

RX is therefore **native, no Nexmon required**. See `tools/s10e/wlmon.c`.

Mapping the ART contract to stock controls:

| ART vendor cmd | Stock equivalent on BCM4375 | Status |
|----------------|------------------------------|--------|
| `ART_SET_CHAN` | `WLC_SET_VAR "chanspec"` | ✅ works (wlmon chanspec) |
| `ART_GET_IF_ADDR` | `WLC_GET_VAR "cur_etheraddr"` / `WLC_GET_BSSID` | ✅ trivial |
| `ART_TX_RATE` | noop (or `WLC_SET_VAR "nrate"`) | ⚠️ likely safe to noop |
| `ART_BSSID` | filter — noop in monitor | ⚠️ likely safe to noop |

## 4. The one open blocker: frame TX (injection)

The daemon injects frames with pcap `send_packet` on the radiotap iface.
On `bcmdhd_101_16`, the monitor netdev's TX handler **drops frames**:

```c
/* drivers/net/wireless/broadcom/bcmdhd_101_16/dhd_linux.c */
static int dhd_monitor_start(struct sk_buff *skb, struct net_device *dev)
{
    PKTFREE(NULL, skb, FALSE);   /* <-- injected frame is freed, never sent */
    return 0;
}
```

So pcap TX on `radiotap0` is silently discarded. This is the single reason
"real RF" does not work, and it is the same wall Pixel 7/8 hit.

There are two candidate ways through, to be evaluated in order:

### Option A — stock `actframe` TX path (no firmware patch)
`bcmdhd` already transmits **action frames** via the cfg80211 mgmt_tx path
(`wl_cfg80211_mgmt_tx` → `wl_cfgp2p_tx_action_frame`, iovar `actframe`) and
reports link-layer ACK (`WLC_E_ACTION_FRAME_COMPLETE`). AWDL sync/discovery
frames are vendor-specific **action** frames (category `0x7f`, Apple OUI
`00:17:f2`), so this path can carry discovery traffic without touching
firmware. `tools/s10e/wlaf.c` sends one action frame via the `actframe`
iovar for testing.

Open question (Task #1): does the stock firmware actually put the frame on
the air, and can an off-channel/unassociated actframe reach an AWDL peer?
Test = send with `wlaf`, capture on a second monitor device on the same
channel. Limitation: `actframe` carries management/action frames only, not
arbitrary data frames, so Option A may cover discovery but not the data
plane.

### Option B — patch the monitor TX path in bcmdhd (BLOCKED by flowrings)
The obvious idea is to change `dhd_monitor_start` to forward the skb to the
real TX path with the 802.11-frame flag (`BCMPCIE_PKT_FLAGS_FRAME_802_11`,
already used on the RX side in `dhd_msgbuf.c:5868`) instead of freeing it.
Reading the data TX path shows this **does not work as-is**:
`dhd_prot_txdata()` requires a **flowring** per packet
(`dhd_msgbuf.c:6853`, `flowid = DHD_PKT_GET_FLOWID(PKTBUF)`), and flowrings
are created per `(ifidx, dest-MAC, prio)` bound to an **association**. A
monitor injection frame has arbitrary 802.11 addresses and no association,
so there is no flowring to carry it. This is precisely why Nexmon patches
the **firmware** (below the flowring/msgbuf layer) rather than the driver.

So driver-level monitor TX injection would require inventing a dedicated
injection flowring or a firmware-level raw-TX path — substantial and
firmware-dependent. It is **not** a small `dhd_monitor_start` edit.

### Which TX path actually works

| TX route | needs flowring? | arbitrary frames? | S10e |
|----------|-----------------|-------------------|------|
| data path (`dhd_prot_txdata`) | yes → needs association | — | ❌ no flowring for monitor frames |
| `actframe` / control path (iovar) | no (goes via control ring) | action frames only | ✅ for AWDL sync/discovery |
| Nexmon firmware patch | n/a (firmware level) | yes (all) | ⚠️ needs port to fw 18.41.117 |

AWDL sync/discovery frames **are** action frames, so the discovery phase can
go through the stock `actframe` control path (Option A) without touching
firmware. Only the **data plane** needs Nexmon. The bridge should therefore
classify daemon TX frames: action frames → `actframe`; data frames → Nexmon
(or deferred).

## 5. Port architecture for S10e

```
          mosey_server (Android 16 ROM only; minSdk/API 36)
                 │  binder  com.google.pixel.moseyservice.IMoseyService
          libmosey_daemon_ffi.so
                 │ pcap RX/TX on "wonder0"      │ ART vendor cmds
                 ▼                               ▼
   ┌─────────────────────────────┐   ┌──────────────────────────────┐
   │ wonder0  = renamed radiotap0 │   │ ART shim  (ioctl/nl80211)    │
   │  (stock _mon firmware)       │   │  ART_SET_CHAN -> chanspec    │
   │  RX  ✅ native                │   │  ART_GET_IF_ADDR-> cur_ether │
   │  TX  action->actframe (ctrl) │   │                              │
   │      data  ->Nexmon (fw)     │   │                              │
   └─────────────────────────────┘   └──────────────────────────────┘
                 │                               │
                 ▼                               ▼
            bcmdhd (BCM4375)  +  GoRhanHee kernel 4.14.113
```

Two separable halves:

- **Radio layer** (this device, testable now on Android 12): RX done; TX is
  Option A/B. Independent of the ROM.
- **mosey_server plumbing** (needs an Android-16 base such as a One UI 8.5
  port, because MoseyApp/mosey_server are `minSdk 36` / need Android-16
  `libbinder_ndk` symbols): rename `radiotap0`→`wonder0`, run the ART shim,
  start mosey_server. Layered on top once the radio layer works.

Even with everything wired, discovery depends on matching Apple's AWDL
framing/timing on the air; see `MOSEY_CAPTURE_TEST_SUITE.md`.

## 6. Tools

`tools/s10e/` (`make` on-device with Termux clang, or NDK cross-compile):

- `wlmon.c` — monitor-mode control over the bcmdhd private ioctl:
  `up/down`, `on/off` (`WLC_SET_MONITOR`), `get`, `chanspec <hex>`,
  `assoc`, and raw `set/geti <cmd> <int>` for probing.
- `wlaf.c` — send one action frame via the `actframe` iovar (Option A
  injection test). Run on the normal STA firmware, capture on a second
  device.

Chanspecs: `0x1006` = 2.4 GHz ch6 / 20 MHz, `0xd095` = 5 GHz ch149 / 20 MHz.

## 7. Reproduce / recover

```sh
# build tools (Termux)
cd tools/s10e && make && su -c 'cp wlmon wlaf /data/local/tmp/ && chmod 755 /data/local/tmp/wl*'

# back to normal Wi-Fi at any time (no reboot needed)
su -c '/data/local/tmp/wlmon off' 2>/dev/null
su -c 'echo -n bcmdhd_sta.bin > /sys/module/dhd/parameters/firmware_path'
su -c 'svc wifi disable; sleep 2; svc wifi enable'
```

`firmware_path` is a module parameter and resets on reboot, so a reboot also
fully restores stock Wi-Fi.

## 8. Open tasks

1. **Option A test** — `wlaf` action frame confirmed on-air by a second
   monitor device? Decides whether discovery works without a firmware patch.
2. **Data-plane TX** — driver monitor injection is blocked by flowrings
   (see §4), so arbitrary-frame TX needs a Nexmon firmware patch ported to
   fw 18.41.117 (base patch exists for 18.41.8.9 / Galaxy S20, same B1
   chip). Scope only if discovery via `actframe` proves the concept.
3. **ART shim** — implement the four ART commands (module or LD_PRELOAD)
   against the daemon once running on an Android-16 base.
4. **wonder0 naming** — `ip link set radiotap0 name wonder0` vs. teaching the
   daemon to use `radiotap0`.
