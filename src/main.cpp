/**
 * @file main.cpp
 * @brief Entry point for the ESP32 Football Tracker.
 *
 * Brings up the hardware, then hands over to the screen manager, which cycles
 * between screens of information and responds to touch.
 *
 * Data currently comes from a compiled-in placeholder built from real captured
 * API responses. The cache and provider layers will replace it without any
 * screen changing, because screens only ever see a const model::Snapshot.
 *
 * Hardware facts established during bring-up and relied on here:
 *   - The panel is an ILI9341 variant that powers up inverted, corrected by
 *     TFT_INVERSION_ON in platformio.ini.
 *   - The touch digitizer's axes are transposed relative to the display; the
 *     touch layer resolves that, so screens work in plain screen coordinates.
 *   - A full-screen fill costs 31 ms, so full redraws are affordable — and no
 *     framebuffer is needed, there being nowhere near enough RAM for one.
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "model.h"
#include "screen.h"
#include "screen_manager.h"
#include "screens.h"
#include "touch_calibration.h"
#include "touch_input.h"

namespace {

TFT_eSPI          tft;
touch::TouchInput touchInput;

/// How long each screen is shown while cycling. Web-configurable later.
constexpr uint32_t kDwellMs = 12000;

/// The single mutable copy of the data. The refresh scheduler will own writes;
/// screens only ever see it as const.
model::Snapshot g_data;

ui::ScreenManager      g_screens;
ui::LiveMatchScreen    g_liveMatch;
ui::SeasonRecordScreen g_seasonRecord;
ui::LastResultScreen   g_lastResult;
ui::NextFixtureScreen  g_nextFixture;
ui::LeagueTableScreen  g_leagueTable;
ui::TopScorerScreen    g_topScorer;

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

/**
 * Print what the silicon says about itself.
 *
 * The cheapest guard against a swapped board or wrong build target, and it
 * keeps the "no PSRAM" constraint that shapes the whole design visible.
 */
void reportChip() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  Serial.println();
  Serial.println(F("=== Chip ==="));
  Serial.printf("Cores / rev    : %d / %d\n", info.cores, info.revision);
  Serial.printf("CPU frequency  : %lu MHz\n",
                (unsigned long)getCpuFrequencyMhz());
  Serial.printf("Flash          : %lu KB @ %lu MHz\n",
                (unsigned long)(ESP.getFlashChipSize() / 1024),
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  Serial.printf("Sketch size    : %lu KB\n",
                (unsigned long)(ESP.getSketchSize() / 1024));
  Serial.printf("Free heap      : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  // The largest contiguous block is what caps any single allocation, and is
  // therefore the number that actually constrains us.
  Serial.printf("Largest block  : %lu bytes\n",
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.printf("PSRAM          : %s\n",
                ESP.getPsramSize() > 0 ? "present" : "NONE (as expected)");
}

/// Backlight on its own LEDC channel so it can be dimmed — the largest single
/// power saving available to us.
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

}  // namespace

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);  // Let the CH340 settle so the banner is not lost.

  Serial.println();
  Serial.println(F("############################################"));
  Serial.println(F("  ESP32 Football Tracker"));
  Serial.println(F("  Board: ESP32-2432S028R (Cheap Yellow Display)"));
  Serial.println(F("############################################"));

  reportChip();

  ledBegin();
  ledSet(false, false, true);  // Blue: starting up, not yet serving data.

  backlightBegin();
  tft.init();
  tft.setRotation(board::kScreenRotation);
  tft.fillScreen(ui::colour::kBackground);
  backlightSet(100);  // Safe to light now the panel is initialised and cleared.

  touchInput.begin();

  // Calibration is not run on every boot. The compiled-in defaults are the
  // verified fit for this unit, and forcing a three-tap ritual before the
  // device becomes useful would be the wrong trade. Holding BOOT through
  // startup asks for it explicitly; once NVS config exists, stored calibration
  // takes over and the web UI gains a "recalibrate" action.
  pinMode(board::kPinBootButton, INPUT_PULLUP);
  if (digitalRead(board::kPinBootButton) == LOW) {
    Serial.println(F("BOOT held -- running touch calibration."));
    touch::Calibration cal;
    if (touch::runCalibration(tft, touchInput, cal)) {
      touchInput.setCalibration(cal);
      touch::verifyOrientation(tft, touchInput);
    } else {
      Serial.println(F("Calibration failed -- keeping compiled-in defaults."));
    }
  } else {
    Serial.println(F("Using compiled-in touch calibration."));
    Serial.println(F("Hold BOOT during reset to recalibrate."));
  }

  // Placeholder data stands in for the cache and providers, which do not exist
  // yet. It is real captured data, so layout is tested against genuine club
  // names and figures rather than convenient invented ones.
  model::loadPlaceholder(g_data);
  Serial.printf("Loaded %u table rows; our team at row %u\n", g_data.tableRows,
                g_data.ourRow);

  // Registration order is display order.
  g_screens.add(&g_liveMatch);
  g_screens.add(&g_seasonRecord);
  g_screens.add(&g_lastResult);
  g_screens.add(&g_nextFixture);
  g_screens.add(&g_leagueTable);
  g_screens.add(&g_topScorer);
  g_screens.begin(tft, g_data, kDwellMs);

  ledSet(false, true, false);  // Green: running.
  Serial.println();
  Serial.println(F("Running. Swipe left/right to change screen, tap to hold"));
  Serial.println(F("or resume cycling, swipe up/down in the table to scroll."));
}

void loop() {
  // One gesture poll per iteration feeds both the manager and its screens.
  const touch::Gesture gesture = touchInput.poll();
  if (gesture != touch::Gesture::None) {
    Serial.printf("gesture: %s%s\n", touch::gestureName(gesture),
                  g_screens.isPinned() ? "  [held]" : "");
  }
  g_screens.handleGesture(gesture);
  g_screens.tick();

  // 8 ms keeps gesture decoding responsive without spinning the CPU. The power
  // phase replaces this with a light-sleep wait on the touch IRQ, since a
  // static screen needs no polling at all.
  delay(8);
}
