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

### ✅ Panel: ILI9341-compatible, but **inverted** — confirmed at bring-up

The binary does not identify the panel controller — TFT_eSPI selects its driver
with a compile-time `#define`, so no driver name survives into the image. It was
resolved empirically instead.

**Electrical read-back does not work on this unit.** Both ID registers return
all zeroes:

```
0xD3 (RDDID4)  : 00 00 00
0x04 (RDDID)   : 00 00 00
```

This was anticipated: panel read-back arrives over MISO on **GPIO12, a live
strapping pin**. Treat all-zeroes as "could not tell", never as "wrong panel".

So identification was done visually, with a labelled colour-bar pattern that
needs no read-back. First flash rendered **every colour as its exact
complement**:

| Expected | Rendered |
|---|---|
| black background | white |
| white text | black |
| red | cyan |
| green | magenta |
| blue | yellow |
| white | black |

**This is a panel inversion mismatch, not a channel-order problem.** The
distinction matters and is easy to get wrong: a BGR panel swaps red and blue
*only* and leaves white, black and green untouched. Here white and black also
swapped and green went magenta, so every channel was complemented.

**Fix:** `-D TFT_INVERSION_ON=1` alongside `ILI9341_2_DRIVER`. Verified correct
on hardware afterwards — bars matching their labels, and a greyscale ramp
running dark-to-light in the right direction.

Geometry needed no correction: the 1px border was unbroken on all four edges and
text was positioned correctly, so the pin map, SPI settings and 240×320 offsets
were right from the start. Only the inversion state was wrong.

### Measured performance

| Metric | Value |
|---|---|
| Full-screen fill (320×240) | **31.2 ms** (~2.5 Mpixel/s) at 40 MHz SPI |
| Free heap at boot | 349,900 bytes |
| **Largest contiguous block** | **114,676 bytes** |
| Sketch size | 311 KB of the 1280 KB app partition |

The largest *contiguous* block is the figure that matters, since it caps any
single allocation — and at ~112 KB it independently confirms that the 150 KB
framebuffer was never an option, before Wi-Fi and TLS have even claimed their
share.

A 31 ms full redraw means rendering is not the bottleneck and DMA is not needed
for our workload, though the factory firmware proves it available if we ever
want it.

### ✅ Touch (XPT2046) — working, calibrated, **axes transposed**

Driven directly rather than through a library; see `src/touch_input.cpp` for
why. Verified on hardware, including a swipe-direction check.

| Measurement | Value |
|---|---|
| IRQ idle level | HIGH (correct — active low on contact) |
| Untouched baseline | X ≈ 500, Y ≈ 3520, **Z ≈ 1–3** |
| Pressure on deliberate contact | **500 – 1000** |
| Pressure threshold | 200 (see below) |
| **Axes** | **TRANSPOSED** — controller Y drives screen X |
| Calibration, screen X | raw `195 … 3711`, not inverted |
| Calibration, screen Y | raw `347 … 3682`, not inverted |

#### The digitizer is rotated relative to the display

The panel is 240×320 native portrait and we run it landscape, but the touch
controller reports in the panel's own orientation. Measured axis response:

```
moving along screen X:  rawX   -76,  rawY +2910   <- raw Y drives screen X
moving along screen Y:  rawX +2568,  rawY   +32   <- raw X drives screen Y
```

A 38× ratio — utterly unambiguous once measured on the correct axes.

**This is easy to get wrong in a way that looks right.** A two-point
calibration using opposite corners cannot detect it: both targets differ in
both axes, so "raw X drives screen X" and "raw X drives screen Y" fit the
measurements equally well and both produce a clean-looking monotonic result.
The symptom is a display that draws and taps plausibly but whose swipes go the
wrong way — it presents as *the screen needing to be rotated 90° under the
digitizer*.

Calibration therefore uses **three targets** — top-left, top-right,
bottom-left — so movement along each screen axis is isolated and the axis
assignment is measured. Firmware then asks for a rightward and a downward swipe
and checks the decoder agrees, so the device reports an orientation fault itself
instead of relying on someone being asked the right question.

#### Pressure threshold

Chosen from measurement. Idle reads Z = 1–3 and deliberate contact 500–1000. An
initial threshold of 300 was observed rejecting the onset of a genuine touch at
Z = 238, so it sat too near light contact; 200 keeps roughly a 60× margin over
the idle baseline while registering a light fingertip on the first sample.

#### Press detection

Uses the IRQ line rather than polling position over SPI: one digital read
instead of nine transfers per idle poll. The same line is the `ext0` deep-sleep
wake source, so this doubles as validation of the power plan.

### ⚠️ Light sensor (GPIO34) reads zero — unresolved

`analogRead` returns **exactly 0 with no variance** across averaged samples.
`analogReadMilliVolts` reports 142 mV, which is the ESP32 ADC's known
calibration floor rather than a real signal, so the pin is sitting at ground.

Most likely the **LDR is not populated on this unit** — some CYD batches ship
the footprint empty. Not yet confirmed with a torch test.

Consequence: **LDR auto-brightness may not be available on this board.** It is a
nice-to-have — backlight dimming on inactivity, which is the actual power win,
works regardless and does not depend on the sensor.

This firmware is overwritten by our first flash; the dual-slot table means the
demo is not recoverable, which is fine — it is freely available upstream.
