# Hardware Reference — ESP32-2432S028R ("Cheap Yellow Display", 2.8")

Everything in the **Verified** column below was read back from the physical board
attached during development, not copied from a datasheet. The discovery commands
are included so any future board can be re-checked the same way.

## 1. Silicon

| Property | Value | How verified |
|---|---|---|
| Chip | ESP32-D0WD-V3, revision v3.1 | `esptool flash-id` |
| Cores | 2 × Xtensa LX6 @ 240 MHz | eFuse feature list |
| Radio | Wi-Fi 802.11 b/g/n + Bluetooth Classic/BLE | eFuse feature list |
| Crystal | 40 MHz | `esptool flash-id` |
| Flash | 4 MB (mfr `0x68`, device `0x4016`) | `esptool flash-id` |
| Flash voltage | 3.3 V, set by strapping pin | `esptool flash-id` |
| **PSRAM** | **None** | `PKG_VERSION = 1` (D0WD ⇒ no in-package PSRAM); absent from eFuse feature list |
| Base MAC | `20:50:0D:34:04:50` | eFuse `MAC` |
| Flash encryption | Disabled (`FLASH_CRYPT_CNT = 0`) | `espefuse summary` |
| Secure boot | Not enabled | `espefuse summary` |
| Boot flash mode | DOUT @ 40 MHz (`clock div:2`) | ROM boot log |

**No PSRAM is the single most important constraint in this project.** We have only
the ~320 KB of internal DRAM, of which roughly 160–200 KB is realistically free
once Wi-Fi and TLS are up. Every design decision about JSON parsing, framebuffers
and HTTP buffering in [SPEC.md](../SPEC.md) follows from this.

```bash
# Reproduce the silicon report
esptool --port /dev/cu.usbserial-2130 --baud 115200 flash-id
espefuse --port /dev/cu.usbserial-2130 summary
```

## 2. USB-to-serial bridge

This board carries a **WCH CH340** (`idVendor 0x1A86` / `idProduct 0x7523`), which
matters for two practical reasons:

* macOS binds it as `/dev/cu.usbserial-*`, **not** `/dev/cu.usbmodem*` (that would
  indicate a CP2102 variant or native USB).
* **It is not reliable above 115200 baud on this unit.** Flash reads at 460800
  failed with `Invalid head of packet (0x80): Possible serial noise or corruption`
  and succeeded immediately at 115200. Upload/monitor speed is pinned accordingly
  in `platformio.ini` — do not raise it without re-testing.

```bash
ioreg -p IOUSB -l -w 0 | grep -E '"USB Product Name"|idVendor|idProduct'
```

## 3. GPIO map

Sourced from the community reference
([witnessmenow/ESP32-Cheap-Yellow-Display `PINS.md`](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md))
and consistent with the factory firmware on this unit.

### Display — ILI9341, 320×240, SPI
| GPIO | Signal |
|---|---|
| 15 | `TFT_CS` |
| 2 | `TFT_DC` / RS (data-command select) |
| 13 | `TFT_MOSI` |
| 12 | `TFT_MISO` |
| 14 | `TFT_SCLK` |
| 21 | `TFT_BL` (backlight — PWM-capable, see power plan) |

> ⚠️ `GPIO12` (MTDI) is a **strapping pin** that selects VDD_SDIO voltage at reset.
> `XPD_SDIO_FORCE` is `False` on this unit, so the strap is live. Never drive
> GPIO12 high externally at boot. `GPIO2` and `GPIO15` are also straps; the board
> wires them to display control lines, which is safe because the display is
> passive at reset, but it does mean **display MISO is on a strapping pin** and
> read-back from the panel should not be relied upon.

### Touch — XPT2046, resistive, separate SPI bus
| GPIO | Signal |
|---|---|
| 33 | `T_CS` |
| 32 | `T_MOSI` |
| 39 | `T_MISO` (input-only pin) |
| 25 | `T_CLK` |
| 36 | `T_IRQ` (input-only; **RTC-capable ⇒ valid `ext0` deep-sleep wake source**) |

Touch is on its own set of pins rather than sharing the display bus, so there is
no CS contention with the panel — but note that **GPIO36 and GPIO39 are
input-only** and have no internal pull-ups.

`T_IRQ` on GPIO36 being RTC-capable is what makes wake-on-touch from deep sleep
possible; this is central to the power strategy.

### SD card — VSPI, third independent bus
| GPIO | Signal |
|---|---|
| 5 | `SD_CS` |
| 18 | `SD_SCK` |
| 19 | `SD_MISO` |
| 23 | `SD_MOSI` |

Three separate SPI groupings (display / touch / SD) means the SD card can be
read without stalling display updates.

### On-board peripherals
| GPIO | Device | Project use |
|---|---|---|
| 4 | RGB LED — red (active LOW) | Goal-against alert, error state |
| 16 | RGB LED — green (active LOW) | Goal-for alert, healthy state |
| 17 | RGB LED — blue (active LOW) | Config/AP mode indicator |
| 26 | Speaker via amplifier (DAC2-capable) | Goal chime, kick-off alert |
| 34 | LDR ambient light sensor (input-only, ADC1) | Automatic backlight brightness |
| 0 | BOOT button | Long-press ⇒ factory reset / force AP mode |

GPIO26 is a true DAC pin and GPIO34 is on **ADC1**, which is the ADC that keeps
working while Wi-Fi is active (ADC2 does not) — so ambient-light reads are safe
at any time.

### Free GPIO on expansion headers
| GPIO | Header | Notes |
|---|---|---|
| 35 | P3 | Input-only, no pull-ups |
| 22 | P3 and CN1 | Fully general purpose |
| 27 | CN1 | Fully general purpose |

## 4. Flash layout as shipped

Read back from offset `0x8000` — the stock Arduino "default with OTA" table:

| Partition | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data/nvs | `0x9000` | 20 KB |
| `otadata` | data/ota | `0xE000` | 8 KB |
| `app0` | app/ota_0 | `0x10000` | 1280 KB |
| `app1` | app/ota_1 | `0x150000` | 1280 KB |
| `spiffs` | data/spiffs | `0x290000` | 1472 KB |

```bash
esptool --port /dev/cu.usbserial-2130 --baud 115200 read-flash 32768 3072 ptable.bin
```

1472 KB of filesystem is generous for our needs (cached JSON, Pure CSS, crests),
so the plan is to **keep dual OTA slots** rather than reclaim app1 for storage —
being able to roll back a bad firmware on a device with no debug header is worth
more than the extra space. See [SPEC.md](../SPEC.md) for the final table.

## 5. Factory firmware

The board arrived running the stock LVGL demo. Captured boot log:

```
ets Jul 29 2019 12:21:46
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
mode:DOUT, clock div:2
entry 0x400806a8
Hello Arduino! V8.3.3
I am LVGL_Arduino
Setup done
```

`V8.3.3` is the LVGL version, confirming Arduino + LVGL 8.3.3.

Strings recovered from the dumped `app0` image tell us what the vendor built with:

```
C:\Users\Administrator\Desktop\JYC\demo\libraries\TFT_eSPI\Processors/TFT_eSPI_ESP32.c
void TFT_eSPI::dmaWait()
I am LVGL_Arduino
LVGL v8
```

So the factory stack is **TFT_eSPI over LVGL 8.3.3, using SPI DMA** (`dmaWait` is
only linked in when DMA is enabled). That is a useful signal: DMA-driven SPI to
this panel is known-good on this exact board, which validates the rendering
approach chosen in [SPEC.md](../SPEC.md).

```bash
esptool --port /dev/cu.usbserial-2130 --baud 115200 read-flash 65536 1310720 app0.bin
strings -n 8 app0.bin | grep -iE "TFT_eSPI|LVGL"
```

### ⚠️ Panel controller is not yet hardware-confirmed

The binary does **not** identify the panel controller — TFT_eSPI selects its
driver with a compile-time `#define`, so no driver name survives into the image.
The ILI9341 attribution above comes from the board reference for the
ESP32-2432S028R, not from this unit.

A small number of `2432S028` batches ship an **ST7789** instead, which presents as
inverted colours and/or a wrong-way-round display rather than a blank screen.
This is resolved on our first display bring-up commit by rendering a known
test pattern; if colours or orientation are wrong, switching the TFT_eSPI driver
define is the one-line fix. Tracked as an open item in the spec.

This firmware is overwritten by our first flash; the dual-slot table means the
demo is not recoverable, which is fine — it is freely available upstream.
