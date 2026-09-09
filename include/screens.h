/**
 * @file screens.h
 * @brief The concrete screens. One class per screen of information.
 */

#pragma once

#include "screen.h"

namespace ui {

/// Live score, clock and competition while a match is in progress.
class LiveMatchScreen : public Screen {
 public:
  const char* title() const override { return "Live"; }
  bool hasData(const model::Snapshot& d) const override { return d.liveActive; }
  void draw(TFT_eSPI& tft, const model::Snapshot& d) override;
  /// The match clock moves, so this screen asks to be refreshed.
  bool needsRedraw() const override { return true; }
};

/// Season win/draw/loss record, derived from our row in the league table.
class SeasonRecordScreen : public Screen {
 public:
  const char* title() const override { return "Season"; }
  bool hasData(const model::Snapshot& d) const override {
    return d.ourTeam() != nullptr;
  }
  void draw(TFT_eSPI& tft, const model::Snapshot& d) override;
};

/// The most recent completed fixture.
class LastResultScreen : public Screen {
 public:
  const char* title() const override { return "Last"; }
  bool hasData(const model::Snapshot& d) const override {
    return d.lastResult.valid;
  }
  void draw(TFT_eSPI& tft, const model::Snapshot& d) override;
};

/// The next scheduled fixture, with a live countdown.
class NextFixtureScreen : public Screen {
 public:
  const char* title() const override { return "Next"; }
  bool hasData(const model::Snapshot& d) const override {
    return d.nextFixture.valid;
  }
  void draw(TFT_eSPI& tft, const model::Snapshot& d) override;
  /// The countdown ticks.
  bool needsRedraw() const override { return true; }
};

/// The full league table, scrollable.
class LeagueTableScreen : public Screen {
 public:
  const char* title() const override { return "Table"; }
  bool hasData(const model::Snapshot& d) const override {
    return d.tableRows > 0;
  }
  void draw(TFT_eSPI& tft, const model::Snapshot& d) override;
  /// Takes vertical swipes for scrolling; leaves horizontal ones to navigate.
  bool handleGesture(touch::Gesture g) override;
  /// Opens centred on our team rather than at the top of the table.
  void onShow(const model::Snapshot& d) override;

  static constexpr uint8_t kRowHeight    = 19;
  /// Vertical space taken by the column-header row.
  static constexpr uint8_t kHeaderRowGap = 18;
  /**
   * Space reserved at the bottom for the summary line.
   *
   * Explicitly reserved because it was not, originally: rows filled the
   * content area down to y=215 while the summary's baseline sat at 216 and its
   * glyphs extended upward, overlapping the final row.
   */
  static constexpr uint8_t kSummaryStrip = 18;

  /// Rows visible at once, given the row height and the reserved strips.
  static uint8_t visibleRows();

 private:
  uint8_t scroll_    = 0;
  uint8_t rowCount_  = 0;
};

/**
 * Top scorers — our own team's by preference, the league's as a fallback.
 *
 * Our team's scorers are the more interesting view: the league chart is the
 * same for everyone and rarely features a side in the bottom half. The league
 * leader is still shown as a single line of context, since both lists come
 * from one request and it costs nothing to display.
 */
class TopScorerScreen : public Screen {
 public:
  const char* title() const override { return "Scorers"; }
  bool hasData(const model::Snapshot& d) const override {
    return d.teamScorerCount > 0 || d.leagueScorerCount > 0;
  }
  void draw(TFT_eSPI& tft, const model::Snapshot& d) override;
};

}  // namespace ui
