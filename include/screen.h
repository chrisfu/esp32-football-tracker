/**
 * @file screen.h
 * @brief The Screen interface and the shared layout the chrome reserves.
 */

#pragma once

#include <TFT_eSPI.h>

#include "board_config.h"
#include "model.h"
#include "touch_input.h"

namespace ui {

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
//
// The manager owns a title bar and a footer; screens own everything between.
// Publishing the bounds here means a screen never has to guess, and the chrome
// can be re-sized in one place.

constexpr int16_t kHeaderHeight = 26;
constexpr int16_t kFooterHeight = 22;
constexpr int16_t kContentTop    = kHeaderHeight;
constexpr int16_t kContentBottom = board::kScreenHeight - kFooterHeight;
constexpr int16_t kContentHeight = kContentBottom - kContentTop;
constexpr int16_t kContentWidth  = board::kScreenWidth;

/// Shared palette. Defined once so screens look like one application.
namespace colour {
constexpr uint16_t kBackground = TFT_BLACK;
constexpr uint16_t kHeaderBg   = 0x0010;  // Very dark blue.
constexpr uint16_t kFooterBg   = 0x0010;
constexpr uint16_t kPrimary    = TFT_WHITE;
constexpr uint16_t kMuted      = 0x8410;  // Mid grey.
constexpr uint16_t kAccent     = TFT_CYAN;
constexpr uint16_t kOurTeam    = TFT_YELLOW;
constexpr uint16_t kWin        = 0x07E0;  // Green.
constexpr uint16_t kDraw       = 0xFDA0;  // Amber.
constexpr uint16_t kLoss       = 0xF800;  // Red.
constexpr uint16_t kRowAlt     = 0x1082;  // Subtle row banding.
}  // namespace colour

/**
 * One screen of information.
 *
 * Screens are stateless with respect to data: they read from a const Snapshot
 * they do not own, so a screen cannot mutate what another screen displays.
 * Only the refresh scheduler writes to the snapshot.
 */
class Screen {
 public:
  virtual ~Screen() = default;

  /// Shown in the title bar.
  virtual const char* title() const = 0;

  /**
   * Whether this screen has anything worth showing.
   *
   * Screens returning false are skipped by the rotation rather than displayed
   * empty — which is how the live-match screen stays out of the way when no
   * match is on, and how the deferred injuries screen will behave until a
   * provider can supply it.
   */
  virtual bool hasData(const model::Snapshot& data) const = 0;

  /// Draw the content area. The chrome is already drawn and must not be
  /// touched; the content region is cleared before this is called.
  virtual void draw(TFT_eSPI& tft, const model::Snapshot& data) = 0;

  /**
   * Offer a gesture to the screen before the manager acts on it.
   * @return true if consumed, which stops the manager changing screen.
   *
   * This is what lets the league table take vertical swipes for scrolling
   * while horizontal swipes still move between screens.
   */
  virtual bool handleGesture(touch::Gesture) { return false; }

  /**
   * Whether the screen wants redrawing despite no gesture or screen change.
   * Used by anything live: a countdown, or a match clock.
   */
  virtual bool needsRedraw() const { return false; }

  /// Called when the screen becomes visible, so it can reset view state.
  virtual void onShow(const model::Snapshot&) {}
};

// ---------------------------------------------------------------------------
// Small drawing helpers, shared so screens stay consistent
// ---------------------------------------------------------------------------

/// Centred label above a large value — the layout most screens use.
void drawStatBlock(TFT_eSPI& tft, int16_t cx, int16_t cy, const char* label,
                   const char* value, uint16_t valueColour);

/// Format a fixture's scoreline as "2-3", or "v" if unplayed.
void formatScore(const model::Fixture& f, char* out, size_t len);

/// Human-friendly kick-off time, e.g. "Sat 12 Sep, 12:30".
/// Rendered in local time, so it needs the clock to have been set; falls back
/// to a UTC-labelled form when it has not.
void formatKickoff(uint32_t utcSeconds, char* out, size_t len);

/// Time remaining until a kick-off, e.g. "in 2d 4h" or "kicking off".
void formatCountdown(uint32_t utcSeconds, char* out, size_t len);

}  // namespace ui
