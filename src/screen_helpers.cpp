/**
 * @file screen_helpers.cpp
 * @brief Shared formatting and drawing used across screens.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "screen.h"

namespace ui {

int16_t drawBoldString(TFT_eSPI& tft, const char* text, int16_t x, int16_t y,
                       uint8_t font) {
  const int16_t w = tft.drawString(text, x, y, font);
  // Second pass one pixel across thickens the strokes. Drawn in the same
  // colours, so it works with whatever the caller has set.
  tft.drawString(text, x + 1, y, font);
  return w + 1;
}

void drawStatBlock(TFT_eSPI& tft, int16_t cx, int16_t cy, const char* label,
                   const char* value, uint16_t valueColour) {
  // Offsets are derived from the font heights rather than eyeballed. Font 2 is
  // 16 px and Font 7 is 48 px, and MC_DATUM centres both vertically on the y
  // given — so with the original -20/+10 the label occupied cy-28..cy-12 and
  // the value cy-14..cy+34, overlapping by two pixels. These offsets leave a
  // 6 px gap instead.
  constexpr int16_t kLabelHalf = 8;   // Font 2: 16 px tall.
  constexpr int16_t kValueHalf = 24;  // Font 7: 48 px tall.
  constexpr int16_t kGap       = 6;

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(label, cx, cy - kValueHalf - kGap - kLabelHalf + 12, 2);
  tft.setTextColor(valueColour, colour::kBackground);
  tft.drawString(value, cx, cy + 12, 7);
}

void drawFormChips(TFT_eSPI& tft, const char* form, int16_t cx, int16_t cy) {
  if (form == nullptr || form[0] == '\0') return;

  const uint8_t n = static_cast<uint8_t>(strlen(form));
  constexpr int16_t kChip = 15;  // Square, and just wide enough for Font 1.
  constexpr int16_t kGap  = 3;

  const int16_t total = n * kChip + (n - 1) * kGap;
  int16_t x = cx - total / 2;

  for (uint8_t i = 0; i < n; ++i) {
    uint16_t fill;
    switch (form[i]) {
      case 'W': fill = colour::kWin;  break;
      case 'D': fill = colour::kDraw; break;
      case 'L': fill = colour::kLoss; break;
      default:  fill = colour::kMuted; break;
    }
    tft.fillRoundRect(x, cy - kChip / 2, kChip, kChip, 3, fill);

    // The most recent result is the last character; outlining it shows which
    // end is "now" without needing a caption.
    if (i == n - 1) {
      tft.drawRoundRect(x - 1, cy - kChip / 2 - 1, kChip + 2, kChip + 2, 4,
                        colour::kPrimary);
    }

    // Black on the bright fills gives better contrast than white would.
    const char letter[2] = {form[i], '\0'};
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, fill);
    tft.drawString(letter, x + kChip / 2, cy, 1);
    x += kChip + kGap;
  }
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
