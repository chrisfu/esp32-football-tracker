/**
 * @file touch_input.cpp
 * @brief XPT2046 touch implementation. See touch_input.h for the rationale.
 */

#include "touch_input.h"

#include <Arduino.h>

namespace touch {
namespace {

// XPT2046 control bytes.
//
// Bit layout: S A2 A1 A0 MODE SER/DFR PD1 PD0
//   S        = 1        start bit, always set
//   A2..A0   = channel  101 = X position, 001 = Y position, 011 = Z1
//   MODE     = 0        12-bit conversion (1 would be 8-bit)
//   SER/DFR  = 0        differential reference — far less noisy than
//                       single-ended, which matters on a resistive panel
//   PD1..PD0 = 00       power down between conversions, and importantly this
//                       is what keeps the IRQ line enabled
constexpr uint8_t kCmdReadX  = 0xD0;  // 1101 0000
constexpr uint8_t kCmdReadY  = 0x90;  // 1001 0000
constexpr uint8_t kCmdReadZ1 = 0xB0;  // 1011 0000

/// The controller returns a left-justified 12-bit result in 16 bits.
constexpr uint8_t kResultShift = 3;

/// Below this pressure reading a touch is treated as noise rather than
/// contact. Resistive panels report a small non-zero Z even when untouched.
///
/// Set from measurement, not guesswork: the untouched baseline on this panel is
/// Z = 1-3, while deliberate contact reads 500-1000. An earlier value of 300
/// was seen rejecting the onset of a real touch at Z = 238, so it sat too close
/// to light contact. 200 keeps a ~60x margin over the idle baseline while
/// catching a light fingertip on the first sample rather than the second.
constexpr uint16_t kPressureThreshold = 200;

/// Median of three, used to discard single-sample spikes.
uint16_t medianOfThree(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { const uint16_t t = a; a = b; b = t; }
  if (b > c) { const uint16_t t = b; b = c; c = t; }
  if (a > b) { const uint16_t t = a; a = b; b = t; }
  return b;
}

}  // namespace

void TouchInput::begin() {
  // The touch pins are not any SPI peripheral's native pins, so this routes
  // through the GPIO matrix. Harmless here: the XPT2046 runs at 2.5 MHz, far
  // below where matrix routing costs anything measurable.
  //
  // VSPI is used because the display already owns HSPI. The SD card also lives
  // on VSPI but on different pins; both are low-rate and never accessed
  // concurrently, so sharing the peripheral is acceptable.
  spi_.begin(board::kPinTouchClk, board::kPinTouchMiso, board::kPinTouchMosi,
             board::kPinTouchCs);

  pinMode(board::kPinTouchCs, OUTPUT);
  digitalWrite(board::kPinTouchCs, HIGH);  // Deselect.

  // No internal pull-up is available on GPIO36 (input-only), but the XPT2046
  // drives this line actively, so none is needed.
  pinMode(board::kPinTouchIrq, INPUT);
}

bool TouchInput::irqAsserted() const {
  // Active low: the controller pulls this down while the panel is pressed.
  return digitalRead(board::kPinTouchIrq) == LOW;
}

uint16_t TouchInput::readChannel(uint8_t command) {
  spi_.transfer(command);
  // A dummy 16-bit transfer clocks out the conversion result. The controller
  // needs the conversion time that the command byte's own clocks provide, so
  // no explicit delay is required.
  return spi_.transfer16(0x0000) >> kResultShift;
}

bool TouchInput::sampleRaw(uint16_t& x, uint16_t& y, uint16_t& z) {
  uint16_t xs[3], ys[3], zs[3];

  spi_.beginTransaction(SPISettings(board::kTouchSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(board::kPinTouchCs, LOW);
  for (uint8_t i = 0; i < 3; ++i) {
    zs[i] = readChannel(kCmdReadZ1);
    xs[i] = readChannel(kCmdReadX);
    ys[i] = readChannel(kCmdReadY);
  }
  digitalWrite(board::kPinTouchCs, HIGH);
  spi_.endTransaction();

  z = medianOfThree(zs[0], zs[1], zs[2]);
  if (z < kPressureThreshold) return false;

  x = medianOfThree(xs[0], xs[1], xs[2]);
  y = medianOfThree(ys[0], ys[1], ys[2]);
  return true;
}

void TouchInput::readRawUnfiltered(uint16_t& x, uint16_t& y, uint16_t& z) {
  uint16_t xs[3], ys[3], zs[3];
  spi_.beginTransaction(SPISettings(board::kTouchSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(board::kPinTouchCs, LOW);
  for (uint8_t i = 0; i < 3; ++i) {
    zs[i] = readChannel(kCmdReadZ1);
    xs[i] = readChannel(kCmdReadX);
    ys[i] = readChannel(kCmdReadY);
  }
  digitalWrite(board::kPinTouchCs, HIGH);
  spi_.endTransaction();
  x = medianOfThree(xs[0], xs[1], xs[2]);
  y = medianOfThree(ys[0], ys[1], ys[2]);
  z = medianOfThree(zs[0], zs[1], zs[2]);
}

uint16_t TouchInput::pressureThreshold() { return kPressureThreshold; }

bool TouchInput::getRaw(uint16_t& x, uint16_t& y, uint16_t& z) {
  // The IRQ line is checked first because it costs one digital read, whereas a
  // position sample costs nine SPI transfers. Most polls find nothing, so this
  // ordering is what keeps idle polling cheap.
  if (!irqAsserted()) return false;
  return sampleRaw(x, y, z);
}

int16_t TouchInput::mapRawToScreenX(uint16_t rawX, uint16_t rawY) const {
  const int32_t span =
      static_cast<int32_t>(calibration_.rawMaxX) - calibration_.rawMinX;
  if (span == 0) return 0;  // Degenerate calibration: fail safe, don't divide.

  // Which raw channel feeds screen X depends on whether the digitizer is
  // transposed relative to the display. Both channels are taken as arguments
  // precisely so this choice lives here rather than at every call site.
  const int32_t raw = calibration_.swapAxes ? rawY : rawX;

  int32_t v = (raw - calibration_.rawMinX) * (board::kScreenWidth - 1) / span;
  if (calibration_.invertX) v = (board::kScreenWidth - 1) - v;
  return static_cast<int16_t>(constrain(v, 0, board::kScreenWidth - 1));
}

int16_t TouchInput::mapRawToScreenY(uint16_t rawX, uint16_t rawY) const {
  const int32_t span =
      static_cast<int32_t>(calibration_.rawMaxY) - calibration_.rawMinY;
  if (span == 0) return 0;

  const int32_t raw = calibration_.swapAxes ? rawX : rawY;

  int32_t v = (raw - calibration_.rawMinY) * (board::kScreenHeight - 1) / span;
  if (calibration_.invertY) v = (board::kScreenHeight - 1) - v;
  return static_cast<int16_t>(constrain(v, 0, board::kScreenHeight - 1));
}

bool TouchInput::getPoint(int16_t& x, int16_t& y) {
  uint16_t rx = 0, ry = 0, rz = 0;
  if (!getRaw(rx, ry, rz)) return false;
  x = mapRawToScreenX(rx, ry);
  y = mapRawToScreenY(rx, ry);
  return true;
}

bool TouchInput::isPressed() {
  // Debounce against release chatter: resistive panels commonly report a brief
  // false press immediately after a finger lifts.
  if (millis() - lastRelease_ < kDebounceMs) return false;
  if (!irqAsserted()) return false;

  // The IRQ line alone can twitch on electrical noise, so confirm with a
  // pressure reading before reporting contact.
  uint16_t x = 0, y = 0, z = 0;
  return sampleRaw(x, y, z);
}

Gesture TouchInput::poll() {
  int16_t x = 0, y = 0;
  const bool down = getPoint(x, y);

  // A pending tap becomes a single tap once the double-tap window closes
  // without a second tap. Emitting it here, rather than at release, is what
  // makes Tap and DoubleTap mutually exclusive — exactly one fires per
  // interaction, so a caller never has to disambiguate them.
  if (tapPending_ && !down && millis() - tapPendingAt_ >= kDoubleTapMs) {
    tapPending_ = false;
    return Gesture::Tap;
  }

  // --- Press begins -------------------------------------------------------
  if (down && !tracking_) {
    if (millis() - lastRelease_ < kDebounceMs) return Gesture::None;
    tracking_   = true;
    startX_     = lastX_ = x;
    startY_     = lastY_ = y;
    pressStart_ = millis();
    return Gesture::None;
  }

  // --- Press continues ----------------------------------------------------
  if (down) {
    lastX_ = x;
    lastY_ = y;
    return Gesture::None;
  }

  // --- Nothing happening --------------------------------------------------
  if (!tracking_) return Gesture::None;

  // --- Release: decode what just happened ---------------------------------
  tracking_    = false;
  lastRelease_ = millis();

  // A finger resting on the panel is not a gesture. Rejecting long presses
  // here also stops a slow drag on the league table registering as a swipe
  // once the finger finally lifts.
  if (millis() - pressStart_ > kMaxGestureMs) return Gesture::None;

  const int16_t dx = lastX_ - startX_;
  const int16_t dy = lastY_ - startY_;

  // Axes are compared against each other first so a diagonal drag resolves to
  // its dominant direction, rather than firing whichever threshold happened to
  // be crossed first.
  if (abs(dx) >= kSwipeMin && abs(dx) >= abs(dy)) {
    return dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight;
  }
  if (abs(dy) >= kSwipeMin && abs(dy) > abs(dx)) {
    return dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown;
  }
  if (abs(dx) <= kTapSlop && abs(dy) <= kTapSlop) {
    // A second tap inside the window is a double tap; otherwise hold this one
    // back until the window closes (see the deferred emission in poll()).
    if (tapPending_ && millis() - tapPendingAt_ < kDoubleTapMs) {
      tapPending_ = false;
      return Gesture::DoubleTap;
    }
    tapPending_   = true;
    tapPendingAt_ = millis();
    return Gesture::None;
  }

  // A swipe cancels any pending tap: the user has moved on, and firing a
  // stale tap afterwards would act on an intention they no longer have.
  tapPending_ = false;

  // Moved too far for a tap, not far enough for a swipe. Deliberately
  // swallowed: guessing produces screen changes the user did not ask for,
  // which is worse than doing nothing.
  return Gesture::None;
}

}  // namespace touch
