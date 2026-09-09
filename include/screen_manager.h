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
 *   - Swipe left/right moves between screens and resets the dwell, but does
 *     NOT pin. Auto-pinning on a swipe meant browsing silently stopped the
 *     rotation, which then had to be undone deliberately.
 *   - **Double** tap toggles the hold. A single tap was too easily triggered
 *     by brushing a resistive panel, and the failure mode was bad: rotation
 *     stops and the device looks frozen.
 *   - Vertical swipes are offered to the screen first, so the league table can
 *     scroll without leaving the screen.
 *   - A long press is intercepted before the manager sees it, to open the
 *     settings menu.
 */
class ScreenManager {
 public:
  static constexpr uint8_t kMaxScreens = 8;

  /// @param dwellMs how long each screen is shown while cycling.
  void begin(TFT_eSPI& tft, const model::Snapshot& data, uint32_t dwellMs);

  /// Screens are added in display order. Not owned; must outlive the manager.
  bool add(Screen* screen);

  /**
   * Nominate a screen that takes priority whenever it has data.
   *
   * While that screen reports data — a match in progress — the rotation stops
   * being a carousel and becomes a leash:
   *
   *   - It is shown *immediately* when it becomes available, not on the next
   *     rotation tick. A goal going in while the table is on screen should not
   *     wait out the dwell.
   *   - Other screens may still be swiped to, but once the dwell elapses the
   *     device returns here rather than continuing round.
   *   - An explicit double-tap hold still wins. Someone who has said "stay on
   *     this" unambiguously must not be overridden, or the device fights them.
   *   - Normal rotation resumes once the screen stops reporting data, which is
   *     driven by the provider confirming the match over — never by the clock
   *     passing 90, because a result can change in stoppage time.
   */
  void setPriority(Screen* screen);

  /// Drive the rotation and redraws. Call every loop iteration.
  void tick();

  /// Route a decoded gesture. Gesture::None is ignored.
  void handleGesture(touch::Gesture gesture);

  /// True while the rotation is paused by the user.
  bool isPinned() const { return pinned_; }

  /**
   * Redraw everything, chrome included.
   *
   * Needed after anything that has taken over the whole panel — the settings
   * menu, a status screen — since the manager otherwise only repaints the
   * content area and would leave a stale title bar and footer behind.
   */
  void refresh();

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

  /// Index of the priority screen, or 0xFF if none is set.
  uint8_t  priorityIndex_ = 0xFF;
  /// Whether the priority screen had data last tick, to detect the moment a
  /// match starts (jump straight to it) and the moment it ends (resume).
  bool     priorityWasActive_ = false;

  Screen*  screens_[kMaxScreens] = {nullptr};
  uint8_t  count_    = 0;
  uint8_t  current_  = 0;
  bool     pinned_   = false;
  uint32_t dwellMs_  = 12000;
  uint32_t shownAt_  = 0;
  uint32_t lastLiveRedraw_ = 0;
};

}  // namespace ui
