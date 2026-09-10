/**
 * @file power.cpp
 * @brief Power instrumentation. See power.h.
 */

#include "power.h"

#include <Arduino.h>

namespace power {
namespace {

Stats    g_stats;
uint32_t g_windowStart   = 0;
uint32_t g_windowBusy    = 0;
uint32_t g_startedAt     = 0;

/// Time-weighted backlight accumulator: sum of (duty x milliseconds).
uint64_t g_backlightAcc  = 0;
uint8_t  g_backlightNow  = 100;
uint32_t g_backlightAt   = 0;

/**
 * Relative weights for the estimate.
 *
 * These are *ratios*, not milliamps, chosen from the published figures for
 * this class of hardware: the backlight dominates, the radio is next, and the
 * CPU is a distant third. The absolute scale is meaningless — only comparisons
 * between two runs of this firmware mean anything, which is exactly what is
 * needed to show whether a change helped.
 *
 * Stated as constants rather than buried in an expression so that anyone who
 * *does* get a meter on this board can correct them from measurements.
 */
constexpr uint32_t kWeightBacklight = 60;  ///< Per percent of duty.
constexpr uint32_t kWeightCpuBusy   = 25;  ///< Per percent of busy time.
constexpr uint32_t kWeightRadio     = 15;  ///< Per fetch per hour.

}  // namespace

void begin() {
  g_stats = Stats{};
  g_startedAt   = millis();
  g_windowStart = g_startedAt;
  g_windowBusy  = 0;

  // The backlight accumulator must be reset too, not just the stats.
  //
  // backlightSet() is called during setup *before* this runs, and it reports
  // to noteBacklight(), which accumulated duty x (now - g_backlightAt) with
  // g_backlightAt still zero — crediting the full uptime at once. The mean
  // then read above 100%, which is how the fault announced itself: an
  // impossible number rather than a subtly wrong one.
  g_backlightAcc = 0;
  g_backlightAt  = g_startedAt;

  g_stats.cpuMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
  g_stats.backlightPct = g_backlightNow;
  Serial.println(F("[power] instrumentation started"));
}

void markBusy(uint32_t ms) {
  g_stats.busyMs += ms;
  g_windowBusy   += ms;
}

void markIdle(uint32_t ms) { g_stats.idleMs += ms; }

void noteRedraw() { ++g_stats.redraws; }
void noteFetch()  { ++g_stats.fetches; }

void noteBacklight(uint8_t percent) {
  // Before begin(), there is no window to accumulate into. Recording the
  // level without crediting any time keeps the mean honest whichever order
  // the two are called in.
  if (g_startedAt == 0) {
    g_backlightNow = percent;
    g_stats.backlightPct = percent;
    return;
  }

  // Accumulate the *previous* setting over the time it was in force, before
  // adopting the new one — otherwise a device dimmed for hours and briefly
  // brightened would average as though it had been bright all along.
  const uint32_t now = millis();
  g_backlightAcc += static_cast<uint64_t>(g_backlightNow) * (now - g_backlightAt);
  g_backlightAt   = now;
  g_backlightNow  = percent;
  g_stats.backlightPct = percent;
}

void tick() {
  const uint32_t now     = millis();
  const uint32_t elapsed = now - g_windowStart;
  if (elapsed < 1000) return;

  g_stats.busyPercent = static_cast<uint8_t>(
      (static_cast<uint64_t>(g_windowBusy) * 100) / elapsed);
  if (g_stats.busyPercent > 100) g_stats.busyPercent = 100;
  g_windowBusy  = 0;
  g_windowStart = now;

  g_stats.uptimeS = (now - g_startedAt) / 1000;
  g_stats.cpuMhz  = static_cast<uint16_t>(getCpuFrequencyMhz());

  // Fold in the backlight time up to now, so the mean is current even when
  // brightness has not changed for a long while.
  const uint64_t acc =
      g_backlightAcc + static_cast<uint64_t>(g_backlightNow) * (now - g_backlightAt);
  const uint32_t totalMs = now - g_startedAt;
  const uint32_t mean = (totalMs > 0) ? static_cast<uint32_t>(acc / totalMs)
                                      : g_backlightNow;
  // Clamped, and loudly. A duty cycle cannot exceed 100%, so exceeding it
  // means the accumulator is wrong rather than the reading being surprising.
  if (mean > 100) {
    Serial.printf("[power] BUG: backlight mean computed as %lu%%\n",
                  (unsigned long)mean);
  }
  g_stats.backlightMeanPct = static_cast<uint8_t>(min<uint32_t>(mean, 100));

  // Fetches per hour, so the radio term is a rate rather than a total that
  // only ever grows.
  const uint32_t hours = (g_stats.uptimeS > 0) ? g_stats.uptimeS : 1;
  const uint32_t fetchesPerHour =
      static_cast<uint32_t>((static_cast<uint64_t>(g_stats.fetches) * 3600) /
                            hours);

  g_stats.relativeCost = g_stats.backlightMeanPct * kWeightBacklight +
                         g_stats.busyPercent * kWeightCpuBusy +
                         fetchesPerHour * kWeightRadio;
}

const Stats& stats() { return g_stats; }

void logSummary() {
  const Stats& s = g_stats;
  const uint32_t total = s.busyMs + s.idleMs;
  const uint8_t busyOverall =
      total > 0 ? static_cast<uint8_t>((static_cast<uint64_t>(s.busyMs) * 100) /
                                       total)
                : 0;
  Serial.printf(
      "[power] up %lus  cpu %uMHz  busy %u%% now / %u%% overall  "
      "backlight %u%% (mean %u%%)  redraws %lu  fetches %lu  cost %lu\n",
      (unsigned long)s.uptimeS, s.cpuMhz, s.busyPercent, busyOverall,
      s.backlightPct, s.backlightMeanPct, (unsigned long)s.redraws,
      (unsigned long)s.fetches, (unsigned long)s.relativeCost);
}

}  // namespace power
