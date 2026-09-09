# ESP32 Football Tracker

A football (soccer) statistics display for the ESP32 **"Cheap Yellow Display"**
(2.8" ESP32-2432S028R). It follows one team and cycles through screens of live
scores, results, fixtures, the league table, top scorers and injuries — all
configured from a built-in web interface, with a first-boot Wi-Fi setup portal.

Built against the [API-Sports football API](https://v3.football.api-sports.io)
free tier, which allows **100 requests per day** — so the design is
aggressively cache-first and budget-aware.

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
| Display | 320×240 SPI, ILI9341 *(assumed — see note)* |
| Touch | XPT2046 resistive, on independent SPI pins |
| Extras | microSD slot (own SPI bus), RGB LED, speaker/DAC, LDR light sensor, 3 free GPIO |
| USB bridge | **WCH CH340** (`0x1A86:0x7523`) |
| Serial port | `/dev/cu.usbserial-2130` (macOS) |
| Flash encryption | Disabled |
| Secure boot | Not enabled |
| As shipped | Factory LVGL 8.3.3 demo over TFT_eSPI with SPI DMA |
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
* ⚠️ **The panel controller is not yet hardware-confirmed.** TFT_eSPI picks its
  driver at compile time, so the factory image contains no driver name to read
  back. ILI9341 comes from the board reference; a few `2432S028` batches ship an
  ST7789, which shows up as inverted colours or wrong orientation and is a
  one-line define change. Confirmed at display bring-up.
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
could drift. Season coverage was checked against the live account: **2010–2026
are all available on the free plan**, including the current season, with
standings, top scorers, injuries and fixture events all covered.

Usage is tracked and displayed in the web interface. See
[the API budget section](SPEC.md#6-api-budget-rule-r7) for the full rationing
strategy.

## Licence

Not yet chosen.
