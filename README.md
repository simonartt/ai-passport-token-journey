# TOKEN JOURNEY — AI balance tracker for FoloToy AI Passport

[English](README.md) | [简体中文](README.zh_CN.md)

**TOKEN JOURNEY** is a community play for [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) that turns the tiny wearable card into a **live AI-spend dashboard**: a 30-day burn heatmap, per-provider balances, and today's spending — rendered on the device, with your API keys stored **only on the device**.

Built on the official `main` baseline of `FoloToy/ai-passport` (MIT). All new code in this repository inherits the MIT license.

<p align="center">
  <img src="docs/screenshots/cover.png" alt="TOKEN JOURNEY — 30-day spend heatmap and live balances on FoloToy AI Passport" width="420">
</p>

---

## Features

| | |
|---|---|
| **TOKEN JOURNEY page** | 30-day spend heatmap (6 × 5 grid, oldest → newest, today at the bottom-right), purple → cyan scale. Every day comes from the device-side ledger. |
| **TOKEN BALANCE page** | Total balance (CNY) plus per-provider balance and today's spend (shown in pink when negative). |
| **Lock screen** | Mint "HELLO TOKEN" dot-matrix. Short-press **UP** to lock/unlock; 3-minute inactivity auto-lock with stepped backlight dimming. |
| **On-demand Wi-Fi** | Radio powers up only during a refresh (≈2–8 s), fetches once for both pages, then tears down. |
| **SoftAP setup portal** | Connect your phone to the device hotspot (`BAL-xxxx`), open `192.168.4.1`, and enter Wi-Fi credentials, provider API keys, and an optional nickname (shown on the lock screen). |
| **Device-side ledger** | 31-slot ring buffer in NVS records the daily baseline; daily spend and the heatmap are computed locally (UTC+8). No cloud backend required. |
| **Providers** | DeepSeek · Kimi — extensible via the adapter table in `main/we_provider.c`. |

### Controls

- Boot → TOKEN JOURNEY page (page 1)
- **DOWN** short-press — switch between TOKEN JOURNEY ↔ TOKEN BALANCE
- **OK** short-press — refresh now (fetch + store ledger, then radio off)
- **UP** short-press — lock / unlock (in lock state only UP unlocks)
- **OK** long-press — back to the demo menu

## Screens

Real captures from the device (240 × 320):

<p align="center">
  <img src="docs/screenshots/lock.png" alt="Lock screen: HELLO TOKEN" width="150">
  <img src="docs/screenshots/journey.png" alt="Page 1: TOKEN JOURNEY 30-day heatmap" width="150">
  <img src="docs/screenshots/balance.png" alt="Page 2: TOKEN BALANCE with today's spend" width="150">
</p>

From left to right: the **HELLO TOKEN** lock screen (short-press UP to unlock), page 1 **TOKEN JOURNEY** — 30-day spend heatmap (today framed in pink), and page 2 **TOKEN BALANCE** — total and per-provider balances with today's spend.

---

## Install (no toolchain required)

1. Download **`FoloToy-AI-Passport-full.bin`** from the latest [Release](https://github.com/hellonick/ai-passport-token-journey/releases/latest).
2. Flash it with the official web flasher: <https://ai-passport.folotoy.cn/tools/web-flasher/> — or with esptool:

   ```bash
   esptool.py --chip esp32c3 --port /dev/cu.usbmodem* write_flash 0x0 FoloToy-AI-Passport-full.bin
   ```

3. First boot: open the menu, start **Setup**, connect your phone to the `BAL-xxxx` hotspot (password `12345678`), browse to `192.168.4.1`, enter your Wi-Fi and **DeepSeek / Kimi API keys**, then refresh with **OK**.

> `full.bin` contains the whole partition image and **erases NVS** when flashed at `0x0` — perfectly fine for a first install. On a device already running a play, prefer flashing the app partition only (`write_flash 0x10000 <app>.bin`) to keep its configuration.

---

## Build from source

- ESP-IDF **5.5.3**, chip **ESP32-C3** (no PSRAM — small buffers only), 8 MB flash.
- The repository keeps the official partition contract: 3 MB app limit, `cardid` at `0x356000`, permanent Recovery at `0x700000`, 5-second UP-key recovery hook. Do not move or erase these.

```bash
# ESP-IDF 5.5.3 environment
source $IDF_PATH/export.sh
idf.py build
# Production image (merged from 0x0)
idf.py merge-bin -o "$PWD/build/FoloToy-AI-Passport-full.bin"
python3 tools/verify_firmware.py "$PWD/build"
```

Repository gate (repository checks + host tests + firmware verification):

```bash
./tools/validate.sh --static    # repo checks + host tests
./tools/validate.sh --firmware  # clean build + merged-image verification
./tools/validate.sh             # complete gate
```

---

## Repository layout

This repository is the official `main` baseline plus the community play. The play-specific code:

| Path | Purpose |
|---|---|
| `main/app_balance.c` | Two-page dashboard (TOKEN JOURNEY / TOKEN BALANCE), lock screen, dimming, ledger logic |
| `main/we_cfg.c` / `we_cfg.h` | Configuration model: Wi-Fi profiles, provider keys, nickname (NVS `cfg`) |
| `main/we_hist.h` | Per-day ledger data model shared by `app_balance.c` and the backup module (NVS `balhist`) |
| `main/we_backup.c` / `we_backup.h` | Config + ledger backup/restore as one JSON document (`GET /backup`, `POST /restore`) |
| `main/we_portal.c` / `we_portal.h` | Setup portal: joins the local network with the saved Wi-Fi profiles, falls back to SoftAP |
| `main/we_provider.c` / `we_provider.h` | Provider adapters: HTTPS direct to official balance endpoints |
| `main/lock_hello.h` | Lock-screen "HELLO TOKEN" dot-matrix bitmap |
| `main/fap_screenshot.c` / `.h` | Screenshot protocol used by the official publishing flow |
| `components/bsp/src/bsp_battery.c` | CW2017 battery profile (required, otherwise SOC always reads as `--`) |
| `scripts/patch-esp-lvgl-port.py` | Patch for the LVGL port component (re-apply after components are re-pulled) |
| `tests/test_we_cfg.c`, `tests/test_we_provider.c` | Host tests for pure-logic layers |

## Privacy

- API keys are entered by you on the device during setup and stored **only in the device NVS**. No keys are hard-coded in the firmware or shipped in this repository.
- The firmware contacts only: your Wi-Fi AP, the configured providers' official balance endpoints, and (optionally) an SNTP time server. No telemetry, no personal data collection.
- Do **not** flash a configuration NVS backup from someone else's device — it would contain their keys.

## License & provenance

MIT License — see [LICENSE](LICENSE). Upstream copyright © 2026 FoloToy. The play was developed on top of the official baseline; see the commit history for the full change list.
