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
