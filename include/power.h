/**
 * @file power.h
 * @brief Power measurement and saving, instrumented on-device.
 *
 * **Why instrument rather than measure externally.** An inline USB meter is
 * the obvious tool, but this board's socket is micro-USB and the available
 * meter is USB-C, so absolute figures are unavailable. That turns out to
 * matter less than it sounds: what validates an optimisation is the
 * *relative* change, and the chip can measure the things that actually
 * determine its own consumption.
 *
 * Three quantities dominate, in order:
 *
 *   1. **Backlight duty.** The LED backlight is the largest single consumer,
 *      and its PWM duty is known exactly.
 *   2. **Radio time.** Wi-Fi is second. Time spent associated and awake
 *      versus in modem sleep is measurable.
 *   3. **CPU busy fraction.** How much of each second is spent out of idle,
 *      and at what clock. This is what light sleep reduces.
 *
 * Tracking all three gives a figure that moves the same way real consumption
 * does, so a change can be shown to help without a meter. It is explicitly
 * *not* a milliamp reading, and the code says so rather than implying
 * precision it does not have.
 */

#pragma once

#include <stdint.h>

namespace power {

/// A snapshot of what the device has been doing.
struct Stats {
  uint32_t uptimeS        = 0;
  /// Milliseconds the main loop spent doing work, since boot.
  uint32_t busyMs         = 0;
  /// Milliseconds spent idle — waiting, with nothing to do.
  uint32_t idleMs         = 0;
  /// Busy fraction of the last window, as a percentage.
  uint8_t  busyPercent    = 0;
  /// Backlight duty, 0-100.
  uint8_t  backlightPct   = 0;
  /// Time-weighted mean backlight duty since boot, 0-100.
  uint8_t  backlightMeanPct = 0;
  /// Current CPU clock.
  uint16_t cpuMhz         = 0;
  /// Screen redraws since boot — SPI traffic is CPU time and bus power.
  uint32_t redraws        = 0;
  /// Network fetches since boot; each is several seconds of radio and TLS.
  uint32_t fetches        = 0;
  /// Estimated relative cost, arbitrary units. Comparable only with itself.
  uint32_t relativeCost   = 0;
};

void begin();

/**
 * Report what this chip can actually do while asleep.
 *
 * Exists because the plan assumed something untrue. The intention was to deep
 * sleep with the panel still lit — the ILI9341 holds its own frame in GRAM, so
 * the image survives — but that needs the backlight pin held high through
 * sleep, and only RTC-capable pins can be held. Whether GPIO21 qualifies is a
 * property of the silicon, so it is asked rather than assumed.
 */
void reportSleepCapabilities();

// ---------------------------------------------------------------------------
// CPU frequency — implemented, measured, and NOT USED
// ---------------------------------------------------------------------------
//
// Kept because the measurements are worth more than the code, and because it
// would work on a build with power management enabled.
//
// CONFIG_PM_ENABLE is not set in the prebuilt Arduino libraries, so
// esp_pm_configure() — and with it *coordinated* frequency scaling — is
// unavailable. Scaling the clock manually with setCpuFrequencyMhz() while
// Wi-Fi is associated made the web interface unreliable: at 160 MHz only one
// of eight requests completed, and at 80 MHz results were erratic. Changing
// the CPU clock changes the APB clock, which the Wi-Fi and lwIP timers depend
// on, and nothing in this build reconciles the two.
//
// It also bought less than expected. Scaling does not reduce the *busy
// percentage* — the same work simply takes longer — it only reduces power
// during that time. Measured at 80 MHz, busy stayed at 11%.
//
// With scaling removed and the idle interval alone doing the work, busy fell
// from 11% to 1% and eight of eight web requests succeeded. That is the
// configuration that ships.

/// Full speed, for parsing and TLS.
constexpr uint32_t kFullSpeedMhz = 240;
/**
 * Idle speed.
 *
 * 160 MHz, not the 80 MHz floor, and that is a measured decision rather than
 * timidity. At 80 MHz the synchronous WebServer became unreliable — pages
 * returning HTTP 000 or taking 20 seconds — because serving a request takes
 * roughly three times as long and the server is polled from the same loop.
 * 160 MHz keeps the web interface dependable while still halving the clock.
 *
 * Note also what scaling does and does not buy: it does not reduce the *busy
 * percentage*, since the same work simply takes longer. It reduces the power
 * drawn during that busy time, which is why the cost index weights the CPU
 * term by clock.
 */
constexpr uint32_t kIdleSpeedMhz = 160;

/**
 * Claim or release full CPU speed.
 *
 * Reference-counted, and that is the important part: the clock is global, so
 * dropping to 80 MHz while the fetch task is mid-TLS-handshake would stretch
 * a six-second handshake toward its timeout. Anything that needs speed claims
 * it, and the frequency only falls when every claim is released.
 */
void requestFullSpeed(bool claim);

/// Current claims outstanding, for diagnostics.
uint8_t fullSpeedClaims();

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

/**
 * Light-sleep for up to `ms`, waking early on a touch.
 *
 * Light sleep rather than deep: it retains pin state, so the backlight stays
 * lit and the panel keeps showing its image. Deep sleep cannot do that on this
 * board — the backlight pin is not RTC-capable and so cannot be held through
 * sleep (see reportSleepCapabilities()).
 *
 * @return true if the sleep was actually attempted. False means it was
 *         declined — see the implementation for when and why.
 */
bool lightSleep(uint32_t ms);

/// Whether light sleep is permitted at all. Cleared if it proves to disturb
/// the radio, so the decision is observable rather than buried.
void setLightSleepEnabled(bool enabled);
bool lightSleepEnabled();

/// Light sleeps entered, and how long spent in them, for the summary line.
uint32_t sleepCount();
uint32_t sleptMs();

/// Called by the main loop around its work, so busy and idle can be split.
void markBusy(uint32_t ms);
void markIdle(uint32_t ms);

/// Note events whose power cost is worth attributing.
void noteRedraw();
void noteFetch();

/// Record the current backlight duty, for the time-weighted mean.
void noteBacklight(uint8_t percent);

/// Roll the measurement window. Call about once a second.
void tick();

const Stats& stats();

/// One line of the running figures, for the serial log.
void logSummary();

}  // namespace power
