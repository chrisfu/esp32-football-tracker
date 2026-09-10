/**
 * @file screen_manager.cpp
 * @brief Screen rotation, chrome and gesture routing.
 */

#include "screen_manager.h"

#include <Arduino.h>

#include "power.h"

namespace ui {
namespace {

/// How often a screen that reports needsRedraw() is refreshed. A live match
/// clock or a countdown only changes once a second, so redrawing faster than
/// this would burn SPI bandwidth and power for no visible benefit.
constexpr uint32_t kLiveRedrawIntervalMs = 1000;

}  // namespace

void ScreenManager::begin(TFT_eSPI& tft, const model::Snapshot& data,
                          uint32_t dwellMs) {
  tft_     = &tft;
  data_    = &data;
  dwellMs_ = dwellMs;

  tft_->fillScreen(colour::kBackground);
  // Start on the first screen that actually has something to show.
  current_ = nextWithData(0, +1);
  showIndex(current_, /*force=*/true);
}

bool ScreenManager::add(Screen* screen) {
  if (count_ >= kMaxScreens || screen == nullptr) return false;
  screens_[count_++] = screen;
  return true;
}

void ScreenManager::setPriority(Screen* screen) {
  priorityIndex_ = 0xFF;
  for (uint8_t i = 0; i < count_; ++i) {
    if (screens_[i] == screen) {
      priorityIndex_ = i;
      return;
    }
  }
}

void ScreenManager::setEnabledMask(uint8_t mask) {
  // Refuse a mask that would leave nothing to show. A blank device is a
  // support call; ignoring an impossible request is not.
  if (mask == 0) {
    Serial.println(F("[screens] refusing an all-disabled mask"));
    return;
  }
  enabledMask_ = mask;
}

bool ScreenManager::isUsable(uint8_t index) const {
  if (index >= count_) return false;
  // The priority screen overrides its own disable bit: a live match matters
  // more than a preference set before it kicked off.
  const bool enabled =
      (enabledMask_ & (1u << index)) != 0 || index == priorityIndex_;
  return enabled && screens_[index]->hasData(*data_);
}

uint8_t ScreenManager::nextWithData(uint8_t from, int8_t direction) const {
  if (count_ == 0) return 0;

  // Walk at most one full lap. If nothing has data we return where we started
  // rather than looping forever — a blank screen is better than a hang.
  for (uint8_t step = 1; step <= count_; ++step) {
    const int16_t candidate =
        (static_cast<int16_t>(from) + direction * step + count_ * 2) % count_;
    if (isUsable(static_cast<uint8_t>(candidate))) {
      return static_cast<uint8_t>(candidate);
    }
  }
  return from;
}

void ScreenManager::showIndex(uint8_t index, bool force) {
  if (!force && index == current_) return;
  current_ = index;
  shownAt_ = millis();
  screens_[current_]->onShow(*data_);
  drawChrome();
  redrawContent();
}

void ScreenManager::advance(int8_t direction) {
  const uint8_t target = nextWithData(current_, direction);
  if (target == current_) {
    // Only one screen has data. Redraw anyway so the swipe visibly did
    // something rather than appearing to be ignored.
    redrawContent();
    shownAt_ = millis();
    return;
  }
  showIndex(target);
}

void ScreenManager::refresh() {
  if (tft_ == nullptr || count_ == 0) return;
  tft_->fillScreen(colour::kBackground);
  drawChrome();
  redrawContent();
  shownAt_ = millis();
}

void ScreenManager::drawChrome() {
  // Header: screen title, and the competition on the right for context.
  tft_->fillRect(0, 0, board::kScreenWidth, kHeaderHeight, colour::kHeaderBg);
  tft_->setTextDatum(ML_DATUM);
  tft_->setTextColor(colour::kPrimary, colour::kHeaderBg);
  tft_->drawString(screens_[current_]->title(), 6, kHeaderHeight / 2, 4);

  if (data_->competitionName[0] != '\0') {
    tft_->setTextDatum(MR_DATUM);
    tft_->setTextColor(colour::kMuted, colour::kHeaderBg);
    tft_->drawString(data_->competitionName, board::kScreenWidth - 6,
                     kHeaderHeight / 2, 2);
  }
  drawFooter();
}

void ScreenManager::drawFooter() {
  tft_->fillRect(0, kContentBottom, board::kScreenWidth, kFooterHeight,
                 colour::kFooterBg);

  // One dot per screen, so position in the rotation is always visible. Screens
  // without data are drawn hollow rather than hidden: a moving set of dots
  // would be more confusing than a consistent one.
  constexpr int16_t kDotSpacing = 14;
  const int16_t totalWidth = (count_ - 1) * kDotSpacing;
  const int16_t startX = (board::kScreenWidth - totalWidth) / 2;
  const int16_t cy = kContentBottom + kFooterHeight / 2;

  for (uint8_t i = 0; i < count_; ++i) {
    const int16_t cx = startX + i * kDotSpacing;
    const bool has = isUsable(i);
    if (i == current_) {
      tft_->fillCircle(cx, cy, 4, colour::kAccent);
    } else if (has) {
      tft_->fillCircle(cx, cy, 2, colour::kMuted);
    } else {
      tft_->drawCircle(cx, cy, 2, 0x4208);  // Dim outline: present but empty.
    }
  }

  // Left of the dots: hold state, and whether a match is in progress. LIVE
  // takes the slot when both apply, because it explains *why* the rotation is
  // behaving differently, which matters more than restating that it is held.
  tft_->setTextDatum(ML_DATUM);
  const bool priorityActive =
      priorityIndex_ < count_ && screens_[priorityIndex_]->hasData(*data_);
  if (priorityActive) {
    tft_->setTextColor(colour::kLoss, colour::kFooterBg);
    tft_->drawString("LIVE", 6, cy, 2);
  } else if (pinned_) {
    tft_->setTextColor(colour::kOurTeam, colour::kFooterBg);
    tft_->drawString("HELD", 6, cy, 2);
  }
}

void ScreenManager::redrawContent() {
  tft_->fillRect(0, kContentTop, kContentWidth, kContentHeight,
                 colour::kBackground);
  screens_[current_]->draw(*tft_, *data_);
  // Counted because SPI traffic is both CPU time and bus power, and a screen
  // that redraws needlessly is the kind of waste that is invisible without
  // being measured.
  power::noteRedraw();
}

void ScreenManager::handleGesture(touch::Gesture gesture) {
  if (gesture == touch::Gesture::None || count_ == 0) return;

  // The screen gets first refusal. This is what lets the league table consume
  // vertical swipes for scrolling while horizontal ones still navigate.
  if (screens_[current_]->handleGesture(gesture)) {
    redrawContent();
    // Interacting with a screen implies wanting to stay on it.
    shownAt_ = millis();
    return;
  }

  switch (gesture) {
    // Swiping navigates but does NOT pin. Auto-pinning on a swipe meant
    // browsing the screens silently stopped the rotation, which then had to be
    // undone deliberately — surprising, and easy to trigger without noticing.
    // Resetting the dwell is enough: it gives a full period to read the screen
    // just arrived at, without changing the device's mode behind the user's
    // back. Holding is now only ever an explicit double tap.
    case touch::Gesture::SwipeLeft:
      advance(+1);
      break;
    case touch::Gesture::SwipeRight:
      advance(-1);
      break;
    case touch::Gesture::DoubleTap:
      // Hold requires a *double* tap. A single tap was too easily triggered by
      // accident on a resistive panel — brushing the screen would silently
      // stop the rotation, which looks like the device having frozen.
      pinned_ = !pinned_;
      shownAt_ = millis();
      drawFooter();
      break;
    case touch::Gesture::Tap:
      // Deliberately does nothing yet. Kept distinct from DoubleTap so a
      // future per-screen action can use it without disturbing hold.
      break;
    default:
      break;
  }
}

void ScreenManager::tick() {
  if (count_ == 0) return;

  // --- Priority screen ----------------------------------------------------
  const bool priorityActive =
      priorityIndex_ < count_ && screens_[priorityIndex_]->hasData(*data_);

  if (priorityActive && !priorityWasActive_) {
    // A match has just kicked off. Jump straight there rather than waiting
    // out the dwell — the whole point of priority is not making someone wait
    // to see that something is happening.
    priorityWasActive_ = true;
    // An explicit hold is respected even here: the user has said where they
    // want to be. The footer still shows LIVE, so the match is not hidden.
    if (!pinned_) {
      showIndex(priorityIndex_, /*force=*/true);
      return;
    }
    drawFooter();
  } else if (!priorityActive && priorityWasActive_) {
    // The match is over — confirmed by the screen no longer reporting data,
    // not by any clock of ours. Resume normal rotation from wherever we are.
    priorityWasActive_ = false;
    shownAt_ = millis();
    drawFooter();
  }

  // Live content (a match clock, a countdown) refreshes on its own schedule,
  // rate-limited so it cannot monopolise the SPI bus — and skipped entirely
  // while dimmed, since the point of the refresh is that someone is reading
  // it.
  if (!lowPower_ && screens_[current_]->needsRedraw() &&
      millis() - lastLiveRedraw_ > kLiveRedrawIntervalMs) {
    lastLiveRedraw_ = millis();
    redrawContent();
  }

  if (pinned_) return;
  if (millis() - shownAt_ < dwellMs_) return;

  if (priorityActive) {
    if (current_ != priorityIndex_) {
      // Return to the match rather than continuing round the carousel.
      showIndex(priorityIndex_);
    } else {
      // Already on it: stay put instead of advancing.
      shownAt_ = millis();
    }
    return;
  }

  advance(+1);
}

}  // namespace ui
