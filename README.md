# ESP32 Football Tracker

A football (soccer) statistics display for the ESP32 **"Cheap Yellow Display"**
(2.8" ESP32-2432S028R). It follows one team and cycles through screens of live
scores, results, fixtures, the league table, top scorers and injuries — all
configured from a built-in web interface, with a first-boot Wi-Fi setup portal.

Default team is **Bolton Wanderers** (English Championship), configurable from
the web UI.

Built against the [API-Sports football API](https://v3.football.api-sports.io)
free tier, which allows **100 requests per day** — so the design is
aggressively cache-first and budget-aware. Note that the free tier turns out to
carry [significant undocumented restrictions](#-the-free-tier-is-more-restricted-than-its-own-metadata-suggests).

> **Status:** early development. Hardware discovery complete; firmware bring-up
> next. See [SPEC.md](SPEC.md) for the design and [the roadmap](SPEC.md#11-roadmap)
> for progress.

## Documentation

| Document | Contents |
|---|---|
| [SPEC.md](SPEC.md) | Living specification — architecture, screens, caching, API budget, power plan |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Verified hardware reference, GPIO map and discovery commands |

## Tested devices

Hardware that has been physically verified for this project. Values here were
read back **from the board itself**, not from a datasheet — see
[docs/HARDWARE.md](docs/HARDWARE.md) for the commands to reproduce them.

### ✅ ESP32-2432S028R — 2.8" Cheap Yellow Display

*Tested 2026-09-09 — the primary development target.*

| Property | Value |
|---|---|
| Board | ESP32-2432S028R ("Cheap Yellow Display", 2.8") |
| Chip | ESP32-D0WD-V3, revision **v3.1** |
| Cores | 2 × Xtensa LX6 @ 240 MHz |
| Radio | Wi-Fi 802.11 b/g/n + Bluetooth Classic / BLE |
| Crystal | 40 MHz |
| Flash | **4 MB** (mfr `0x68`, device `0x4016`), DOUT mode @ 40 MHz |
| PSRAM | **None** — `PKG_VERSION = 1` (D0WD has no in-package PSRAM) |
| MAC | `20:50:0D:34:04:50` |
| Display | 320×240 SPI, ILI9341-compatible — **inverted variant**, needs `TFT_INVERSION_ON` |
| Touch | XPT2046 resistive, on independent SPI pins |
| Extras | microSD slot (own SPI bus), RGB LED, speaker/DAC, LDR light sensor, 3 free GPIO |
| USB bridge | **WCH CH340** (`0x1A86:0x7523`) |
| Serial port | `/dev/cu.usbserial-2130` (macOS) |
| Flash encryption | Disabled |
| Secure boot | Not enabled |
| Display speed | 31.2 ms full-screen fill (~2.5 Mpixel/s) at 40 MHz SPI |
| Free heap / largest block | 349,900 B / **114,676 B contiguous** |
| Light sensor | ⚠️ reads 0 — **likely not populated** on this unit |
| As shipped | Factory LVGL 8.3.3 demo over TFT_eSPI with SPI DMA (since overwritten) |
| Partitions | Stock Arduino dual-OTA: `nvs` 20 K, `otadata` 8 K, `app0`/`app1` 1280 K each, `spiffs` 1472 K |

**Findings worth knowing before you flash this board:**

* ⚠️ **The CH340 on this unit is not reliable above 115200 baud.** Flash reads at
  460800 failed with `Invalid head of packet (0x80): Possible serial noise or
  corruption` and worked first time at 115200. Upload and monitor speeds are
  pinned to 115200 in `platformio.ini` — don't raise them without re-testing.
* ⚠️ **No PSRAM.** With ~320 KB of internal DRAM (and only ~160–200 KB free once
  Wi-Fi and TLS are up), a full 320×240×16-bit framebuffer would need 150 KB and
  is simply not affordable. The firmware streams API responses straight into a
  filtered JSON parser and draws through a small reusable sprite instead.
* ⚠️ **This panel is an inverted variant.** With a stock ILI9341 config it
  renders every colour as its exact complement — white background, black text,
  green as magenta, blue as yellow. The fix is `-D TFT_INVERSION_ON=1`, already
  applied. Note this is *not* a BGR channel-order problem, which is the usual
  first guess: a BGR panel swaps red and blue only and leaves white and green
  alone. Geometry, pin map and offsets all needed no correction.
* ⚠️ **Panel ID read-back is unavailable.** Both `0xD3` and `0x04` return all
  zeroes, because MISO is on GPIO12, a live strapping pin. Identify the panel
  visually with a test pattern instead — all-zeroes means "can't tell", not
  "wrong panel".
* ⚠️ **The light sensor reads a flat 0** (142 mV is just the ADC calibration
  floor), so GPIO34 is at ground and the LDR is probably not populated on this
  unit. Auto-brightness may therefore be unavailable; inactivity-based dimming
  works regardless.
* ℹ️ `GPIO12` (display MISO) is a live strapping pin — never drive it high at
  boot. `GPIO2` and `GPIO15` are also straps but are safely wired here.
* ℹ️ Display, touch and SD sit on **three separate SPI groupings**, so SD access
  never stalls display updates.
* ℹ️ Touch IRQ (`GPIO36`) is RTC-capable, so **wake-on-touch from deep sleep**
  works. The LDR (`GPIO34`) is on ADC1, which keeps working while Wi-Fi is
  active.

## Toolchain

Development uses PlatformIO with the Arduino framework:

| Component | Version |
|---|---|
| PlatformIO Core | 6.1.19 |
| `espressif32` platform | 6.13.0 |
| Arduino ESP32 core | 2.0.17 |
| esptool | 5.0.0 |

```bash
# Build, flash and monitor (serial speed pinned to 115200 — see above)
pio run
pio run --target upload
pio device monitor
```

## API notes

The free API-Sports tier has **two** limits, both verified from live response
headers:

* **100 requests/day**, resetting at midnight UTC (`x-ratelimit-requests-limit`)
* **10 requests/minute** (`x-ratelimit-limit`) — this one wasn't in the original
  brief but is just as real

Because every response reports the remaining quota, the device reconciles its
own counter against the server's figure rather than trusting a local tally that
could drift. (`/status` proved unreliable for this — it reported 2 of 100 used
after six billable calls, so the headers are the source of truth.)

### ⚠️ The free tier is more restricted than its own metadata suggests

`/leagues` advertises seasons 2010–2026 with full coverage flags. That describes
seasons which *exist*, not seasons the plan may *query*. Probing the real data
endpoints found:

- **Season queries are locked to 2022–2024** — the current season is
  unreachable for standings, top scorers and team statistics
- **The `next` and `last` fixture parameters are blocked outright**
- **Date queries are clamped to roughly ±1 day around today**
- **But `live=all` works and returns genuinely current in-play matches**

Which is a curious inversion: this API gives away the live data most providers
charge for, and withholds the static tables most providers hand out free. So the
live-match screen works well, while the league table, season record and top
scorer need another source for current-season data.

Resolving this is an open decision — the options and their trade-offs are in
[the API coverage section](SPEC.md#6-api-budget-rule-r7), along with the full
probe matrix and the rationing strategy. Usage is tracked and displayed in the
web interface.

## Licence

Not yet chosen.
