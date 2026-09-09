/**
 * @file main.cpp
 * @brief Hardware bring-up diagnostic for the ESP32 Football Tracker.
 *
 * Written as a diagnostic rather than a demo: its job is to settle the open
 * hardware questions in SPEC.md and to produce the per-device constants the
 * finished firmware needs.
 *
 * Display bring-up (complete) established that this panel is an ILI9341
 * variant that powers up inverted, and measured a 31 ms full-screen fill.
 *
 * Touch bring-up (this stage) does three things:
 *   1. Confirms the XPT2046 responds on its own SPI bus without disturbing the
 *      display, which shares neither pins nor peripheral.
 *   2. Derives calibration constants by asking for two taps at known targets.
 *      Resistive panels vary unit to unit, so these cannot be assumed.
 *   3. Verifies the touch IRQ line behaves — the same line is the deep-sleep
 *      wake source, so if it misbehaves here, wake-on-touch will not work.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "board_config.h"
#include "touch_input.h"

namespace {

TFT_eSPI tft;
touch::TouchInput touchInput;

/// Kept so the result can be re-announced periodically — a capture started
/// after calibration finished would otherwise miss it entirely.
touch::Calibration g_calibration;
bool g_calibrated = false;

/// Remembered from setup() so the serial toggle handler can redraw correctly.
bool g_panelConfirmed = false;

/// Current display inversion state, shown on screen and toggleable over serial.
bool g_inverted = true;  // matches the TFT_INVERSION_ON build flag

// ---------------------------------------------------------------------------
// Chip report
// ---------------------------------------------------------------------------

/**
 * Print what the silicon says about itself.
 *
 * The cheapest possible guard against a swapped board or wrong build target,
 * and it confirms the "no PSRAM" constraint that shapes the whole design.
 */
void reportChip() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  Serial.println();
  Serial.println(F("=== Chip ==="));
  Serial.printf("Cores / rev    : %d / %d\n", info.cores, info.revision);
  Serial.printf("CPU frequency  : %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
  Serial.printf("Flash          : %lu KB @ %lu MHz\n",
                (unsigned long)(ESP.getFlashChipSize() / 1024),
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  Serial.printf("Sketch size    : %lu KB\n",
                (unsigned long)(ESP.getSketchSize() / 1024));

  Serial.println();
  Serial.println(F("=== Memory ==="));
  Serial.printf("Free heap      : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  // The real constraint is not total free heap but the largest *contiguous*
  // block, since that is what caps any single allocation.
  Serial.printf("Largest block  : %lu bytes  <-- caps any single alloc\n",
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.printf("PSRAM          : %s\n",
                ESP.getPsramSize() > 0 ? "present" : "NONE (as expected)");
}

// ---------------------------------------------------------------------------
// Panel identification
// ---------------------------------------------------------------------------

/**
 * Read the panel's ID registers to identify the controller.
 *
 * Read-back arrives over MISO on GPIO12, a live strapping pin, and is known to
 * fail on this unit. All-zeroes therefore means "could not tell", never "wrong
 * panel" — the on-screen pattern is the authoritative check.
 */
bool identifyPanel() {
  Serial.println();
  Serial.println(F("=== Panel identification ==="));
  const uint8_t d3[3] = {tft.readcommand8(0xD3, 1), tft.readcommand8(0xD3, 2),
                         tft.readcommand8(0xD3, 3)};
  Serial.printf("0xD3 (RDDID4)  : %02X %02X %02X\n", d3[0], d3[1], d3[2]);

  const bool isIli9341 = (d3[1] == 0x93 && d3[2] == 0x41);
  Serial.println(isIli9341
                     ? F("VERDICT: ILI9341 confirmed.")
                     : F("VERDICT: read-back unavailable (expected here) --"
                         " judged visually instead."));
  return isIli9341;
}

// ---------------------------------------------------------------------------
// Backlight and LED
// ---------------------------------------------------------------------------

void backlightBegin() {
  ledcSetup(board::kBacklightChannel, board::kBacklightFreqHz,
            board::kBacklightBits);
  ledcAttachPin(board::kPinBacklight, board::kBacklightChannel);
  ledcWrite(board::kBacklightChannel, 0);  // Start dark: no boot flash.
}

void backlightSet(uint8_t percent) {
  ledcWrite(board::kBacklightChannel,
            (static_cast<uint32_t>(min<uint8_t>(percent, 100)) *
             board::kBacklightMaxDuty) / 100);
}

// The RGB LED is active LOW; these wrappers exist so no caller must remember.
void ledBegin() {
  for (const uint8_t pin :
       {board::kPinLedRed, board::kPinLedGreen, board::kPinLedBlue}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, board::kLedOff);
  }
}

void ledSet(bool red, bool green, bool blue) {
  digitalWrite(board::kPinLedRed, red ? board::kLedOn : board::kLedOff);
  digitalWrite(board::kPinLedGreen, green ? board::kLedOn : board::kLedOff);
  digitalWrite(board::kPinLedBlue, blue ? board::kLedOn : board::kLedOff);
}

// ---------------------------------------------------------------------------
// Ambient light
// ---------------------------------------------------------------------------

uint16_t readAmbient() {
  constexpr uint8_t kSamples = 16;
  uint32_t total = 0;
  for (uint8_t i = 0; i < kSamples; ++i) {
    total += analogRead(board::kPinLdr);
    delay(2);
  }
  return static_cast<uint16_t>(total / kSamples);
}

// ---------------------------------------------------------------------------
// Touch calibration
// ---------------------------------------------------------------------------

/// Human-readable gesture name, shared by the direction check and the test
/// screen so both describe gestures identically.
const char* gestureName(touch::Gesture g) {
  switch (g) {
    case touch::Gesture::Tap:        return "TAP";
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
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TOUCH CALIBRATION", board::kScreenWidth / 2,
                 board::kScreenHeight / 2 - 26, 4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(prompt, board::kScreenWidth / 2,
                 board::kScreenHeight / 2 + 4, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char step[24];
  snprintf(step, sizeof(step), "target %u of %u", index, total);
  tft.drawString(step, board::kScreenWidth / 2,
                 board::kScreenHeight / 2 + 26, 2);

  tft.drawLine(x - 12, y, x + 12, y, TFT_RED);
  tft.drawLine(x, y - 12, x, y + 12, TFT_RED);
  tft.drawCircle(x, y, 7, TFT_RED);
  tft.fillCircle(x, y, 2, TFT_YELLOW);
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
  while (!touchInput.isPressed()) {
    if (millis() - lastBeat > 2000) {
      lastBeat = millis();
      uint16_t rx = 0, ry = 0, rz = 0;
      touchInput.readRawUnfiltered(rx, ry, rz);
      Serial.printf("waiting for tap...  IRQ=%s  raw=(%4u,%4u) Z=%4u"
                    " (need Z>%u)\n",
                    touchInput.irqAsserted() ? "LOW " : "HIGH", rx, ry, rz,
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
    if (touchInput.getRaw(rx, ry, rz)) {
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
  while (touchInput.isPressed()) delay(10);
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
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("DIRECTION CHECK", board::kScreenWidth / 2,
                 board::kScreenHeight / 2 - 34, 4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(label, board::kScreenWidth / 2, board::kScreenHeight / 2 + 2, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("across the middle of the screen",
                 board::kScreenWidth / 2, board::kScreenHeight / 2 + 34, 2);

  Serial.printf("Please swipe: %s\n", label);

  const uint32_t deadline = millis() + 30000;
  while (millis() < deadline) {
    const touch::Gesture g = touchInput.poll();
    if (g == touch::Gesture::None) {
      delay(8);
      continue;
    }
    const bool ok = (g == expected);
    Serial.printf("  got %s -- %s\n", gestureName(g), ok ? "PASS" : "MISMATCH");
    return ok;
  }
  Serial.println(F("  timed out waiting for a swipe"));
  return false;
}

// ---------------------------------------------------------------------------
// Interactive touch test
// ---------------------------------------------------------------------------

void drawTestScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, board::kScreenWidth, board::kScreenHeight, TFT_DARKGREY);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TOUCH TEST", board::kScreenWidth / 2, 6, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("draw, tap, and swipe", board::kScreenWidth / 2, 30, 2);
}

/// Show the most recent decoded gesture, so swipe thresholds can be felt out.
void showGesture(touch::Gesture g) {
  if (g == touch::Gesture::None) return;
  const char* name = gestureName(g);
  Serial.printf("gesture: %s\n", name);

  tft.setTextDatum(BC_DATUM);
  tft.fillRect(0, board::kScreenHeight - 30, board::kScreenWidth, 28,
               TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString(name, board::kScreenWidth / 2, board::kScreenHeight - 6, 4);

  // A tap clears the sketch area, which doubles as confirming taps are
  // distinguished from swipes.
  if (g == touch::Gesture::Tap) {
    tft.fillRect(1, 44, board::kScreenWidth - 2,
                 board::kScreenHeight - 76, TFT_BLACK);
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);  // Let the CH340 settle so the banner is not lost.

  Serial.println();
  Serial.println(F("############################################"));
  Serial.println(F("  ESP32 Football Tracker - bring-up"));
  Serial.println(F("  Board: ESP32-2432S028R (Cheap Yellow Display)"));
  Serial.println(F("############################################"));

  reportChip();

  ledBegin();
  ledSet(false, false, true);  // Blue: starting up, not yet serving data.

  backlightBegin();
  tft.init();
  tft.setRotation(board::kScreenRotation);
  tft.fillScreen(TFT_BLACK);
  g_panelConfirmed = identifyPanel();
  backlightSet(100);  // Safe to light now the panel is initialised and cleared.

  analogSetPinAttenuation(board::kPinLdr, ADC_11db);
  Serial.println();
  Serial.println(F("=== Ambient light (ADC1) ==="));
  Serial.printf("LDR raw: %u -- a flat 0 means the sensor is unpopulated\n",
                readAmbient());

  touchInput.begin();
  Serial.println();
  Serial.println(F("=== Touch controller ==="));
  Serial.printf("IRQ pin (GPIO%u) idle level: %s\n", board::kPinTouchIrq,
                touchInput.irqAsserted() ? "LOW (asserted?!)" : "HIGH (correct)");

  // Prove the SPI link before asking for any interaction. If these values are
  // all zero or all 4095 the bus is wrong, and no amount of tapping will help.
  Serial.println();
  Serial.println(F("=== Raw channel check (untouched) ==="));
  for (uint8_t i = 0; i < 3; ++i) {
    uint16_t rx = 0, ry = 0, rz = 0;
    touchInput.readRawUnfiltered(rx, ry, rz);
    Serial.printf("  sample %u: X=%4u Y=%4u Z=%4u\n", i + 1, rx, ry, rz);
    delay(120);
  }
  Serial.println(F("  (all-zero or all-4095 would mean a bad SPI link;"));
  Serial.println(F("   small non-zero values are normal when untouched)"));

  touch::Calibration cal;
  if (calibrate(cal)) {
    touchInput.setCalibration(cal);
    g_calibration = cal;
    g_calibrated  = true;
    ledSet(false, true, false);  // Green: bring-up complete.
  } else {
    // Keep the built-in defaults so the test screen is still usable, but make
    // the failure obvious rather than silently shipping a bad mapping.
    Serial.println(F("Calibration failed -- falling back to defaults."));
    ledSet(true, false, false);  // Red: something needs attention.
  }

  // Verify orientation in firmware rather than trusting the fit. The previous
  // bug produced a calibration that looked perfect in every number we printed
  // and was still rotated 90 degrees.
  if (g_calibrated) {
    Serial.println();
    Serial.println(F("=== Direction check ==="));
    const bool right = verifySwipe("SWIPE RIGHT ->", touch::Gesture::SwipeRight);
    const bool down  = verifySwipe("SWIPE DOWN", touch::Gesture::SwipeDown);
    if (right && down) {
      Serial.println(F("Orientation CONFIRMED: swipes match their directions."));
      ledSet(false, true, false);
    } else {
      Serial.println(F("ORIENTATION FAULT: swipes do not match directions."));
      Serial.println(F("The axis mapping is still wrong -- do not trust it."));
      ledSet(true, false, false);
    }
  }

  drawTestScreen();
  Serial.println();
  Serial.println(F("Touch test active. Drag to draw, tap to clear,"));
  Serial.println(F("swipe to see gestures decoded."));
}

void loop() {
  // Live inversion toggle, kept from display bring-up: it removes a whole
  // build-flash-inspect round trip from getting panel config right.
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 'i' || c == 'I') {
      g_inverted = !g_inverted;
      tft.invertDisplay(g_inverted);
      Serial.printf("inversion now %s\n", g_inverted ? "ON" : "OFF");
    }
  }

  // Re-announce the calibration result periodically. Serial output is only
  // seen by whoever is listening at the time, so a one-shot print is lost to
  // any capture that starts even a second late.
  static uint32_t lastAnnounce = 0;
  if (g_calibrated && millis() - lastAnnounce > 5000) {
    lastAnnounce = millis();
    Serial.printf("CALIBRATION swap=%d rawX=[%d..%d] invX=%d"
                  " rawY=[%d..%d] invY=%d\n",
                  g_calibration.swapAxes ? 1 : 0, g_calibration.rawMinX,
                  g_calibration.rawMaxX, g_calibration.invertX ? 1 : 0,
                  g_calibration.rawMinY, g_calibration.rawMaxY,
                  g_calibration.invertY ? 1 : 0);
  }

  // Gesture decoding must be polled steadily or presses are missed.
  showGesture(touchInput.poll());

  // Draw where the finger is. This is the real accuracy check: if the trail
  // does not sit under the fingertip, calibration is wrong in a way no
  // coordinate readout makes as obvious.
  int16_t x = 0, y = 0;
  if (touchInput.isPressed() && touchInput.getPoint(x, y)) {
    if (y > 44 && y < board::kScreenHeight - 32) {
      tft.fillCircle(x, y, 3, TFT_GREEN);
    }
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 150) {
      lastLog = millis();
      uint16_t rx = 0, ry = 0, rz = 0;
      touchInput.getRaw(rx, ry, rz);
      Serial.printf("touch screen=(%3d,%3d)  raw=(%4u,%4u)  pressure=%u\n", x,
                    y, rx, ry, rz);
    }
  }

  delay(8);
}
