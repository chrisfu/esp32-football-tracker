/**
 * @file screen_manager.h
 * @brief Owns the screen rotation, the chrome, and gesture routing.
 */

#pragma once

#include <TFT_eSPI.h>

#include "model.h"
#include "screen.h"
#include "touch_input.h"

namespace ui {

/**
 * Cycles between screens on a timer, and hands control to touch when used.
 *
 * Interaction model:
 *   - Auto-cycles every `dwellMs`, skipping screens with no data.
 *   - Swipe left/right moves between screens and pins the rotation, because
 *     someone who navigated deliberately does not want it moving on under them.
 *   - Tap toggles the pin, so cycling can be resumed without waiting.
 *   - Vertical swipes are offered to the screen first, so the league table can
 *     scroll without leaving the screen.
 */
class ScreenManager {
 public:
  static constexpr uint8_t kMaxScreens = 8;

  /// @param dwellMs how long each screen is shown while cycling.
  void begin(TFT_eSPI& tft, const model::Snapshot& data, uint32_t dwellMs);

  /// Screens are added in display order. Not owned; must outlive the manager.
  bool add(Screen* screen);

  /// Drive the rotation and redraws. Call every loop iteration.
  void tick();

  /// Route a decoded gesture. Gesture::None is ignored.
  void handleGesture(touch::Gesture gesture);

  /// True while the rotation is paused by the user.
  bool isPinned() const { return pinned_; }

 private:
  void showIndex(uint8_t index, bool force = false);
  void advance(int8_t direction);
  /// Next index in `direction` that has data, or the current one if none do.
  uint8_t nextWithData(uint8_t from, int8_t direction) const;
  void drawChrome();
  void drawFooter();
  void redrawContent();

  TFT_eSPI*             tft_  = nullptr;
  const model::Snapshot* data_ = nullptr;

  Screen*  screens_[kMaxScreens] = {nullptr};
  uint8_t  count_    = 0;
  uint8_t  current_  = 0;
  bool     pinned_   = false;
  uint32_t dwellMs_  = 12000;
  uint32_t shownAt_  = 0;
  uint32_t lastLiveRedraw_ = 0;
};

}  // namespace ui
