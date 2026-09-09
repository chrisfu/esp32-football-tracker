/**
 * @file touch_calibration.cpp
 * @brief Interactive touch calibration. See touch_calibration.h.
 */

#include "touch_calibration.h"

#include <Arduino.h>
#include <stdio.h>

#include "board_config.h"

namespace touch {
namespace {

/// Set by runCalibration/verifyOrientation so the file-local helpers, which
/// were written as free functions during bring-up, can reach the hardware.
TFT_eSPI*   g_tft   = nullptr;
TouchInput* g_input = nullptr;

/// Human-readable gesture name. Wrapped by the public gestureName() below.
const char* gestureNameImpl(touch::Gesture g) {
  switch (g) {
    case touch::Gesture::Tap:        return "TAP";
    case touch::Gesture::DoubleTap:  return "DOUBLE TAP";
    case touch::Gesture::LongPress:  return "LONG PRESS";
    case touch::Gesture::SwipeLeft:  return "SWIPE LEFT";
    case touch::Gesture::SwipeRight: return "SWIPE RIGHT";
    case touch::Gesture::SwipeUp:    return "SWIPE UP";
    case touch::Gesture::SwipeDown:  return "SWIPE DOWN";
    default:                         return "NONE";
  }
}

/// Where the calibration targets sit, inset from the corners so a fingertip
/// can reach them comfortably without falling off the panel edge.
constexpr int16_t kTargetInsetX = 28;
constexpr int16_t kTargetInsetY = 28;

/// Draw a crosshair target with a prompt above it.
void drawTarget(int16_t x, int16_t y, const char* prompt, uint8_t index,
                uint8_t total) {
  g_tft->fillScreen(TFT_BLACK);
  g_tft->setTextDatum(MC_DATUM);
  g_tft->setTextColor(TFT_WHITE, TFT_BLACK);
  g_tft->drawString("TOUCH CALIBRATION", board::kScreenWidth / 2,
                 board::kScreenHeight / 2 - 26, 4);
  g_tft->setTextColor(TFT_CYAN, TFT_BLACK);
  g_tft->drawString(prompt, board::kScreenWidth / 2,
                 board::kScreenHeight / 2 + 4, 2);
  g_tft->setTextColor(TFT_DARKGREY, TFT_BLACK);
  char step[24];
  snprintf(step, sizeof(step), "target %u of %u", index, total);
  g_tft->drawString(step, board::kScreenWidth / 2,
                 board::kScreenHeight / 2 + 26, 2);

  g_tft->drawLine(x - 12, y, x + 12, y, TFT_RED);
  g_tft->drawLine(x, y - 12, x, y + 12, TFT_RED);
  g_tft->drawCircle(x, y, 7, TFT_RED);
  g_tft->fillCircle(x, y, 2, TFT_YELLOW);
}

/**
 * Block until the panel is tapped, and return an averaged raw reading.
 *
 * Samples are taken while the finger is down and averaged, because a single
 * resistive read is noisy. The first few readings after contact are discarded:
 * they are taken while pressure is still building and are the least accurate.
 *
 * @return false if no tap arrived within the timeout.
 */
bool waitForTap(uint16_t& outX, uint16_t& outY) {
  // Deliberately waits forever rather than timing out. A timeout here made
  // bring-up depend on the operator reading an instruction within a fixed
  // window, which is a race the firmware should not impose. Raw values are
  // logged while waiting so a dead SPI link is still diagnosable.
  uint32_t lastBeat = 0;
  while (!g_input->isPressed()) {
    if (millis() - lastBeat > 2000) {
      lastBeat = millis();
      uint16_t rx = 0, ry = 0, rz = 0;
      g_input->readRawUnfiltered(rx, ry, rz);
      Serial.printf("waiting for tap...  IRQ=%s  raw=(%4u,%4u) Z=%4u"
                    " (need Z>%u)\n",
                    g_input->irqAsserted() ? "LOW " : "HIGH", rx, ry, rz,
                    touch::TouchInput::pressureThreshold());
    }
    delay(10);
  }

  // Discard the settling period.
  delay(60);

  uint32_t sumX = 0, sumY = 0;
  uint16_t samples = 0;
  uint16_t rx = 0, ry = 0, rz = 0;
  const uint32_t sampleUntil = millis() + 180;
  while (millis() < sampleUntil) {
    if (g_input->getRaw(rx, ry, rz)) {
      sumX += rx;
      sumY += ry;
      ++samples;
    }
    delay(5);
  }
  if (samples == 0) return false;

  outX = static_cast<uint16_t>(sumX / samples);
  outY = static_cast<uint16_t>(sumY / samples);

  // Wait for release so the next target does not consume this same press.
  while (g_input->isPressed()) delay(10);
  delay(120);
  return true;
}

/// One raw reading, paired with the screen position that produced it.
struct RawSample {
  uint16_t x = 0;
  uint16_t y = 0;
};

/**
 * Derive calibration from three taps at known screen positions.
 *
 * **Why three and not two.** An earlier version used two targets on the
 * diagonal — top-left and bottom-right — and computed a linear fit per axis.
 * That is structurally incapable of detecting a transposed digitizer: when both
 * targets differ in both axes, "raw X tracks screen X" and "raw X tracks screen
 * Y" fit the measurements equally well, and both look like a clean monotonic
 * result. It silently produced a mapping rotated 90 degrees.
 *
 * Three targets fix this by isolating each axis:
 *
 *   TL ──────► TR     moving TL->TR changes ONLY screen X, so whichever raw
 *   │                 channel moves is the one feeding screen X
 *   ▼
 *   BL                moving TL->BL changes ONLY screen Y
 *
 * Axis assignment and inversion are therefore both measured, not assumed.
 */
bool calibrate(touch::Calibration& out) {
  Serial.println();
  Serial.println(F("=== Touch calibration (3-point) ==="));

  const int16_t xL = kTargetInsetX;
  const int16_t xR = board::kScreenWidth - kTargetInsetX;
  const int16_t yT = kTargetInsetY;
  const int16_t yB = board::kScreenHeight - kTargetInsetY;

  RawSample tl, tr, bl;

  struct Step {
    int16_t x, y;
    const char* prompt;
    RawSample* into;
  };
  const Step steps[] = {
      {xL, yT, "Tap the cross: TOP-LEFT", &tl},
      {xR, yT, "Now the TOP-RIGHT cross", &tr},
      {xL, yB, "Now the BOTTOM-LEFT cross", &bl},
  };

  uint8_t index = 1;
  for (const Step& step : steps) {
    drawTarget(step.x, step.y, step.prompt, index, 3);
    if (!waitForTap(step.into->x, step.into->y)) {
      Serial.println(F("No touch detected -- aborting calibration."));
      return false;
    }
    Serial.printf("target %u (%3d,%3d) -> raw (%4u,%4u)\n", index, step.x,
                  step.y, step.into->x, step.into->y);
    ++index;
  }

  // How each raw channel responds to movement along each screen axis.
  const int32_t alongX_rawX = static_cast<int32_t>(tr.x) - tl.x;
  const int32_t alongX_rawY = static_cast<int32_t>(tr.y) - tl.y;
  const int32_t alongY_rawX = static_cast<int32_t>(bl.x) - tl.x;
  const int32_t alongY_rawY = static_cast<int32_t>(bl.y) - tl.y;

  Serial.println();
  Serial.println(F("--- axis response ---"));
  Serial.printf("moving along screen X: rawX %+ld, rawY %+ld\n",
                (long)alongX_rawX, (long)alongX_rawY);
  Serial.printf("moving along screen Y: rawX %+ld, rawY %+ld\n",
                (long)alongY_rawX, (long)alongY_rawY);

  // Whichever channel moves more when only screen X changes is the channel
  // that feeds screen X.
  out.swapAxes = abs(alongX_rawY) > abs(alongX_rawX);
  Serial.printf("=> axes %s\n",
                out.swapAxes ? "TRANSPOSED (raw Y drives screen X)"
                             : "direct (raw X drives screen X)");

  // Cross-check against the other axis. These must agree; if they do not, the
  // taps were probably not where they were asked for.
  const bool consistent = out.swapAxes ? (abs(alongY_rawX) > abs(alongY_rawY))
                                       : (abs(alongY_rawY) > abs(alongY_rawX));
  if (!consistent) {
    Serial.println(F("Axis response is contradictory -- calibration rejected."));
    Serial.println(F("Were all three taps on the crosses shown?"));
    return false;
  }

  // Movement along each screen axis, in the raw channel assigned to it.
  const int32_t spanAlongX = out.swapAxes ? alongX_rawY : alongX_rawX;
  const int32_t spanAlongY = out.swapAxes ? alongY_rawX : alongY_rawY;
  if (abs(spanAlongX) < 200 || abs(spanAlongY) < 200) {
    Serial.println(F("Raw travel too small -- calibration rejected."));
    return false;
  }

  // Linear fit per screen axis, extrapolated out to the screen edges. Targets
  // are inset from the corners because tapping the very edge of a resistive
  // panel is both unreliable and uncomfortable.
  const float slopeX = static_cast<float>(spanAlongX) / (xR - xL);
  const float slopeY = static_cast<float>(spanAlongY) / (yB - yT);
  const float baseX  = out.swapAxes ? tl.y : tl.x;
  const float baseY  = out.swapAxes ? tl.x : tl.y;
  const float rawAtX0   = baseX - slopeX * xL;
  const float rawAtY0   = baseY - slopeY * yT;
  const float rawAtXMax = rawAtX0 + slopeX * (board::kScreenWidth - 1);
  const float rawAtYMax = rawAtY0 + slopeY * (board::kScreenHeight - 1);

  out.invertX = slopeX < 0;
  out.invertY = slopeY < 0;
  // Stored unclamped. A correct fit can extrapolate outside the controller's
  // 0..4095 range, and clamping distorts the mapping near that edge — the
  // reference unit fits a negative value at one edge. Only the wide bounds of
  // a signed 16-bit value are enforced, to keep the arithmetic sane.
  out.rawMinX = static_cast<int16_t>(constrain(min(rawAtX0, rawAtXMax), -4095.0f, 8190.0f));
  out.rawMaxX = static_cast<int16_t>(constrain(max(rawAtX0, rawAtXMax), -4095.0f, 8190.0f));
  out.rawMinY = static_cast<int16_t>(constrain(min(rawAtY0, rawAtYMax), -4095.0f, 8190.0f));
  out.rawMaxY = static_cast<int16_t>(constrain(max(rawAtY0, rawAtYMax), -4095.0f, 8190.0f));
  out.valid = true;

  Serial.println();
  Serial.println(F("--- calibration result ---"));
  Serial.printf("swapAxes=%s\n", out.swapAxes ? "true" : "false");
  Serial.printf("rawMinX=%d rawMaxX=%d invertX=%s\n", out.rawMinX, out.rawMaxX,
                out.invertX ? "true" : "false");
  Serial.printf("rawMinY=%d rawMaxY=%d invertY=%s\n", out.rawMinY, out.rawMaxY,
                out.invertY ? "true" : "false");
  return true;
}

/**
 * Ask for a swipe in a named direction and check the decoder agrees.
 *
 * This exists because the previous orientation bug was invisible to every
 * automated check and was only caught by a person noticing that swipes went
 * the wrong way. Verifying it in firmware means the device reports the fault
 * itself rather than depending on someone being asked the right question.
 */
bool verifySwipe(const char* label, touch::Gesture expected) {
  g_tft->fillScreen(TFT_BLACK);
  g_tft->setTextDatum(MC_DATUM);
  g_tft->setTextColor(TFT_WHITE, TFT_BLACK);
  g_tft->drawString("DIRECTION CHECK", board::kScreenWidth / 2,
                 board::kScreenHeight / 2 - 34, 4);
  g_tft->setTextColor(TFT_YELLOW, TFT_BLACK);
  g_tft->drawString(label, board::kScreenWidth / 2, board::kScreenHeight / 2 + 2, 4);
  g_tft->setTextColor(TFT_DARKGREY, TFT_BLACK);
  g_tft->drawString("across the middle of the screen",
                 board::kScreenWidth / 2, board::kScreenHeight / 2 + 34, 2);

  Serial.printf("Please swipe: %s\n", label);

  const uint32_t deadline = millis() + 30000;
  while (millis() < deadline) {
    const touch::Gesture g = g_input->poll();
    if (g == touch::Gesture::None) {
      delay(8);
      continue;
    }
    const bool ok = (g == expected);
    Serial.printf("  got %s -- %s\n", gestureNameImpl(g), ok ? "PASS" : "MISMATCH");
    return ok;
  }
  Serial.println(F("  timed out waiting for a swipe"));
  return false;
}

}  // namespace

const char* gestureName(Gesture g) { return gestureNameImpl(g); }

bool runCalibration(TFT_eSPI& tft, TouchInput& input, Calibration& out) {
  g_tft   = &tft;
  g_input = &input;
  return calibrate(out);
}

bool verifyOrientation(TFT_eSPI& tft, TouchInput& input) {
  g_tft   = &tft;
  g_input = &input;
  Serial.println();
  Serial.println(F("=== Direction check ==="));
  const bool right = verifySwipe("SWIPE RIGHT ->", Gesture::SwipeRight);
  const bool down  = verifySwipe("SWIPE DOWN", Gesture::SwipeDown);
  if (right && down) {
    Serial.println(F("Orientation CONFIRMED: swipes match their directions."));
  } else {
    Serial.println(F("ORIENTATION FAULT: swipes do not match directions."));
    Serial.println(F("The axis mapping is wrong -- do not trust it."));
  }
  return right && down;
}

}  // namespace touch
