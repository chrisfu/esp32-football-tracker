/**
 * @file screen_helpers.cpp
 * @brief Shared formatting and drawing used across screens.
 */

#include <stdio.h>
#include <time.h>

#include "screen.h"

namespace ui {

void drawStatBlock(TFT_eSPI& tft, int16_t cx, int16_t cy, const char* label,
                   const char* value, uint16_t valueColour) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(label, cx, cy - 20, 2);
  tft.setTextColor(valueColour, colour::kBackground);
  tft.drawString(value, cx, cy + 10, 7);
}

void formatScore(const model::Fixture& f, char* out, size_t len) {
  if (f.homeGoals < 0 || f.awayGoals < 0) {
    snprintf(out, len, "v");
  } else {
    snprintf(out, len, "%d-%d", f.homeGoals, f.awayGoals);
  }
}

void formatKickoff(uint32_t utcSeconds, char* out, size_t len) {
  if (utcSeconds == 0) {
    snprintf(out, len, "date unknown");
    return;
  }

  const time_t t = static_cast<time_t>(utcSeconds);
  struct tm tmBuf;

  // localtime_r honours the timezone once NTP and TZ are configured. Until
  // then it returns UTC, so the output is labelled to avoid quietly showing a
  // kick-off an hour out — a real risk during BST.
  localtime_r(&t, &tmBuf);
  const bool haveLocalTime = (tmBuf.tm_year + 1900) > 2020;

  if (!haveLocalTime) {
    snprintf(out, len, "time not synced");
    return;
  }

  static const char* kDays[]   = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  snprintf(out, len, "%s %d %s, %02d:%02d", kDays[tmBuf.tm_wday],
           tmBuf.tm_mday, kMonths[tmBuf.tm_mon], tmBuf.tm_hour, tmBuf.tm_min);
}

void formatCountdown(uint32_t utcSeconds, char* out, size_t len) {
  const time_t nowT = time(nullptr);
  // Before the clock is set, time() returns a value near zero. Any countdown
  // computed from it would be nonsense, so say so rather than show it.
  if (nowT < 1600000000L || utcSeconds == 0) {
    snprintf(out, len, "--");
    return;
  }

  const int32_t remaining =
      static_cast<int32_t>(utcSeconds) - static_cast<int32_t>(nowT);
  if (remaining <= 0) {
    snprintf(out, len, "kicking off");
    return;
  }

  const int32_t days  = remaining / 86400;
  const int32_t hours = (remaining % 86400) / 3600;
  const int32_t mins  = (remaining % 3600) / 60;

  // Progressively finer as it approaches: days out, nobody cares about
  // minutes; an hour out, nobody cares about days.
  if (days > 0) {
    snprintf(out, len, "in %ldd %ldh", (long)days, (long)hours);
  } else if (hours > 0) {
    snprintf(out, len, "in %ldh %ldm", (long)hours, (long)mins);
  } else {
    snprintf(out, len, "in %ldm", (long)mins);
  }
}

}  // namespace ui
