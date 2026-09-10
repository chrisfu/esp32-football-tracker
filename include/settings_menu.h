/**
 * @file settings_menu.h
 * @brief On-device settings menu, opened by a long press.
 *
 * A modal mode rather than a screen in the carousel: while it is open the
 * rotation is suspended and touch means "press a button" rather than
 * "navigate".
 *
 * Scope is deliberately narrow. Anything requiring typing belongs in the web
 * interface — a resistive panel is a poor keyboard — so this offers only what
 * genuinely needs to work when the web UI cannot be reached: seeing the
 * device's address so it *can* be reached, the destructive resets that recover
 * a device joined to a network that no longer exists, and instructions for
 * someone who has never used it.
 *
 * Every page has a way back, and every destructive action is confirmed.
 */

#pragma once

#include <TFT_eSPI.h>

#include "model.h"
#include "store.h"
#include "touch_input.h"

namespace ui {

class SettingsMenu {
 public:
  /// What the menu is asking the caller to do once it closes.
  enum class Action : uint8_t {
    None,
    ResetWifi,     ///< Clear credentials and reboot into setup mode.
    FactoryReset,  ///< Erase everything and reboot.
  };

  /**
   * @param data used to name the tracked club.
   *
   * The club's name comes from the fetched standings, not from a stored
   * setting. A separately stored name is a third place that says which team
   * this is, and it drifted exactly as you would expect: configuring a new
   * club by id left the old name in place, so Device info reported the
   * previous team indefinitely.
   */
  void begin(const store::Settings& settings, const model::Snapshot& data) {
    settings_ = &settings;
    data_     = &data;
  }

  /// Open at the root page and draw it.
  void open(TFT_eSPI& tft);

  bool isOpen() const { return page_ != Page::Closed; }

  /**
   * Route a gesture.
   *
   * @param x,y where the gesture happened, for button hit-testing.
   * @return true while the menu remains open and has consumed the input.
   *         false means the menu has closed and the caller should resume the
   *         carousel.
   */
  bool handleGesture(TFT_eSPI& tft, touch::Gesture gesture, int16_t x,
                     int16_t y);

  /// Take any pending action, clearing it. Returns Action::None if there is
  /// nothing to do.
  Action takeAction();

 private:
  enum class Page : uint8_t {
    Closed,
    Root,
    DeviceInfo,
    HowToUse,
    ConfirmWifiReset,
    ConfirmFactoryReset,
  };

  /// A touch target. Kept as plain geometry so hit-testing is obvious.
  ///
  /// No default member initialisers: the Arduino ESP32 core builds as
  /// gnu++11, where a struct with NSDMIs is not an aggregate and so cannot be
  /// brace-initialised. Every use assigns all four fields, so the defaults
  /// would have been redundant anyway.
  struct Button {
    int16_t     y;
    int16_t     height;
    const char* label;
    uint16_t    colour;
  };

  void show(TFT_eSPI& tft, Page page);
  void drawRoot(TFT_eSPI& tft);
  void drawDeviceInfo(TFT_eSPI& tft);
  /// Tracked club's name, preferring the fetched data over any stored name.
  const char* teamLabel() const;
  void drawHowToUse(TFT_eSPI& tft);
  void drawConfirm(TFT_eSPI& tft, const char* heading, const char* detail);
  void drawTitle(TFT_eSPI& tft, const char* title);
  void drawButton(TFT_eSPI& tft, const Button& b, bool danger);
  /// Index of the button containing y, or -1.
  int8_t buttonAt(int16_t y) const;

  const store::Settings*  settings_ = nullptr;
  const model::Snapshot*  data_     = nullptr;
  Page    page_    = Page::Closed;
  Action  action_  = Action::None;

  static constexpr uint8_t kMaxButtons = 6;
  Button  buttons_[kMaxButtons] = {};
  uint8_t buttonCount_ = 0;
};

}  // namespace ui
