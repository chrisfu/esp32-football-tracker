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
  return static_cast<uint8_t>((kContentHeight - kHeaderRowGap - kSummaryStrip) /
                              kRowHeight);
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
  // Column right-edges. All nine columns the brief asks for fit within 320 px:
  // the numbers need about 238 px of glyphs, so an earlier claim that there
  // was "no room for a tenth column" was simply a bad layout, not a real
  // constraint. Everything is right-aligned so digits line up down the table,
  // which is what makes a table scannable.
  struct Column { const char* label; int16_t right; };
  constexpr Column kCols[] = {
      {"MP", 92}, {"W", 116}, {"D", 138}, {"L", 160},
      {"GF", 190}, {"GA", 218}, {"GD", 252},
  };
  constexpr int16_t kPosRight  = 20;
  constexpr int16_t kTlaLeft   = 26;
  /// Points sits hard right, separated, and drawn bold — it is the figure a
  /// reader looks for first, and every other source presents it that way.
  constexpr int16_t kPtsRight  = 314;
  constexpr int16_t kPtsDivide = 264;

  const int16_t headerY = kContentTop + 8;
  const int16_t rowsTop = kContentTop + kHeaderRowGap;

  // --- Header ------------------------------------------------------------
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("#", kPosRight, headerY, 2);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("CLUB", kTlaLeft, headerY, 2);
  tft.setTextDatum(MR_DATUM);
  for (const Column& c : kCols) tft.drawString(c.label, c.right, headerY, 2);
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  drawBoldString(tft, "PTS", kPtsRight, headerY, 2);
  tft.drawFastHLine(0, rowsTop - 2, board::kScreenWidth, colour::kMuted);

  // --- Rows --------------------------------------------------------------
  const uint8_t visible = visibleRows();
  const uint8_t last = min<uint8_t>(scroll_ + visible, d.tableRows);
  for (uint8_t i = scroll_; i < last; ++i) {
    const model::TableRow& r = d.table[i];
    const int16_t rowTop = rowsTop + (i - scroll_) * kRowHeight;
    const int16_t cy = rowTop + kRowHeight / 2;

    // Banding aids horizontal tracking across eight numeric columns; our own
    // row gets a solid highlight instead.
    const bool banded = ((i & 1) == 0);
    const uint16_t bg = r.isOurTeam ? 0x3800
                                    : (banded ? colour::kRowAlt
                                              : colour::kBackground);
    if (r.isOurTeam || banded) {
      tft.fillRect(0, rowTop, board::kScreenWidth, kRowHeight, bg);
    }

    // Numeric columns are drawn slightly muted so the points column carries
    // the emphasis rather than competing with seven other figures.
    const uint16_t nameFg = r.isOurTeam ? colour::kOurTeam : colour::kPrimary;
    const uint16_t numFg  = r.isOurTeam ? colour::kOurTeam : 0xC618;

    char buf[8];
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(numFg, bg);
    snprintf(buf, sizeof(buf), "%u", r.position);
    tft.drawString(buf, kPosRight, cy, 2);

    // The official three-letter code from the provider. We deliberately do not
    // invent abbreviations of our own.
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(nameFg, bg);
    tft.drawString(r.tla, kTlaLeft, cy, 2);

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(numFg, bg);
    const int16_t values[] = {r.played, r.won,      r.drawn,
                              r.lost,   r.goalsFor, r.goalsAgainst};
    for (uint8_t c = 0; c < 6; ++c) {
      snprintf(buf, sizeof(buf), "%d", values[c]);
      tft.drawString(buf, kCols[c].right, cy, 2);
    }
    // Goal difference carries a sign, which matters at a glance.
    snprintf(buf, sizeof(buf), "%+d", r.goalDifference);
    tft.drawString(buf, kCols[6].right, cy, 2);

    // Points: bold, full brightness, and set apart by the divider.
    tft.setTextColor(r.isOurTeam ? colour::kOurTeam : colour::kPrimary, bg);
    snprintf(buf, sizeof(buf), "%u", r.points);
    drawBoldString(tft, buf, kPtsRight, cy, 2);
  }

  // Divider ahead of the points column, spanning only the rows drawn so it
  // does not run into the summary strip.
  tft.drawFastVLine(kPtsDivide, rowsTop, (last - scroll_) * kRowHeight,
                    colour::kMuted);

  // --- Summary strip -----------------------------------------------------
  // Drawn in space reserved by kSummaryStrip. The three-letter codes keep the
  // table aligned but are terse, so our team's full name is spelled out here.
  const int16_t summaryY = kContentBottom - kSummaryStrip / 2;
  const model::TableRow* us = d.ourTeam();
  if (us != nullptr) {
    char name[model::kNameLen];
    shortenClubName(us->name, name, sizeof(name));
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(colour::kOurTeam, colour::kBackground);
    tft.drawString(name, 4, summaryY, 2);
  }

  // Scroll position, so it is clear more rows exist off-screen.
  if (d.tableRows > visible) {
    char pos[16];
    snprintf(pos, sizeof(pos), "%u-%u of %u", scroll_ + 1, last, d.tableRows);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(pos, board::kScreenWidth - 4, summaryY, 2);
  }
}

// ---------------------------------------------------------------------------
// Top scorers
// ---------------------------------------------------------------------------

void TopScorerScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  // Prefer our own team's scorers. The league chart is identical for every
  // user of every device, and a side in the bottom half never appears in it.
  const bool showingTeam = d.teamScorerCount > 0;
  const model::Scorer* list =
      showingTeam ? d.teamScorers : d.leagueScorers;
  const uint8_t count =
      showingTeam ? d.teamScorerCount : d.leagueScorerCount;

  // Say whose list this is, so the numbers are never ambiguous — two goals
  // makes sense as a team-leading tally and nonsense as a league-leading one.
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(showingTeam ? colour::kOurTeam : colour::kPrimary,
                   colour::kBackground);
  tft.drawString(showingTeam ? "OUR SCORERS" : "LEAGUE SCORERS", 6,
                 kContentTop + 4, 2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("GLS", board::kScreenWidth - 8, kContentTop + 4, 2);
  tft.drawFastHLine(0, kContentTop + 22, board::kScreenWidth, colour::kMuted);

  // Reserve a strip at the bottom for the league-leader context line, so rows
  // cannot grow into it.
  constexpr int16_t kContextStrip = 20;
  constexpr int16_t kRowHeight    = 32;
  const int16_t rowsTop    = kContentTop + 26;
  const int16_t rowsBottom = kContentBottom - kContextStrip;

  for (uint8_t i = 0; i < count; ++i) {
    const model::Scorer& sc = list[i];
    const int16_t rowTop = rowsTop + i * kRowHeight;
    if (rowTop + kRowHeight > rowsBottom) break;  // Out of room; stop cleanly.
    const int16_t cy = rowTop + kRowHeight / 2 - 2;

    char rank[6];
    snprintf(rank, sizeof(rank), "%u", i + 1);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(rank, 6, cy, 4);

    tft.setTextColor(colour::kPrimary, colour::kBackground);
    tft.drawString(sc.name, 28, cy - 6, 2);

    // When showing our own team the club code is the same on every row, so it
    // is dropped in favour of the appearance count alone.
    char sub[32];
    if (showingTeam) {
      snprintf(sub, sizeof(sub), "%u appearances", sc.played);
    } else {
      snprintf(sub, sizeof(sub), "%s  -  %u apps", sc.tla, sc.played);
    }
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(sub, 28, cy + 10, 2);

    char goals[6];
    snprintf(goals, sizeof(goals), "%u", sc.goals);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(colour::kAccent, colour::kBackground);
    tft.drawString(goals, board::kScreenWidth - 8, cy, 7);
  }

  // League leader as context. Only worth showing alongside our own list; it
  // would be a redundant restatement of row one otherwise.
  if (showingTeam && d.leagueScorerCount > 0) {
    const model::Scorer& top = d.leagueScorers[0];
    char line[64];
    snprintf(line, sizeof(line), "League: %s (%s) %u", top.name, top.tla,
             top.goals);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(line, 6, kContentBottom - kContextStrip / 2, 2);
  }
}

}  // namespace ui
