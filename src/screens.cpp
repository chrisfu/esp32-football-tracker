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

#include "crest_cache.h"

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

// ---------------------------------------------------------------------------
// Fixture layout
// ---------------------------------------------------------------------------
//
// Each club gets a column centre, and everything belonging to that club is
// centred on it: crest, name, form. Previously the crests sat at fixed insets
// while the names were edge-aligned against the scoreline, so a name's centre
// moved with its length and could never line up with the crest above it —
// "Watford" and "Wolverhampton Wanderers" landed in visibly different places.

// Column centres and the centre guard are derived from measurement, not
// chosen for tidiness. Font 2 widths on this display, and Font 4 for the
// score:
//
//   Bolton Wanderers         109 px      score "2-3"   36 px
//   Queens Park Rangers      129 px      "v"           12 px
//   Preston North End        115 px
//   West Ham United          101 px
//   Wolverhampton Wanderers  161 px
//
// The first version used quarter-point columns (80 / 240) and a 26 px guard,
// which allowed 108 px of name — one pixel short of "Bolton Wanderers", so it
// was truncated to "Bolton Wanderer." for the sake of a single pixel. The
// guard was also reserving 52 px for a 36 px score.
//
// Both are now sized from the measurements: a 22 px guard leaves 44 px clear
// for the score, and the columns sit where the inward and outward limits meet,
// which is what maximises symmetric width.
constexpr int16_t kCentreGuard = 22;
/// (kScreenWidth/2 - kCentreGuard + 2) / 2 — the point where the inward limit
/// and the outward limit are equal, giving the widest centred column.
constexpr int16_t kHomeColumnX = 70;
constexpr int16_t kAwayColumnX = board::kScreenWidth - kHomeColumnX;  // 250

/**
 * Width a club's text may occupy without reaching the scoreline.
 *
 * The bound differs by side: the left column's inward limit is
 * centre − guard, the right column's is centre + guard. Treating both the
 * same gave the away column 212 px against the home column's 108 — which
 * would have let away names run twice as wide and reintroduced exactly the
 * imbalance this layout exists to remove.
 */
int16_t columnTextWidth(int16_t columnX) {
  constexpr int16_t centre = board::kScreenWidth / 2;
  const bool leftSide = columnX < centre;

  const int16_t innerBound = leftSide ? (centre - kCentreGuard)
                                      : (centre + kCentreGuard);
  const int16_t inward  = static_cast<int16_t>(abs(innerBound - columnX));
  const int16_t outward = leftSide
                              ? static_cast<int16_t>(columnX - 2)
                              : static_cast<int16_t>(board::kScreenWidth - 2 -
                                                     columnX);
  // The narrower side governs, so the text stays centred on the column.
  return static_cast<int16_t>(2 * min<int16_t>(inward, outward));
}

/**
 * Shorten a club name until it fits, measuring rather than guessing.
 *
 * Character counts do not work here: Font 2 is proportional, so "Millwall" and
 * "Wolverhampton" differ by more than their letter count suggests. textWidth()
 * asks the font.
 */
void fitClubName(TFT_eSPI& tft, const char* in, char* out, size_t outLen,
                 int16_t maxPixels, uint8_t font) {
  shortenClubName(in, out, outLen);
  if (tft.textWidth(out, font) <= maxPixels) return;

  // Trim from the end, marking the cut with two dots rather than one.
  //
  // A single full stop was mistaken for a clipped glyph — at this size the
  // bottom of an "s" and a period look much alike, so "Bolton Wanderer."
  // read as a rendering fault rather than as deliberate shortening. Two dots
  // are unambiguous.
  size_t len = strlen(out);
  while (len > 1) {
    out[--len] = '\0';
    // Trailing spaces would leave the marker floating away from the text.
    while (len > 1 && out[len - 1] == ' ') out[--len] = '\0';

    char probe[model::kNameLen];
    // Built in a separate buffer and copied back: snprintf(out, ..., "%s..",
    // out) reads and writes the same storage, which is undefined behaviour
    // even though it usually appears to work.
    snprintf(probe, sizeof(probe), "%s..", out);
    if (tft.textWidth(probe, font) <= maxPixels) {
      strncpy(out, probe, outLen - 1);
      out[outLen - 1] = '\0';
      return;
    }
  }
}

/**
 * Draw both clubs' crests, each centred on its column.
 *
 * @return true if at least one was drawn, so the caller knows whether to
 *         reserve the vertical space. Falling back cleanly matters: a crest
 *         may legitimately be missing while it is still downloading, and the
 *         screen must not leave a hole waiting for it.
 */
bool drawFixtureCrests(TFT_eSPI& tft, const model::Fixture& f, int16_t y) {
  constexpr int16_t half = crest::kSize / 2;
  bool any = false;
  if (crest::draw(tft, f.homeId, kHomeColumnX - half, y)) any = true;
  if (crest::draw(tft, f.awayId, kAwayColumnX - half, y)) any = true;
  return any;
}

// ---------------------------------------------------------------------------
// Scrolling names
// ---------------------------------------------------------------------------
//
// A few club names are simply wider than any column this screen can offer —
// "Wolverhampton Wanderers" measures 161 px against 136 px available — so
// rather than abbreviate them into something ambiguous, they scroll.
//
// Only names that genuinely overflow move; everything else is drawn static,
// which is most of them. Scrolling stops when the backlight dims, because the
// manager stops calling animate() there.

constexpr int16_t kNameHeight = 18;  ///< Font 2 is 16 px; 18 gives margin.

/// Phase timings. Long enough at each end to read the name without waiting.
constexpr uint32_t kMarqueeHoldStartMs = 1800;
constexpr uint32_t kMarqueeHoldEndMs   = 1400;
constexpr uint32_t kMarqueePxPerSecond = 22;

/**
 * Sprite used to clip scrolling text, sized to exactly the width requested.
 *
 * TFT_eSPI has no arbitrary clip region, so the text is drawn into a sprite
 * the width of the column and pushed as a block — which is also why the
 * animation touches no pixels outside its own strip.
 *
 * **The width must match what the caller pushes.** pushSprite() writes the
 * whole sprite, so a sprite wider than the column paints its surplus
 * background over whatever sits beyond it. That is not hypothetical: the
 * sprite was previously created at the full column width while the caller
 * positioned it as though it were `maxWidth` wide, and on the live match
 * screen — the one panel where the names share a row with the score — the
 * spill covered the first digit with a black block.
 *
 * Kept between calls rather than created per frame: at 136x18x2 it is about
 * 4.9 KB, and allocating and freeing that many times a second would churn a
 * heap whose largest block is 110 KB. Recreated only if the width changes,
 * which in practice happens once.
 */
TFT_eSprite& nameSprite(TFT_eSPI& tft, int16_t width) {
  static TFT_eSprite sprite(&tft);
  static int16_t createdWidth = 0;

  if (createdWidth != width) {
    if (createdWidth != 0) sprite.deleteSprite();
    sprite.setColorDepth(16);
    createdWidth = sprite.createSprite(width, kNameHeight) ? width : 0;
    if (createdWidth == 0) {
      Serial.printf("[ui] name sprite %dx%d allocation failed\n", width,
                    kNameHeight);
    }
  }
  return sprite;
}

/// Horizontal offset for a marquee, derived from the clock so no state is kept.
int16_t marqueeOffset(int16_t overflowPx) {
  if (overflowPx <= 0) return 0;
  const uint32_t travelMs =
      (static_cast<uint32_t>(overflowPx) * 1000) / kMarqueePxPerSecond;
  const uint32_t period =
      kMarqueeHoldStartMs + travelMs + kMarqueeHoldEndMs;
  const uint32_t t = millis() % period;

  if (t < kMarqueeHoldStartMs) return 0;
  if (t < kMarqueeHoldStartMs + travelMs) {
    const uint32_t elapsed = t - kMarqueeHoldStartMs;
    return static_cast<int16_t>((static_cast<uint32_t>(overflowPx) * elapsed) /
                                travelMs);
  }
  return overflowPx;  // Held at the end before looping back.
}

/**
 * Draw one club name in its column, scrolling if it does not fit.
 *
 * @return true if the name is scrolling, so the caller knows the strip needs
 *         animating rather than being left alone.
 */
bool drawColumnName(TFT_eSPI& tft, const char* rawName, int16_t columnX,
                    int16_t y, uint16_t fg) {
  char name[model::kNameLen];
  shortenClubName(rawName, name, sizeof(name));

  // MARQUEE_SQUEEZE narrows the column at build time so the scrolling path can
  // be exercised with the club names actually on screen. Without it the
  // current fixture may well be two short names, and the animation would ship
  // untested.
#ifdef MARQUEE_SQUEEZE
  const int16_t maxWidth = columnTextWidth(columnX) - MARQUEE_SQUEEZE;
#else
  const int16_t maxWidth = columnTextWidth(columnX);
#endif
  const int16_t textPx   = tft.textWidth(name, 2);

  if (textPx <= maxWidth) {
    // Fits: drawn directly, no sprite and no animation.
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, colour::kBackground);
    tft.drawString(name, columnX, y, 2);
    return false;
  }

  TFT_eSprite& sprite = nameSprite(tft, maxWidth);
  // Only an exact match is safe: narrower would crop the text, wider would
  // paint background over the neighbouring element.
  if (sprite.width() != maxWidth) {
    // Sprite unavailable: fall back to the static truncated form rather than
    // showing nothing.
    fitClubName(tft, rawName, name, sizeof(name), maxWidth, 2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, colour::kBackground);
    tft.drawString(name, columnX, y, 2);
    return false;
  }

  // Logged once per name, per column.
  //
  // Keyed on the column and compared by *content*. An earlier version kept a
  // single `const char*` and compared pointers, which logged on every frame:
  // the two names alternate, so the pointer always differed from the last one
  // seen, and a once-per-name diagnostic became hundreds of lines a second.
  {
    static char lastLogged[2][model::kNameLen] = {{0}, {0}};
    const uint8_t slot = (columnX < board::kScreenWidth / 2) ? 0 : 1;
    if (strncmp(lastLogged[slot], name, model::kNameLen) != 0) {
      strncpy(lastLogged[slot], name, model::kNameLen - 1);
      lastLogged[slot][model::kNameLen - 1] = '\0';
      Serial.printf("[ui] scrolling \"%s\" (%d px in %d px column)\n", name,
                    textPx, maxWidth);
    }
  }

  sprite.fillSprite(colour::kBackground);
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextColor(fg, colour::kBackground);
  sprite.drawString(name, -marqueeOffset(textPx - maxWidth), 1, 2);
  sprite.pushSprite(columnX - maxWidth / 2, y - kNameHeight / 2);
  return true;
}

/// Draw both club names, centred on the same columns as their crests.
/// @return true if either name is scrolling.
bool drawFixtureNames(TFT_eSPI& tft, const model::Fixture& f, int16_t y) {
  const uint16_t homeFg = f.weAreHome ? colour::kOurTeam : colour::kPrimary;
  const uint16_t awayFg = f.weAreHome ? colour::kPrimary : colour::kOurTeam;
  // Both are drawn before the results are combined, so a scrolling name on
  // one side never short-circuits the other.
  const bool homeScrolls = drawColumnName(tft, f.homeName, kHomeColumnX, y,
                                          homeFg);
  const bool awayScrolls = drawColumnName(tft, f.awayName, kAwayColumnX, y,
                                          awayFg);
  return homeScrolls || awayScrolls;
}

/// Draw the centre element — a scoreline, or "v" for an unplayed fixture.
void drawFixtureCentre(TFT_eSPI& tft, int16_t y, const char* centre,
                       uint16_t colourOf) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colourOf, colour::kBackground);
  tft.drawString(centre, board::kScreenWidth / 2, y, 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// Live match
// ---------------------------------------------------------------------------

namespace {

/// Draw the marker for one event kind, centred on (cx, cy).
///
/// Shape and colour together rather than colour alone: a card is a rectangle
/// and a goal a circle, so the two remain distinguishable to someone who
/// cannot easily separate red from green.
void drawEventMarker(TFT_eSPI& tft, model::EventKind kind, int16_t cx,
                     int16_t cy) {
  switch (kind) {
    case model::EventKind::Goal:
      tft.fillCircle(cx, cy, 4, colour::kWin);
      break;
    case model::EventKind::Penalty:
      // A goal, with a mark in the middle to say it came from the spot.
      tft.fillCircle(cx, cy, 4, colour::kWin);
      tft.drawPixel(cx, cy, TFT_BLACK);
      tft.drawPixel(cx - 1, cy, TFT_BLACK);
      tft.drawPixel(cx + 1, cy, TFT_BLACK);
      break;
    case model::EventKind::OwnGoal:
      tft.fillCircle(cx, cy, 4, colour::kLoss);
      break;
    case model::EventKind::YellowCard:
      tft.fillRect(cx - 2, cy - 4, 5, 8, colour::kDraw);
      break;
    case model::EventKind::RedCard:
      tft.fillRect(cx - 2, cy - 4, 5, 8, colour::kLoss);
      break;
    default:
      tft.drawPixel(cx, cy, colour::kMuted);
      break;
  }
}

/// Suffix marking an event that needs a word of explanation.
const char* eventSuffix(model::EventKind kind) {
  switch (kind) {
    case model::EventKind::Penalty: return " (pen)";
    case model::EventKind::OwnGoal: return " (og)";
    default:                        return "";
  }
}

}  // namespace

void LiveMatchScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::LiveMatch& m = d.live;
  const model::Fixture&   f = m.fixture;

  // --- Scoreline ----------------------------------------------------------
  //
  // The same column centres as the other fixture screens, via the same
  // helpers. This screen was still edge-aligning its names against the score,
  // so a name's centre moved with its length — and it had no width fitting at
  // all, meaning a long name ran off the left edge rather than being trimmed
  // or scrolled. Names sit on the score's own row, so consistency costs no
  // vertical space here.
  const int16_t scoreY = kContentTop + 16;
  char score[12];
  formatScore(f, score, sizeof(score));
  drawFixtureCentre(tft, scoreY, score, colour::kPrimary);
  drawFixtureNames(tft, f, scoreY);
  namesY_ = scoreY;

  // --- Clock and state ----------------------------------------------------
  char clock[24];
  if (f.state == model::MatchState::Paused) {
    snprintf(clock, sizeof(clock), "HALF TIME");
  } else if (f.state == model::MatchState::Finished) {
    snprintf(clock, sizeof(clock), "FULL TIME");
  } else if (m.extra > 0) {
    snprintf(clock, sizeof(clock), "%u+%u'", m.minute, m.extra);
  } else {
    snprintf(clock, sizeof(clock), "%u'", m.minute);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kAccent, colour::kBackground);
  tft.drawString(clock, board::kScreenWidth / 2, kContentTop + 40, 2);

  // --- Event columns ------------------------------------------------------
  // Home left, away right, each in chronological order, so the shape of the
  // match reads without having to work out which side a line belongs to.
  constexpr int16_t kDividerX  = board::kScreenWidth / 2;
  constexpr int16_t kRowHeight = 14;
  const int16_t colTop = kContentTop + 54;

  tft.drawFastHLine(0, colTop - 6, board::kScreenWidth, colour::kMuted);
  tft.drawFastVLine(kDividerX, colTop - 4, kContentBottom - colTop + 2,
                    colour::kMuted);

  if (m.eventCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString("no goals or cards yet", board::kScreenWidth / 2,
                   colTop + 30, 2);
    return;
  }

  // Rows are allocated per column, so a busy half for one side does not push
  // the other side's events down the screen.
  int16_t nextRow[2] = {colTop, colTop};
  const int16_t maxY = kContentBottom - kRowHeight;

  for (uint8_t i = 0; i < m.eventCount; ++i) {
    const model::MatchEvent& e = m.events[i];
    const uint8_t col = e.home ? 0 : 1;
    if (nextRow[col] > maxY) continue;  // That column is full.

    const int16_t left = (col == 0) ? 2 : kDividerX + 4;
    const int16_t cy   = nextRow[col] + kRowHeight / 2;

    char minute[10];
    if (e.extra > 0) {
      snprintf(minute, sizeof(minute), "%u+%u", e.minute, e.extra);
    } else {
      snprintf(minute, sizeof(minute), "%u'", e.minute);
    }
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(minute, left + 26, cy, 1);

    drawEventMarker(tft, e.kind, left + 34, cy);

    char label[32];
    snprintf(label, sizeof(label), "%s%s", e.player, eventSuffix(e.kind));
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(colour::kPrimary, colour::kBackground);
    tft.drawString(label, left + 42, cy, 1);

    nextRow[col] += kRowHeight;
  }
}

bool LiveMatchScreen::animate(TFT_eSPI& tft, const model::Snapshot& d) {
  if (namesY_ == 0 || !d.liveActive) return false;
  // Without this the names would only shift when the once-a-second clock
  // refresh redrew them, which reads as stuttering rather than scrolling.
  return drawFixtureNames(tft, d.live.fixture, namesY_);
}

// ---------------------------------------------------------------------------
// Season record
// ---------------------------------------------------------------------------

void SeasonRecordScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::TableRow* us = d.ourTeam();
  if (us == nullptr) return;  // hasData() guards this, but be explicit.

  char name[model::kNameLen];
  shortenClubName(us->name, name, sizeof(name));

  // Our own crest, beside the name rather than above it — the name is the
  // wider element, so pairing them horizontally wastes less vertical space.
  const uint16_t ourId = d.nextFixture.valid
                             ? (d.nextFixture.weAreHome ? d.nextFixture.homeId
                                                        : d.nextFixture.awayId)
                             : 0;
  const bool haveCrest = crest::draw(tft, ourId, 8, kContentTop + 2);
  tft.setTextDatum(haveCrest ? ML_DATUM : TC_DATUM);
  tft.setTextColor(colour::kOurTeam, colour::kBackground);
  if (haveCrest) {
    tft.drawString(name, 8 + crest::kSize + 10, kContentTop + 26, 4);
  } else {
    tft.drawString(name, board::kScreenWidth / 2, kContentTop + 4, 4);
  }

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
  tft.drawString(standing, board::kScreenWidth / 2, kContentTop + 140, 2);

  // Our own form. Taken from whichever fixture we appear in, since form is
  // derived per club rather than stored per table row.
  const char* ourForm = nullptr;
  if (d.nextFixture.valid) {
    ourForm = d.nextFixture.weAreHome ? d.nextFixture.homeForm
                                      : d.nextFixture.awayForm;
  } else if (d.lastResult.valid) {
    ourForm = d.lastResult.weAreHome ? d.lastResult.homeForm
                                     : d.lastResult.awayForm;
  }
  if (ourForm != nullptr && ourForm[0] != '\0') {
    drawFormChips(tft, ourForm, board::kScreenWidth / 2, kContentTop + 168,
                  d.liveActive ? d.live.provisionalResult : 0);
  }
}

// ---------------------------------------------------------------------------
// Last result
// ---------------------------------------------------------------------------

void LastResultScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::Fixture& f = d.lastResult;

  // Crests on their own row with the score between them, then the names
  // directly beneath, centred on the same columns.
  const bool crests = drawFixtureCrests(tft, f, kContentTop + 4);
  const int16_t centreY = crests ? kContentTop + 4 + crest::kSize / 2
                                 : kContentTop + 20;
  const int16_t namesY  = crests ? kContentTop + 4 + crest::kSize + 12
                                 : kContentTop + 44;

  char score[8];
  formatScore(f, score, sizeof(score));
  drawFixtureCentre(tft, centreY, score, resultColour(f));
  drawFixtureNames(tft, f, namesY);
  namesY_ = namesY;

  // Verdict, in the same colour language as the scoreline.
  const int8_t ours   = f.weAreHome ? f.homeGoals : f.awayGoals;
  const int8_t theirs = f.weAreHome ? f.awayGoals : f.homeGoals;
  const char* verdict = ours > theirs ? "WON" : (ours < theirs ? "LOST" : "DREW");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(resultColour(f), colour::kBackground);
  tft.drawString(verdict, board::kScreenWidth / 2, namesY + 26, 4);

  char when[40];
  formatKickoff(f.kickoffUtc, when, sizeof(when));
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  tft.drawString(when, board::kScreenWidth / 2, kContentTop + 130, 2);

  char detail[52];
  snprintf(detail, sizeof(detail), "%s  -  Matchday %u", f.competition,
           f.matchday);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(detail, board::kScreenWidth / 2, kContentTop + 152, 2);
}

bool LastResultScreen::animate(TFT_eSPI& tft, const model::Snapshot& d) {
  if (namesY_ == 0 || !d.lastResult.valid) return false;
  return drawFixtureNames(tft, d.lastResult, namesY_);
}

// ---------------------------------------------------------------------------
// Next fixture
// ---------------------------------------------------------------------------

void NextFixtureScreen::draw(TFT_eSPI& tft, const model::Snapshot& d) {
  const model::Fixture& f = d.nextFixture;

  const bool crests = drawFixtureCrests(tft, f, kContentTop + 2);
  const int16_t centreY = crests ? kContentTop + 2 + crest::kSize / 2
                                 : kContentTop + 18;
  const int16_t namesY  = crests ? kContentTop + 2 + crest::kSize + 12
                                 : kContentTop + 42;
  drawFixtureCentre(tft, centreY, "v", colour::kMuted);
  drawFixtureNames(tft, f, namesY);
  namesY_ = namesY;

  // Form guides, one under each club, on the same side as its name. Only
  // labelled once, centrally, since two identical captions would be noise.
  const int16_t formY = namesY + 22;
  // The same column centres as the crests and names, not the quarter points.
  // These were left at 80/240 when the columns moved to 70/250, so the chips
  // sat 10 px off each name — and in opposite directions, making 20 px of
  // mismatch between the two sides. Referencing the shared constants means
  // they cannot drift apart again.
  const int16_t leftCx  = kHomeColumnX;
  const int16_t rightCx = kAwayColumnX;
  if (f.homeForm[0] != '\0' || f.awayForm[0] != '\0') {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString("FORM", board::kScreenWidth / 2, formY, 1);
    // A match in progress contributes a provisional chip to *our* form only —
    // we know how our own game is going, not the next opponent's.
    const char provisional = d.liveActive ? d.live.provisionalResult : 0;
    drawFormChips(tft, f.homeForm, leftCx, formY,
                  f.weAreHome ? provisional : 0);
    drawFormChips(tft, f.awayForm, rightCx, formY,
                  f.weAreHome ? 0 : provisional);
  }

  char countdown[24];
  formatCountdown(f.kickoffUtc, countdown, sizeof(countdown));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kAccent, colour::kBackground);
  tft.drawString(countdown, board::kScreenWidth / 2, formY + 30, 4);

  char when[40];
  formatKickoff(f.kickoffUtc, when, sizeof(when));
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  tft.drawString(when, board::kScreenWidth / 2, kContentTop + 136, 2);

  char detail[52];
  snprintf(detail, sizeof(detail), "%s  -  %s", f.competition,
           f.weAreHome ? "HOME" : "AWAY");
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString(detail, board::kScreenWidth / 2, kContentTop + 158, 2);
}

bool NextFixtureScreen::animate(TFT_eSPI& tft, const model::Snapshot& d) {
  if (namesY_ == 0 || !d.nextFixture.valid) return false;
  return drawFixtureNames(tft, d.nextFixture, namesY_);
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

    // Font 4 (26 px), not Font 7. Font 7 is the 48 px seven-segment face and
    // overflowed this 32 px row badly. It is right for a single hero figure —
    // the live match clock — and wrong for a list.
    char goals[6];
    snprintf(goals, sizeof(goals), "%u", sc.goals);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(colour::kAccent, colour::kBackground);
    tft.drawString(goals, board::kScreenWidth - 10, cy, 4);
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

// ---------------------------------------------------------------------------
// Setup and status screens
// ---------------------------------------------------------------------------

void drawSetupScreen(TFT_eSPI& tft, const char* ssid, const char* password,
                     const char* url) {
  tft.fillScreen(colour::kBackground);

  tft.fillRect(0, 0, board::kScreenWidth, 30, colour::kHeaderBg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kAccent, colour::kHeaderBg);
  tft.drawString("SETUP REQUIRED", board::kScreenWidth / 2, 15, 4);

  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("Join this Wi-Fi network:", board::kScreenWidth / 2, 46, 2);

  // The three values a user needs, each labelled and given room. Font 4 for
  // the values so they are readable from arm's length while typing them into
  // a phone.
  const struct { const char* label; const char* value; uint16_t colour; }
      fields[] = {
          {"NETWORK",  ssid,     colour::kOurTeam},
          {"PASSWORD", password, colour::kOurTeam},
          {"THEN OPEN", url,     colour::kAccent},
      };

  int16_t y = 68;
  for (const auto& f : fields) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(f.label, board::kScreenWidth / 2, y, 1);
    tft.setTextColor(f.colour, colour::kBackground);
    tft.drawString(f.value, board::kScreenWidth / 2, y + 12, 4);
    y += 48;
  }

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("a setup page should open automatically",
                 board::kScreenWidth / 2, board::kScreenHeight - 4, 1);
}

void drawStatusScreen(TFT_eSPI& tft, const char* heading, const char* detail,
                      uint16_t headingColour) {
  tft.fillScreen(colour::kBackground);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(headingColour, colour::kBackground);
  tft.drawString(heading, board::kScreenWidth / 2,
                 board::kScreenHeight / 2 - 12, 4);
  if (detail != nullptr && detail[0] != '\0') {
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(detail, board::kScreenWidth / 2,
                   board::kScreenHeight / 2 + 18, 2);
  }
}

}  // namespace ui
