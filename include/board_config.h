/**
 * @file board_config.h
 * @brief Pin map and hardware constants for the ESP32-2432S028R
 *        ("Cheap Yellow Display", 2.8").
 *
 * Single source of truth for every GPIO on the board. Every value here was
 * verified against the physical unit during hardware discovery — see
 * docs/HARDWARE.md for how, and for the reasoning behind the warnings below.
 *
 * Display pins are NOT repeated here: TFT_eSPI needs them at compile time, so
 * they live in platformio.ini's build flags. Duplicating them would create two
 * places to get them wrong.
 */

#pragma once

#include <stdint.h>

namespace board {

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

/// Panel is 240x320 native (portrait). We run landscape, hence the swap.
constexpr uint16_t kScreenWidth  = 320;
constexpr uint16_t kScreenHeight = 240;

/// TFT_eSPI rotation for landscape with the USB socket on the left.
constexpr uint8_t kScreenRotation = 1;

/**
 * Backlight, driven by us rather than TFT_eSPI so it can be dimmed.
 *
 * Dimming is the single largest power saving available (the backlight
 * dominates draw), so this pin is on its own LEDC channel from the start.
 */
constexpr uint8_t kPinBacklight = 21;

/// LEDC channel and parameters for backlight PWM.
/// 5 kHz is well above audible range and far below the LEDC 8-bit limit at
/// 80 MHz, so there is no whine and no visible flicker.
constexpr uint8_t  kBacklightChannel  = 0;
constexpr uint32_t kBacklightFreqHz   = 5000;
constexpr uint8_t  kBacklightBits     = 8;
constexpr uint8_t  kBacklightMaxDuty  = 255;

// ---------------------------------------------------------------------------
// Touch — XPT2046 resistive controller
// ---------------------------------------------------------------------------
//
// On its own set of pins rather than sharing the display bus, so there is no
// chip-select contention with the panel. These are not native SPI pins, so the
// touch driver routes through the GPIO matrix — fine at the low clock a
// resistive touch controller needs.

constexpr uint8_t kPinTouchClk  = 25;
constexpr uint8_t kPinTouchMosi = 32;
constexpr uint8_t kPinTouchCs   = 33;
constexpr uint8_t kPinTouchMiso = 39;  ///< Input-only pin, no internal pull-up.

/**
 * Touch interrupt.
 *
 * Input-only, and — importantly — RTC-capable, which makes it a valid `ext0`
 * deep-sleep wake source. Wake-on-touch is central to the power plan, so this
 * pin is why the device can sleep with a screen still showing.
 */
constexpr uint8_t kPinTouchIrq = 36;

/// XPT2046 tolerates only a slow clock; 2.5 MHz is the usual safe figure.
constexpr uint32_t kTouchSpiHz = 2500000;

// ---------------------------------------------------------------------------
// microSD card — VSPI, a third independent bus
// ---------------------------------------------------------------------------
//
// Separate from both display and touch, so card access never stalls a screen
// redraw. Optional hardware: everything degrades gracefully with no card in.

constexpr uint8_t kPinSdCs   = 5;
constexpr uint8_t kPinSdSck  = 18;
constexpr uint8_t kPinSdMiso = 19;
constexpr uint8_t kPinSdMosi = 23;

// ---------------------------------------------------------------------------
// On-board RGB LED
// ---------------------------------------------------------------------------
//
// ACTIVE LOW — driving a pin LOW lights that channel. Easy to get backwards,
// hence use the helpers in the status LED module rather than these directly.

constexpr uint8_t kPinLedRed   = 4;
constexpr uint8_t kPinLedGreen = 16;
constexpr uint8_t kPinLedBlue  = 17;

/// Level that switches an LED channel on. See note above.
constexpr uint8_t kLedOn  = 0;  // LOW
constexpr uint8_t kLedOff = 1;  // HIGH

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

/**
 * Speaker via the on-board amplifier. This is a true DAC pin (DAC2), so it can
 * play a real waveform rather than only a square-wave beep.
 *
 * Muted by default in software: a device that beeps unbidden is a device that
 * gets unplugged. Goal chimes are opt-in from the web UI.
 */
constexpr uint8_t kPinSpeaker = 26;

// ---------------------------------------------------------------------------
// Ambient light sensor
// ---------------------------------------------------------------------------

/**
 * LDR for automatic backlight brightness.
 *
 * On ADC1, which is the ADC that keeps working while Wi-Fi is active (ADC2
 * does not). That is what makes auto-brightness safe to sample at any time.
 * Input-only pin.
 */
constexpr uint8_t kPinLdr = 34;

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

/// BOOT button. Long-press clears credentials and forces AP config mode.
constexpr uint8_t kPinBootButton = 0;

// ---------------------------------------------------------------------------
// Free GPIO on the expansion headers
// ---------------------------------------------------------------------------
//
// Unused by the board itself and available for future expansion.
//   GPIO 35 — P3 header, input only, no pull-ups
//   GPIO 22 — P3 and CN1 headers, fully general purpose
//   GPIO 27 — CN1 header, fully general purpose

// ---------------------------------------------------------------------------
// Strapping-pin warnings
// ---------------------------------------------------------------------------
//
// GPIO12 (display MISO) is the MTDI strapping pin selecting VDD_SDIO voltage
// at reset, and XPD_SDIO_FORCE is not burned on this unit, so the strap is
// live: never drive GPIO12 high externally at boot.
//
// GPIO2 and GPIO15 are also strapping pins. They are safely wired here because
// the panel is passive at reset — but it does mean display read-back over MISO
// should not be relied upon.

}  // namespace board
