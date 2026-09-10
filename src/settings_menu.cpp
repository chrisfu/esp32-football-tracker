/**
 * @file settings_menu.cpp
 * @brief On-device settings menu. See settings_menu.h.
 */

#include "settings_menu.h"

#include <Arduino.h>
#include <stdio.h>

#include "board_config.h"
#include "network.h"
#include "screen.h"

namespace ui {
namespace {

constexpr int16_t kTitleHeight = 30;
/// Buttons are 34 px tall with 5 px gaps: large enough to hit reliably with a
/// fingertip on a resistive panel, and five still fit below the title.
constexpr int16_t kButtonHeight = 34;
constexpr int16_t kButtonGap    = 5;
constexpr int16_t kButtonLeft   = 8;

constexpr uint16_t kDangerColour = 0x6000;  // Dark red.
constexpr uint16_t kButtonColour = 0x2124;  // Dark grey.

}  // namespace

void SettingsMenu::open(TFT_eSPI& tft) { show(tft, Page::Root); }

SettingsMenu::Action SettingsMenu::takeAction() {
  const Action a = action_;
  action_ = Action::None;
  return a;
}

void SettingsMenu::drawTitle(TFT_eSPI& tft, const char* title) {
  tft.fillScreen(colour::kBackground);
  tft.fillRect(0, 0, board::kScreenWidth, kTitleHeight, colour::kHeaderBg);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(colour::kAccent, colour::kHeaderBg);
  tft.drawString(title, 8, kTitleHeight / 2, 4);
}

void SettingsMenu::drawButton(TFT_eSPI& tft, const Button& b, bool danger) {
  const uint16_t fill = danger ? kDangerColour : b.colour;
  tft.fillRoundRect(kButtonLeft, b.y, board::kScreenWidth - 2 * kButtonLeft,
                    b.height, 5, fill);
  tft.drawRoundRect(kButtonLeft, b.y, board::kScreenWidth - 2 * kButtonLeft,
                    b.height, 5, danger ? colour::kLoss : colour::kMuted);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour::kPrimary, fill);
  tft.drawString(b.label, board::kScreenWidth / 2, b.y + b.height / 2, 2);
}

int8_t SettingsMenu::buttonAt(int16_t y) const {
  for (uint8_t i = 0; i < buttonCount_; ++i) {
    if (y >= buttons_[i].y && y < buttons_[i].y + buttons_[i].height) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

void SettingsMenu::drawRoot(TFT_eSPI& tft) {
  drawTitle(tft, "Settings");

  static const char* kLabels[] = {
      "Device info",
      "How to use",
      "Reset Wi-Fi settings",
      "Factory reset",
      "Back to screens",
  };
  buttonCount_ = sizeof(kLabels) / sizeof(kLabels[0]);

  int16_t y = kTitleHeight + 6;
  for (uint8_t i = 0; i < buttonCount_; ++i) {
    buttons_[i] = {y, kButtonHeight, kLabels[i], kButtonColour};
    // The two resets are drawn as dangerous so they are not tapped casually,
    // even though both are confirmed on the next page.
    drawButton(tft, buttons_[i], i == 2 || i == 3);
    y += kButtonHeight + kButtonGap;
  }
}

/**
 * The tracked club's name, taken from the data rather than from settings.
 *
 * Falls back to the stored name only before any standings have been fetched,
 * which is the one moment the data cannot answer.
 */
const char* SettingsMenu::teamLabel() const {
  if (data_ != nullptr) {
    const model::TableRow* us = data_->ourTeam();
    if (us != nullptr && us->name[0] != '\0') return us->name;
  }
  if (settings_ != nullptr && settings_->teamDisplayName[0] != '\0') {
    return settings_->teamDisplayName;
  }
  return "not identified";
}

void SettingsMenu::drawDeviceInfo(TFT_eSPI& tft) {
  drawTitle(tft, "Device info");

  const net::Status& st = net::status();
  char buf[64];

  // The address is the point of this page: it is how the user reaches the web
  // interface to change anything that needs typing.
  struct Line { const char* label; const char* value; uint16_t colour; };
  char signalBuf[24], quotaBuf[24], heapBuf[24];

  store::Quota q;
  store::loadQuota(q);
  snprintf(signalBuf, sizeof(signalBuf), "%d dBm (%u%%)", st.rssi, st.quality);
  snprintf(quotaBuf, sizeof(quotaBuf), "%u of %u today", q.used, q.limit);
  snprintf(heapBuf, sizeof(heapBuf), "%lu KB free",
           (unsigned long)(ESP.getFreeHeap() / 1024));

  const Line lines[] = {
      {"Network",  st.ssid[0] != '\0' ? st.ssid : "not connected",
       st.ssid[0] != '\0' ? colour::kPrimary : colour::kLoss},
      {"Address",  st.ip[0] != '\0' ? st.ip : "-", colour::kOurTeam},
      {"Web UI",   "http://football.local", colour::kAccent},
      {"Signal",   signalBuf, colour::kPrimary},
      {"Team",     teamLabel(), colour::kPrimary},
      {"Clock",    st.timeSynced ? "synced" : "not synced",
       st.timeSynced ? colour::kWin : colour::kDraw},
      {"API calls", quotaBuf, colour::kPrimary},
      {"Memory",   heapBuf, colour::kMuted},
  };

  int16_t y = kTitleHeight + 8;
  for (const Line& l : lines) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(colour::kMuted, colour::kBackground);
    tft.drawString(l.label, 10, y, 1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(l.colour, colour::kBackground);
    tft.drawString(l.value, board::kScreenWidth - 10, y - 2, 2);
    y += 20;
  }

  buttonCount_ = 1;
  buttons_[0] = {board::kScreenHeight - kButtonHeight - 6, kButtonHeight,
                 "Back", kButtonColour};
  drawButton(tft, buttons_[0], false);
  (void)buf;
}

void SettingsMenu::drawHowToUse(TFT_eSPI& tft) {
  drawTitle(tft, "How to use");

  // Deliberately gesture-first: these are the things that are not discoverable
  // by looking at the device, which is exactly what on-device help should
  // cover. Anything requiring typing is pointed at the web interface instead.
  struct Hint { const char* gesture; const char* effect; };
  static const Hint kHints[] = {
      {"Swipe left/right", "change screen"},
      {"Double tap",       "hold this screen"},
      {"Swipe up/down",    "scroll the table"},
      {"Long press",       "open settings"},
      {"Live match",       "always takes priority"},
  };

  int16_t y = kTitleHeight + 10;
  for (const Hint& h : kHints) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(colour::kAccent, colour::kBackground);
    tft.drawString(h.gesture, 10, y, 2);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(colour::kPrimary, colour::kBackground);
    tft.drawString(h.effect, board::kScreenWidth - 10, y, 2);
    y += 22;
  }

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("Screens change on their own; a held screen",
                 board::kScreenWidth / 2, y + 4, 1);
  tft.drawString("stays put. Settings that need typing live at",
                 board::kScreenWidth / 2, y + 14, 1);
  tft.setTextColor(colour::kAccent, colour::kBackground);
  tft.drawString("http://football.local", board::kScreenWidth / 2, y + 24, 1);

  buttonCount_ = 1;
  buttons_[0] = {board::kScreenHeight - kButtonHeight - 6, kButtonHeight,
                 "Back", kButtonColour};
  drawButton(tft, buttons_[0], false);
}

void SettingsMenu::drawConfirm(TFT_eSPI& tft, const char* heading,
                               const char* detail) {
  drawTitle(tft, "Are you sure?");

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(colour::kLoss, colour::kBackground);
  tft.drawString(heading, board::kScreenWidth / 2, kTitleHeight + 16, 4);
  tft.setTextColor(colour::kPrimary, colour::kBackground);
  tft.drawString(detail, board::kScreenWidth / 2, kTitleHeight + 48, 2);
  tft.setTextColor(colour::kMuted, colour::kBackground);
  tft.drawString("This cannot be undone.", board::kScreenWidth / 2,
                 kTitleHeight + 70, 2);

  // "Go back" sits below "Yes", so the safe option is the one nearest the
  // thumb and the destructive one is not where a stray tap lands.
  buttonCount_ = 2;
  buttons_[0] = {kTitleHeight + 100, kButtonHeight, "Yes, do it",
                 kDangerColour};
  buttons_[1] = {kTitleHeight + 100 + kButtonHeight + kButtonGap + 6,
                 kButtonHeight, "Go back", kButtonColour};
  drawButton(tft, buttons_[0], true);
  drawButton(tft, buttons_[1], false);
}

void SettingsMenu::show(TFT_eSPI& tft, Page page) {
  page_        = page;
  buttonCount_ = 0;
  switch (page) {
    case Page::Root:       drawRoot(tft); break;
    case Page::DeviceInfo: drawDeviceInfo(tft); break;
    case Page::HowToUse:   drawHowToUse(tft); break;
    case Page::ConfirmWifiReset:
      drawConfirm(tft, "Reset Wi-Fi?",
                  "The device will restart into setup mode.");
      break;
    case Page::ConfirmFactoryReset:
      drawConfirm(tft, "Factory reset?",
                  "Erases Wi-Fi, settings and cache.");
      break;
    case Page::Closed: break;
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool SettingsMenu::handleGesture(TFT_eSPI& tft, touch::Gesture gesture,
                                 int16_t x, int16_t y) {
  if (page_ == Page::Closed) return false;
  (void)x;  // Buttons are full-width, so only the row matters.

  // A long press anywhere closes the menu, mirroring the gesture that opened
  // it. Cheap insurance: whatever page someone is stranded on, the gesture
  // they already know gets them out.
  if (gesture == touch::Gesture::LongPress) {
    page_ = Page::Closed;
    return false;
  }

  // A right swipe reads as "back" everywhere, which matches the swipe-based
  // navigation of the carousel this menu sits on top of.
  if (gesture == touch::Gesture::SwipeRight) {
    if (page_ == Page::Root) {
      page_ = Page::Closed;
      return false;
    }
    show(tft, Page::Root);
    return true;
  }

  // Only taps press buttons. Deliberately not DoubleTap: a double tap
  // registers a Tap first in no case (they are mutually exclusive), so a
  // hurried double tap on a confirmation button cannot fire it twice.
  if (gesture != touch::Gesture::Tap) return true;

  const int8_t hit = buttonAt(y);
  if (hit < 0) return true;  // Missed a button; stay put.

  switch (page_) {
    case Page::Root:
      switch (hit) {
        case 0: show(tft, Page::DeviceInfo); break;
        case 1: show(tft, Page::HowToUse); break;
        case 2: show(tft, Page::ConfirmWifiReset); break;
        case 3: show(tft, Page::ConfirmFactoryReset); break;
        case 4: page_ = Page::Closed; return false;
        default: break;
      }
      return true;

    case Page::DeviceInfo:
    case Page::HowToUse:
      show(tft, Page::Root);
      return true;

    case Page::ConfirmWifiReset:
      if (hit == 0) {
        action_ = Action::ResetWifi;
        page_   = Page::Closed;
        return false;
      }
      show(tft, Page::Root);
      return true;

    case Page::ConfirmFactoryReset:
      if (hit == 0) {
        action_ = Action::FactoryReset;
        page_   = Page::Closed;
        return false;
      }
      show(tft, Page::Root);
      return true;

    case Page::Closed:
      return false;
  }
  return true;
}

}  // namespace ui
