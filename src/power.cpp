/**
 * @file power.cpp
 * @brief Power instrumentation. See power.h.
 */

#include "power.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "board_config.h"

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

uint8_t  g_fullSpeedClaims = 0;
uint32_t g_currentMhz      = 240;
/**
 * Light sleep is off by default, and that is a finding rather than caution.
 *
 * Enabling it reset the device the instant it dimmed, with
 * `rst:0x10 (RTCWDT_RTC_RESET)` — the RTC watchdog firing during the sleep
 * transition. The cause is that this build has **CONFIG_PM_ENABLE unset** in
 * the prebuilt Arduino libraries, so there is no power-management layer to
 * coordinate `esp_light_sleep_start()` with the Wi-Fi driver or to place the
 * sleep-entry code in IRAM.
 *
 * Isolated rather than assumed: with light sleep off but frequency scaling on,
 * the device ran indefinitely and reported 80 MHz, so the clock change was not
 * the cause.
 *
 * The code is kept because it is correct as written and would work on a build
 * with power management enabled — which needs a custom IDF sdkconfig, not a
 * change here. Enable it with setLightSleepEnabled(true) if you have one.
 */
bool     g_lightSleepOk    = false;
uint32_t g_sleepCount      = 0;
uint32_t g_sleptMs         = 0;

/// Apply a frequency, and only when it actually changes.
void applyFrequency(uint32_t mhz) {
  if (mhz == g_currentMhz) return;
  // Serial is flushed first: the UART divisor is derived from the CPU clock,
  // so changing it mid-transmission garbles whatever is still buffered.
  Serial.flush();
  if (setCpuFrequencyMhz(mhz)) {
    g_currentMhz   = mhz;
    g_stats.cpuMhz = static_cast<uint16_t>(mhz);
  }
}

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

void reportSleepCapabilities() {
  Serial.println();
  Serial.println(F("=== Sleep capabilities ==="));

  const bool backlightIsRtc =
      rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(board::kPinBacklight));
  const bool touchIrqIsRtc =
      rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(board::kPinTouchIrq));

  Serial.printf("Backlight GPIO%u RTC-capable : %s\n", board::kPinBacklight,
                backlightIsRtc ? "yes" : "NO");
  Serial.printf("Touch IRQ GPIO%u RTC-capable : %s\n", board::kPinTouchIrq,
                touchIrqIsRtc ? "yes" : "NO");

  // ext0 wake is what makes wake-on-touch possible. Asked for and immediately
  // cancelled, purely to learn whether the pin is accepted.
  const esp_err_t ext0 = esp_sleep_enable_ext0_wakeup(
      static_cast<gpio_num_t>(board::kPinTouchIrq), 0);
  Serial.printf("ext0 wake on touch IRQ       : %s\n",
                ext0 == ESP_OK ? "accepted" : esp_err_to_name(ext0));
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);

  if (!backlightIsRtc) {
    Serial.println(F("=> Deep sleep CANNOT keep the screen lit on this board:"));
    Serial.println(F("   the backlight pin is not RTC-capable, so it cannot be"));
    Serial.println(F("   held through sleep. The panel keeps its image in GRAM,"));
    Serial.println(F("   but with no backlight there is nothing to see."));
    Serial.println(F("   Light sleep retains pin state, so that is the mode"));
    Serial.println(F("   for an idle-but-visible screen."));
  }
}

void requestFullSpeed(bool claim) {
  if (claim) {
    ++g_fullSpeedClaims;
    applyFrequency(kFullSpeedMhz);
    return;
  }
  if (g_fullSpeedClaims > 0) --g_fullSpeedClaims;
  // Only step down once nothing is claiming speed. The single shared counter
  // is what stops the UI and the fetch task undercutting each other.
  if (g_fullSpeedClaims == 0) applyFrequency(kIdleSpeedMhz);
}

uint8_t fullSpeedClaims() { return g_fullSpeedClaims; }

void setLightSleepEnabled(bool enabled) {
  if (g_lightSleepOk == enabled) return;
  g_lightSleepOk = enabled;
  Serial.printf("[power] light sleep %s\n", enabled ? "enabled" : "disabled");
}

bool lightSleepEnabled() { return g_lightSleepOk; }
uint32_t sleepCount() { return g_sleepCount; }
uint32_t sleptMs() { return g_sleptMs; }

bool lightSleep(uint32_t ms) {
  if (!g_lightSleepOk || ms == 0) return false;
  // Never sleep while something needs full speed: that claim means work is in
  // progress, and sleeping through it would be worse than pointless.
  if (g_fullSpeedClaims > 0) return false;

  // Wake on the touch IRQ as well as the timer, so a tap is answered at once
  // rather than at the end of the interval. This is the pin the capability
  // probe confirmed is RTC-capable.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(ms) * 1000ULL);
  esp_sleep_enable_ext0_wakeup(
      static_cast<gpio_num_t>(board::kPinTouchIrq), 0);

  const uint32_t before = millis();
  const esp_err_t rc = esp_light_sleep_start();
  const uint32_t slept = millis() - before;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  if (rc != ESP_OK) {
    // Reported once rather than silently falling back to a busy wait for the
    // rest of the device's life.
    static bool warned = false;
    if (!warned) {
      warned = true;
      Serial.printf("[power] light sleep refused (%s); using delay instead\n",
                    esp_err_to_name(rc));
    }
    return false;
  }

  ++g_sleepCount;
  g_sleptMs += slept;
  return true;
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

  // The CPU term is scaled by clock, since power rises with frequency: the
  // same busy time at 80 MHz costs roughly a third of what it does at 240.
  const uint32_t cpuTerm = (static_cast<uint32_t>(g_stats.busyPercent) *
                            kWeightCpuBusy * g_stats.cpuMhz) /
                           kFullSpeedMhz;
  g_stats.relativeCost = g_stats.backlightMeanPct * kWeightBacklight +
                         cpuTerm + fetchesPerHour * kWeightRadio;
}

const Stats& stats() { return g_stats; }

void logSummary() {
  const Stats& s = g_stats;
  const uint32_t total = s.busyMs + s.idleMs;
  const uint8_t busyOverall =
      total > 0 ? static_cast<uint8_t>((static_cast<uint64_t>(s.busyMs) * 100) /
                                       total)
                : 0;
  const uint32_t sleptPct =
      (s.uptimeS > 0) ? min<uint32_t>((g_sleptMs / 10) / s.uptimeS, 100) : 0;
  Serial.printf(
      "[power] up %lus  cpu %uMHz  busy %u%% now / %u%% overall  "
      "backlight %u%% (mean %u%%)  asleep %lu%% (%lu naps)  "
      "redraws %lu  fetches %lu  cost %lu\n",
      (unsigned long)s.uptimeS, s.cpuMhz, s.busyPercent, busyOverall,
      s.backlightPct, s.backlightMeanPct, (unsigned long)sleptPct,
      (unsigned long)g_sleepCount, (unsigned long)s.redraws,
      (unsigned long)s.fetches, (unsigned long)s.relativeCost);
}

}  // namespace power
