/**
 * @file screens.cpp
 * @brief Rendering for each screen.
 *
 * Every screen draws only within the content area published by screen.h, and
 * reads a const Snapshot it does not own. None of them know where the data
 * came from.
 */

#include "screens.h"

#include <stdio.h>
#include <string.h>

namespace ui {
namespace {

/// Shorten a club name for display, dropping the " FC"/" AFC" suffix that adds
/// no information and costs horizontal space we need for the numbers.
void shortenClubName(const char* in, char* out, size_t len) {
  strncpy(out, in, len - 1);
  out[len - 1] = '\0';
  const size_t n = strlen(out);
  const struct { const char* suffix; } kDrop[] = {{" FC"}, {" AFC"}};
  for (const auto& d : kDrop) {
    const size_t sl = strlen(d.suffix);
    if (n > sl && strcmp(out + n - sl, d.suffix) == 0) {
      out[n - sl] = '\0';
      return;
    }
  }
}

/// Colour for a result from our point of view.
uint16_t resultColour(const model::Fixture& f) {
  if (f.homeGoals < 0 || f.awayGoals < 0) return colour::kPrimary;
  const int8_t ours   = f.weAreHome ? f.homeGoals : f.awayGoals;
  const int8_t theirs = f.weAreHome ? f.awayGoals : f.homeGoals;
  if (ours > theirs) return colour::kWin;
  if (ours < theirs) return colour::kLoss;
  return colour::kDraw;
}

/// Draw "HOME  score  AWAY" with our side highlighted, used by three screens.
void drawFixtureHeadline(TFT_eSPI& tft, const model::Fixture& f, int16_t y,
                         const char* centre, uint16_t centreColour) {
  char home[model::kNameLen], away[model::kNameLen];
  shortenClubName(f.homeName, home, sizeof(home));
  shortenClubName(f.awayName, away, sizeof(away));

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(f.weAreHome ? colour::kOurTeam : colour::kPrimary,
                   colour::kBackground);
  tft.drawString(home, board::kScreenWidth / 2 - 40, y, 2);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(f.weAreHome ? colour::kPrimary : colour::kOurTeam,
                   colour::kBackground);
  tft.drawString(away, board::kScreenWidth / 2 + 40, y, 2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(centreColour, colour::kBackground);
  tft.drawString(centre, board::kScreenWidth / 2, y, 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// Live match
// ---------------------------------------------------------------------------

void LiveMatchScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::Fixture& f = d.live.fixture;

  char score[8];
  formatScore(f, score, sizeof(score));
  drawFixtureHeadline(tft, f, kContentTop + 30, score, colour::kPrimary);

  // The clock, large and central — the thing you look up from across a room.
  char minute[12];
  snprintf(minute, sizeof(minute), "%u'", d.live.minute);
  drawStatBlock(tft, board::kScreenWidth / 2, kContentTop + 108, "MINUTE",
                minute, colour::kAccent);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  const char* stateText =
      f.state == model::MatchState::Paused ? "HALF TIME" : "IN PLAY";
  tft.drawString(stateText, board::kScreenWidth / 2, kContentBottom - 4, 2);
}

// ---------------------------------------------------------------------------
// Season record
// ---------------------------------------------------------------------------

void SeasonRecordScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::TableRow* us = d.ourTeam();
  if (us == nullptr) return;  // hasData() guards this, but be explicit.

  char name[model::kNameLen];
  shortenClubName(us->name, name, sizeof(name));
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(colour::kOurTeam, colour::kBackground);
  tft.drawString(name, board::kScreenWidth / 2, kContentTop + 4, 4);

  // W / D / L across the middle, colour-coded consistently with results.
  const int16_t cy = kContentTop + 74;
  const int16_t third = board::kScreenWidth / 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", us->won);
  drawStatBlock(tft, third / 2 + 4, cy, "WON", buf, colour::kWin);
  snprintf(buf, sizeof(buf), "%u", us->drawn);
  drawStatBlock(tft, board::kScreenWidth / 2, cy, "DRAWN", buf, colour::kDraw);
  snprintf(buf, sizeof(buf), "%u", us->lost);
  drawStatBlock(tft, board::kScreenWidth - third / 2 - 4, cy, "LOST", buf,
                colour::kLoss);

  // Win rate: the "rate" the brief asks for, which raw counts do not give.
  tft.setTextDatum(TC_DATUM);
  char summary[52];
  const uint8_t winPct =
      us->played > 0 ? static_cast<uint8_t>((us->won * 100U) / us->played) : 0;
  snprintf(summary, sizeof(summary), "%u played  -  %u%% win rate", us->played,
           winPct);
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  tft.drawString(summary, board::kScreenWidth / 2, kContentTop + 122, 2);

  char standing[52];
  snprintf(standing, sizeof(standing), "%u pts  -  %uth  -  GD %+d",
           us->points, us->position, us->goalDifference);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(standing, board::kScreenWidth / 2, kContentTop + 144, 2);
}

// ---------------------------------------------------------------------------
// Last result
// ---------------------------------------------------------------------------

void LastResultScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::Fixture& f = d.lastResult;

  char score[8];
  formatScore(f, score, sizeof(score));
  drawFixtureHeadline(tft, f, kContentTop + 34, score, resultColour(f));

  // Verdict, in the same colour language as the scoreline.
  const int8_t ours   = f.weAreHome ? f.homeGoals : f.awayGoals;
  const int8_t theirs = f.weAreHome ? f.awayGoals : f.homeGoals;
  const char* verdict = ours > theirs ? "WON" : (ours < theirs ? "LOST" : "DREW");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(resultColour(f), colour::kBackground);
  tft.drawString(verdict, board::kScreenWidth / 2, kContentTop + 84, 4);

  char when[40];
  formatKickoff(f.kickoffUtc, when, sizeof(when));
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  tft.drawString(when, board::kScreenWidth / 2, kContentTop + 120, 2);

  char detail[52];
  snprintf(detail, sizeof(detail), "%s  -  matchday %u", f.competition,
           f.matchday);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(detail, board::kScreenWidth / 2, kContentTop + 144, 2);
}

// ---------------------------------------------------------------------------
// Next fixture
// ---------------------------------------------------------------------------

void NextFixtureScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::Fixture& f = d.nextFixture;

  drawFixtureHeadline(tft, f, kContentTop + 34, "v", colour::kMuted);

  char countdown[24];
  formatCountdown(f.kickoffUtc, countdown, sizeof(countdown));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kAccent, colour::kBackground);
  tft.drawString(countdown, board::kScreenWidth / 2, kContentTop + 84, 4);

  char when[40];
  formatKickoff(f.kickoffUtc, when, sizeof(when));
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  tft.drawString(when, board::kScreenWidth / 2, kContentTop + 120, 2);

  char detail[52];
  snprintf(detail, sizeof(detail), "%s  -  %s", f.competition,
           f.weAreHome ? "HOME" : "AWAY");
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(detail, board::kScreenWidth / 2, kContentTop + 144, 2);
}

// ---------------------------------------------------------------------------
// League table
// ---------------------------------------------------------------------------

uint8_t LeagueTableScreen::visibleRows() {
  return static_cast<uint8_t>((kContentHeight - kHeaderRowGap) / kRowHeight);
}

void LeagueTableScreen::onShow(const model::Snapshot& d) {
  rowCount_ = d.tableRows;

  // Open centred on our team. Bolton sit 22nd of 24, so a table that opened at
  // the top would show a Bolton fan nothing they came for.
  const uint8_t visible = visibleRows();
  if (d.ourRow < d.tableRows && d.tableRows > visible) {
    const int16_t centred = static_cast<int16_t>(d.ourRow) - visible / 2;
    const int16_t maxScroll = static_cast<int16_t>(d.tableRows) - visible;
    scroll_ = static_cast<uint8_t>(constrain(centred, 0, maxScroll));
  } else {
    scroll_ = 0;
  }
}

bool LeagueTableScreen::handleGesture(touch::Gesture g) {
  const uint8_t visible = visibleRows();
  if (rowCount_ <= visible) return false;  // Nothing to scroll; let it pass.

  const uint8_t maxScroll = rowCount_ - visible;
  // Scroll by most of a page, keeping a row of overlap so the reader has an
  // anchor rather than losing their place entirely.
  const uint8_t step = visible > 1 ? visible - 1 : 1;

  switch (g) {
    case touch::Gesture::SwipeUp:
      if (scroll_ >= maxScroll) return false;  // At the end: don't swallow it.
      scroll_ = static_cast<uint8_t>(min<int16_t>(scroll_ + step, maxScroll));
      return true;
    case touch::Gesture::SwipeDown:
      if (scroll_ == 0) return false;
      scroll_ = static_cast<uint8_t>(max<int16_t>(scroll_ - step, 0));
      return true;
    default:
      return false;
  }
}

void LeagueTableScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  // Column right-edges, tuned against the real club codes and two-digit
  // figures rather than guessed. Everything is right-aligned so the digits
  // line up down the table, which is what makes a table scannable.
  struct Column { const char* label; int16_t right; };
  constexpr Column kCols[] = {
      {"MP",  148}, {"W", 175}, {"D", 199}, {"L", 223},
      {"GF",  251}, {"GA", 279}, {"GD", 310},
  };
  constexpr int16_t kPosRight = 26;
  constexpr int16_t kTlaLeft  = 32;

  // Header row.
  const int16_t headerY = kContentTop + 8;
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("#", kPosRight, headerY, 2);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("CLUB", kTlaLeft, headerY, 2);
  tft.setTextDatum(MR_DATUM);
  for (const Column& c : kCols) tft.drawString(c.label, c.right, headerY, 2);
  tft.drawFastHLine(0, kContentTop + kHeaderRowGap - 2, board::kScreenWidth,
                    colour::kMuted);

  // Rows.
  const uint8_t visible = visibleRows();
  const uint8_t last = min<uint8_t>(scroll_ + visible, d.tableRows);
  for (uint8_t i = scroll_; i < last; ++i) {
    const model::TableRow& r = d.table[i];
    const int16_t rowTop =
        kContentTop + kHeaderRowGap + (i - scroll_) * kRowHeight;
    const int16_t cy = rowTop + kRowHeight / 2;

    // Banding aids horizontal tracking across eight numeric columns; our own
    // row gets a solid highlight instead.
    if (r.isOurTeam) {
      tft.fillRect(0, rowTop, board::kScreenWidth, kRowHeight, 0x3800);
    } else if ((i & 1) == 0) {
      tft.fillRect(0, rowTop, board::kScreenWidth, kRowHeight, colour::kRowAlt);
    }

    const uint16_t fg = r.isOurTeam ? colour::kOurTeam : colour::kPrimary;
    const uint16_t bg = r.isOurTeam ? 0x3800
                                    : ((i & 1) == 0 ? colour::kRowAlt
                                                    : colour::kBackground);
    tft.setTextColor(fg, bg);

    char buf[8];
    tft.setTextDatum(MR_DATUM);
    snprintf(buf, sizeof(buf), "%u", r.position);
    tft.drawString(buf, kPosRight, cy, 2);

    // The official three-letter code, so the numeric columns stay aligned.
    // Supplied by the provider — we deliberately do not invent abbreviations.
    tft.setTextDatum(ML_DATUM);
    tft.drawString(r.tla, kTlaLeft, cy, 2);

    tft.setTextDatum(MR_DATUM);
    const int16_t values[] = {r.played, r.won,          r.drawn,
                              r.lost,   r.goalsFor,     r.goalsAgainst};
    for (uint8_t c = 0; c < 6; ++c) {
      snprintf(buf, sizeof(buf), "%d", values[c]);
      tft.drawString(buf, kCols[c].right, cy, 2);
    }
    // Goal difference carries a sign, which matters at a glance.
    snprintf(buf, sizeof(buf), "%+d", r.goalDifference);
    tft.drawString(buf, kCols[6].right, cy, 2);
  }

  // Points are the column that matters most, so they get their own emphasis
  // rather than competing with six other numbers... but there is no room at
  // this row height for a tenth column, so the full name and points of the
  // highlighted row are shown in the footer strip instead.
  const model::TableRow* us = d.ourTeam();
  if (us != nullptr) {
    char line[64];
    char name[model::kNameLen];
    shortenClubName(us->name, name, sizeof(name));
    snprintf(line, sizeof(line), "%s  -  %u pts", name, us->points);
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(colour::kOurTeam, colour::kBackground);
    tft.drawString(line, 4, kContentBottom - 2, 2);
  }

  // Scroll position, so it is clear more rows exist off-screen.
  if (d.tableRows > visible) {
    char pos[16];
    snprintf(pos, sizeof(pos), "%u-%u/%u", scroll_ + 1, last, d.tableRows);
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(pos, board::kScreenWidth - 4, kContentBottom - 2, 2);
  }
}

// ---------------------------------------------------------------------------
// Top scorers
// ---------------------------------------------------------------------------

void TopScorerScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("PLAYER", 6, kContentTop + 4, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("GLS", board::kScreenWidth - 6, kContentTop + 4, 2);
  tft.drawFastHLine(0, kContentTop + 22, board::kScreenWidth, colour::kMuted);

  constexpr int16_t kRowHeight = 34;
  for (uint8_t i = 0; i < d.scorerCount; ++i) {
    const model::Scorer& s = d.scorers[i];
    const int16_t rowTop = kContentTop + 26 + i * kRowHeight;
    if (rowTop + kRowHeight > kContentBottom) break;
    const int16_t cy = rowTop + kRowHeight / 2 - 4;

    // Rank, then name, then goals in a large face on the right.
    char rank[6];
    snprintf(rank, sizeof(rank), "%u", i + 1);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(rank, 6, cy, 4);

    tft.setTextColor(colour::kPrimary, colour::kBackground);
    tft.drawString(s.name, 28, cy - 6, 2);

    char sub[32];
    snprintf(sub, sizeof(sub), "%s  -  %u apps", s.tla, s.played);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(sub, 28, cy + 10, 2);

    char goals[6];
    snprintf(goals, sizeof(goals), "%u", s.goals);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(colour::kAccent, colour::kBackground);
    tft.drawString(goals, board::kScreenWidth - 8, cy, 7);
  }
}

}  // namespace ui
