![Bruce](./media/pictures/bruce_banner.jpg)

# FlockDetect

A private fork of the [Bruce](https://github.com/BruceDevices/firmware) ESP32 firmware
that adds **FlockDetect** — a passive detector for Flock Safety ALPR gear and related
surveillance devices, built for the **LilyGO T-Embed CC1101** (ESP32-S3, 16 MB flash /
8 MB PSRAM).

Everything Bruce already does is still here. This fork adds one module,
`src/modules/wifi/flock_detect.{h,cpp}`, reachable from two menu entries:

- `WiFi → FlockDetect`
- `Bluetooth → FlockDetect BLE`

Detection is **passive receive only** — the module never transmits.

---

## Status

> **Detection is not yet confirmed against real hardware.** FlockDetect has not
> produced a verified hit even with Flock Safety equipment known to be in range, and
> the root cause has not been identified.

The current build (**v2**) is therefore a *diagnostics* build. It deliberately leaves
the detection logic byte-for-byte unchanged from v1 and instead adds the
instrumentation needed to find out which stage of the pipeline is failing. Adding
measurement before guessing at a fix is the whole point of this build — a speculative
change now would likely mask the real fault.

Five hypotheses are still live: randomized/locally-administered MAC rejection, missing
OUI table entries, an over-brittle exact-match on the IE signature, ring-buffer frame
drops, and 256-byte frame truncation. The diagnostic counters below are designed to
discriminate between them.

---

## How it detects

Four independent vectors feed a tiered **CONFIRMED / SUSPICIOUS / INFO** model.
CONFIRMED fires an alert takeover with buzzer and LED; SUSPICIOUS and INFO are
list-only.

| Vector | Signal |
|---|---|
| IE signature | Information-Element fingerprint of a Flock camera's wildcard probe request |
| SSID patterns | `Flock-XXXXXX`, `test_flck`, `Penguin-<10 digits>`, `FS Ext Battery`, `flock`/`flck` substrings |
| Hidden SSID | Beacons with an empty SSID element from a Flock-family OUI, deduplicated per BSSID |
| OUI tables | Flock, SoundThinking/ShotSpotter, and surveillance-camera vendor prefixes |

BLE adds Raven acoustic-sensor service UUIDs (with a firmware-generation estimate),
Flock-family device names, and the XUNTONG `0x09C8` manufacturer ID.

Detections can be logged to SD as CSV, optionally GPS-tagged.

---

## Diagnostics

`SEL → Diagnostics`. Then **SEL** cycles pages, **Up/Dn** scrolls, **ESC** returns to
the list.

1. **Capture chain** — a counter at every boundary from the promiscuous callback
   through the ring buffer, the parser, and the classifier. If `rx all` is 0 the radio
   callback never fired; if `localMacRej` is high while `cand oui` is 0 the target is
   MAC-randomizing; if `ringHigh` approaches the ring size, frames are being dropped.
2. **IE signatures** — the signatures actually computed from wildcard probes, recorded
   *before* the match test so a near-miss against the expected constant is visible
   rather than silently discarded.
3. **All sources** — every management-frame source heard this session regardless of
   tier, with its real OUI. This is how a target's MAC surfaces when it never scores.

With `SD Log: ON`, a companion `flockdiag_*.csv` is written alongside the detection log
for offline analysis — useful because the device is normally flashed over USB rather
than kept on a serial console.

---

## Build and flash

```sh
pio run -e lilygo-t-embed-cc1101
```

That produces a merged image at `Bruce-lilygo-t-embed-cc1101.bin` in the project root
(bootloader + partition table + app), plus the app-only image at
`.pio/build/lilygo-t-embed-cc1101/firmware.bin`.

```sh
esptool.py --chip esp32s3 --baud 921600 write_flash 0x0 Bruce-lilygo-t-embed-cc1101.bin
```

If the board doesn't enter download mode on its own, hold **BOOT** while tapping
**RESET**. Prebuilt images are attached to the [releases](../../releases).

### Verifying a build works

Before trusting a negative result, ground-truth the SSID vector: bring up any AP named
`Flock-A1B2C3` or `test_flck`. It must fire CONFIRMED within a channel sweep. If it
doesn't, the fault is in capture rather than in the signature tables — go to diagnostics
page 1.

---

## Upstream Bruce

This fork tracks [BruceDevices/firmware](https://github.com/BruceDevices/firmware).
The original project README — install options, supported boards, the full feature list,
contributing guidelines and community links — is preserved verbatim at
**[README-Bruce.md](./README-Bruce.md)**.

Bruce is licensed AGPL-3.0; see [LICENSE](./LICENSE).

---

## Credits

FlockDetect is a port, not original research. The detection logic comes from the
upstream projects below, adapted to Bruce's promiscuous-capture path, UI and data
model. Full per-function attribution — including which upstream contributed which
routine — is in **[THIRD_PARTY.md](./THIRD_PARTY.md)**.

| Project | | Contributed |
|---|---|---|
| **[colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you)** | MIT | The wildcard-probe Information-Element signature builder and matcher, the IE-string canonicalizer, and the phantom-overflow / TLV-resync tolerant IE walk. |
| **[JakeSwiz/flock-you-wifi-recon](https://github.com/JakeSwiz/flock-you-wifi-recon)** | MIT¹ | SSID text-pattern matching, hidden-SSID flagging with per-BSSID dedup, the curated Flock / SoundThinking OUI tables, and the BLE detector (Raven service UUIDs, device names, XUNTONG manufacturer ID). |
| **[f1yaw4y/FlockSquawk](https://github.com/f1yaw4y/FlockSquawk)** | GPL-3.0 | The tiered CONFIRMED / SUSPICIOUS / INFO alert model — device tracking, first-seen anti-spam, alert takeover — and the surveillance-camera vendor OUI table curated from the FlockOff database. |
| **[wgreenberg/flock-you](https://github.com/wgreenberg/flock-you)** | — ² | BLE manufacturer-ID research behind the XUNTONG detection. |
| **[zmattmanz/flock-detection](https://github.com/zmattmanz/flock-detection)** | — ² | ESP32-S3 surveillance detector covering the same ground — Flock ALPR cameras and Raven gunshot sensors over WiFi/BLE, with confidence scoring and GPS-tagged CSV logging. Prior art for the tiered-confidence and logging approach. |

¹ MIT per the source headers, © 2026 colonelpanichacks (fork by JakeSwiz); the repository
carries no separate `LICENSE` file.
² No license file published upstream at the time of writing; credited for research and
prior art, no code was taken directly.

Underlying research credited by those projects: **@NitekryDPaul** (OUI list, receiver-address
technique), **Michael / DeFlockJoplin** (wildcard-probe signature), **GainSec** (2025 Flock
Safety SSID research), **Jon Gaines**, and **Pintor & Atzori (2022)** (probe-request IE
fingerprinting).

---

Signatures shift as vendors change hardware. Treat SUSPICIOUS and INFO results as leads
to verify, not as positive identifications.
