/**
 * @file main.cpp
 * @brief Hardware bring-up diagnostic for the ESP32 Football Tracker.
 *
 * This is the first firmware flashed to the board, and its job is to answer
 * the open hardware questions in SPEC.md §13 rather than to look pretty:
 *
 *   1. Which panel controller is fitted? TFT_eSPI selects its driver at
 *      compile time, so the factory image told us nothing. We read the
 *      panel's own ID registers and print them.
 *   2. Is the colour order RGB or BGR? A mis-set order shows as red and blue
 *      swapped, which is trivial to see on a colour-bar pattern and a
 *      one-flag fix.
 *   3. Is there a row/column offset? ST7789 and ILI9341 differ here, and a
 *      1px border reveals it immediately.
 *   4. Do the peripherals the power and UX plans depend on actually work —
 *      backlight PWM dimming, the ADC1 light sensor, and the RGB LED?
 *
 * It also prints a chip report so the claims in docs/HARDWARE.md can be
 * checked against the running silicon rather than trusted.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "board_config.h"

namespace {

TFT_eSPI tft;

// ---------------------------------------------------------------------------
// Chip report
// ---------------------------------------------------------------------------

/**
 * Print what the silicon says about itself.
 *
 * Worth doing on every boot during development: it is the cheapest possible
 * guard against a swapped board or a wrong build target, and it confirms the
 * "no PSRAM" constraint that shapes the whole design.
 */
void reportChip() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  Serial.println();
  Serial.println(F("=== Chip ==="));
  Serial.printf("Model          : %s\n",
                info.model == CHIP_ESP32 ? "ESP32" : "other");
  Serial.printf("Cores          : %d\n", info.cores);
  Serial.printf("Silicon rev    : %d\n", info.revision);
  Serial.printf("CPU frequency  : %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
  Serial.printf("Features       : WiFi%s%s\n",
                (info.features & CHIP_FEATURE_BT) ? " + BT" : "",
                (info.features & CHIP_FEATURE_BLE) ? " + BLE" : "");
  Serial.printf("Flash size     : %lu KB\n",
                (unsigned long)(ESP.getFlashChipSize() / 1024));
  Serial.printf("Flash speed    : %lu MHz\n",
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  // getFreeSketchSpace() reports the size of the *next OTA slot*, not the
  // headroom in the running partition — printing them added together would
  // imply a partition size that does not exist.
  Serial.printf("Sketch size    : %lu KB\n",
                (unsigned long)(ESP.getSketchSize() / 1024));
  Serial.printf("OTA slot free  : %lu KB (the other app partition)\n",
                (unsigned long)(ESP.getFreeSketchSpace() / 1024));

  // The number that matters most. Every design decision about JSON parsing
  // and framebuffers follows from how little of this we have.
  Serial.println();
  Serial.println(F("=== Memory ==="));
  Serial.printf("Free heap      : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  // The real constraint is not total free heap but the largest *contiguous*
  // block, since that is what caps any single allocation.
  Serial.printf("Largest block  : %lu bytes  <-- caps any single alloc\n",
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.printf("PSRAM          : %s\n",
                ESP.getPsramSize() > 0 ? "present" : "NONE (as expected)");

  // A full 320x240x16-bit framebuffer would need this much. Printed as a
  // standing reminder of why we draw through a small sprite instead.
  constexpr uint32_t kFullFramebuffer =
      static_cast<uint32_t>(board::kScreenWidth) * board::kScreenHeight * 2;
  Serial.printf("Full framebuffer would cost %lu bytes — not affordable\n",
                (unsigned long)kFullFramebuffer);
}

// ---------------------------------------------------------------------------
// Panel identification
// ---------------------------------------------------------------------------

/**
 * Read the panel's ID registers to identify the controller.
 *
 * ILI9341 answers command 0xD3 (RDDID4) with 0x00 0x93 0x41 — the "9341" is
 * legible in the last two bytes. ST7789 does not implement 0xD3 and reports a
 * different signature on 0x04 (RDDID). We print both so the result is
 * interpretable even if neither matches expectations.
 *
 * Caveat, and the reason we do not treat a failed read as proof of anything:
 * panel read-back arrives over MISO on GPIO12, which is a live strapping pin
 * on this board. Reads are known to be unreliable on some units. So a
 * successful read is strong evidence, while all-zeroes or all-0xFF means
 * "could not tell" — not "wrong panel". The colour-bar test below is the
 * authoritative check either way, because it needs no read-back at all.
 *
 * @return true if the signature positively identifies an ILI9341.
 */
bool identifyPanel() {
  Serial.println();
  Serial.println(F("=== Panel identification ==="));

  const uint8_t d3[3] = {
      tft.readcommand8(0xD3, 1),
      tft.readcommand8(0xD3, 2),
      tft.readcommand8(0xD3, 3),
  };
  const uint8_t rddid[3] = {
      tft.readcommand8(0x04, 1),
      tft.readcommand8(0x04, 2),
      tft.readcommand8(0x04, 3),
  };

  Serial.printf("0xD3 (RDDID4)  : %02X %02X %02X\n", d3[0], d3[1], d3[2]);
  Serial.printf("0x04 (RDDID)   : %02X %02X %02X\n", rddid[0], rddid[1], rddid[2]);

  const bool isIli9341 = (d3[1] == 0x93 && d3[2] == 0x41);
  const bool readFailed =
      (d3[0] == 0x00 && d3[1] == 0x00 && d3[2] == 0x00) ||
      (d3[0] == 0xFF && d3[1] == 0xFF && d3[2] == 0xFF);

  if (isIli9341) {
    Serial.println(F("VERDICT: ILI9341 confirmed — build flags are correct."));
  } else if (readFailed) {
    Serial.println(F("VERDICT: read-back unavailable (expected on some units)."));
    Serial.println(F("         Judge by the colour bars on screen instead."));
  } else {
    Serial.println(F("VERDICT: unexpected signature — likely ST7789."));
    Serial.println(F("         Swap ILI9341_2_DRIVER for ST7789_DRIVER in"));
    Serial.println(F("         platformio.ini and reflash."));
  }
  return isIli9341;
}

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------

/// Attach the backlight to its LEDC channel. Starts dark to avoid a bright
/// flash of uninitialised panel memory at boot.
void backlightBegin() {
  ledcSetup(board::kBacklightChannel, board::kBacklightFreqHz,
            board::kBacklightBits);
  ledcAttachPin(board::kPinBacklight, board::kBacklightChannel);
  ledcWrite(board::kBacklightChannel, 0);
}

/// @param percent 0 = off, 100 = full.
void backlightSet(uint8_t percent) {
  const uint32_t duty =
      (static_cast<uint32_t>(min<uint8_t>(percent, 100)) *
       board::kBacklightMaxDuty) / 100;
  ledcWrite(board::kBacklightChannel, duty);
}

// ---------------------------------------------------------------------------
// Status LED
// ---------------------------------------------------------------------------
//
// The RGB LED is active LOW, so these wrappers exist purely so no caller has
// to remember that.

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

/// Cycle each channel so a wiring or polarity mistake is visible at a glance.
void ledSelfTest() {
  Serial.println();
  Serial.println(F("=== RGB LED self-test ==="));
  const struct { const char* name; bool r, g, b; } steps[] = {
      {"red", true, false, false},
      {"green", false, true, false},
      {"blue", false, false, true},
      {"white", true, true, true},
  };
  for (const auto& s : steps) {
    Serial.printf("  %s\n", s.name);
    ledSet(s.r, s.g, s.b);
    delay(350);
  }
  ledSet(false, false, false);
}

// ---------------------------------------------------------------------------
// Ambient light sensor
// ---------------------------------------------------------------------------

/**
 * Sample the LDR and report it.
 *
 * Averaged over several reads because the raw ESP32 ADC is noisy; the
 * auto-brightness feature will need this smoothing anyway, so it is proven
 * here. A reading that never changes when the sensor is covered means
 * auto-brightness is not viable and the feature should be dropped from §9.
 */
uint16_t readAmbient() {
  constexpr uint8_t kSamples = 16;
  uint32_t total = 0;
  for (uint8_t i = 0; i < kSamples; ++i) {
    total += analogRead(board::kPinLdr);
    delay(2);
  }
  return static_cast<uint16_t>(total / kSamples);
}

/// Same reading expressed in millivolts, which distinguishes "dark room" from
/// "pin sitting at 0 V because nothing is attached to it".
uint32_t readAmbientMillivolts() {
  constexpr uint8_t kSamples = 16;
  uint32_t total = 0;
  for (uint8_t i = 0; i < kSamples; ++i) {
    total += analogReadMilliVolts(board::kPinLdr);
    delay(2);
  }
  return total / kSamples;
}

// ---------------------------------------------------------------------------
// Display test pattern
// ---------------------------------------------------------------------------

/**
 * Draw the pattern that settles colour order and screen offset.
 *
 * Deliberately labelled on-screen: the whole point is that someone looking at
 * the board can tell instantly whether the bar under the word "RED" is
 * actually red. If red and blue are swapped, add -D TFT_RGB_ORDER=TFT_BGR.
 */
void drawTestPattern(bool panelConfirmed) {
  tft.fillScreen(TFT_BLACK);

  // A 1px border touching all four edges. If any edge is missing or clipped,
  // the driver's row/column offset is wrong for this panel.
  tft.drawRect(0, 0, board::kScreenWidth, board::kScreenHeight, TFT_WHITE);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("FOOTBALL TRACKER", board::kScreenWidth / 2, 8, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("hardware bring-up", board::kScreenWidth / 2, 32, 2);

  // Colour bars with their expected names underneath.
  const struct { const char* name; uint16_t colour; } bars[] = {
      {"RED", TFT_RED},
      {"GRN", TFT_GREEN},
      {"BLU", TFT_BLUE},
      {"WHT", TFT_WHITE},
  };
  constexpr int16_t kBarTop    = 56;
  constexpr int16_t kBarHeight = 60;
  const int16_t barWidth = (board::kScreenWidth - 20) / 4;

  for (int i = 0; i < 4; ++i) {
    const int16_t x = 10 + i * barWidth;
    tft.fillRect(x, kBarTop, barWidth - 4, kBarHeight, bars[i].colour);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(bars[i].name, x + (barWidth - 4) / 2,
                   kBarTop + kBarHeight + 4, 2);
  }

  // Verdict line, so the screen alone tells the story.
  tft.setTextDatum(TC_DATUM);
  if (panelConfirmed) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("ILI9341 confirmed", board::kScreenWidth / 2, 148, 2);
  } else {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("panel ID unread - check bars", board::kScreenWidth / 2, 148, 2);
  }
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("If RED and BLU are swapped,", board::kScreenWidth / 2, 168, 2);
  tft.drawString("set TFT_RGB_ORDER=TFT_BGR", board::kScreenWidth / 2, 186, 2);
}

/**
 * Time a series of full-screen fills.
 *
 * Establishes the SPI throughput baseline we will measure future rendering
 * against, and confirms DMA-free blocking writes are already fast enough that
 * the UI will not feel sluggish.
 */
void benchmarkFill() {
  Serial.println();
  Serial.println(F("=== Fill benchmark ==="));
  const uint16_t colours[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_BLACK};
  const uint32_t start = millis();
  constexpr uint8_t kRounds = 4;
  for (uint8_t r = 0; r < kRounds; ++r) {
    for (const uint16_t c : colours) tft.fillScreen(c);
  }
  const uint32_t elapsed = millis() - start;
  const uint32_t fills = kRounds * 4;
  const uint32_t pixels = fills * board::kScreenWidth * board::kScreenHeight;

  Serial.printf("%lu full-screen fills in %lu ms (%.1f ms each)\n",
                (unsigned long)fills, (unsigned long)elapsed,
                static_cast<double>(elapsed) / fills);
  Serial.printf("~%.1f Mpixel/s => a full redraw costs ~%.1f ms\n",
                (pixels / 1000000.0) / (elapsed / 1000.0),
                static_cast<double>(elapsed) / fills);
}

}  // namespace

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // Give the CH340 a moment to come up so the banner is not lost. Not a
  // correctness requirement — purely so first-boot output is readable.
  delay(300);

  Serial.println();
  Serial.println(F("############################################"));
  Serial.println(F("  ESP32 Football Tracker - bring-up"));
  Serial.println(F("  Board: ESP32-2432S028R (Cheap Yellow Display)"));
  Serial.println(F("############################################"));

  reportChip();

  ledBegin();
  // Blue while starting up: the same convention the finished firmware uses to
  // mean "not yet serving data".
  ledSet(false, false, true);

  backlightBegin();

  tft.init();
  tft.setRotation(board::kScreenRotation);
  tft.fillScreen(TFT_BLACK);

  const bool panelConfirmed = identifyPanel();

  // Panel is initialised and cleared, so it is now safe to light the screen.
  backlightSet(100);

  benchmarkFill();
  drawTestPattern(panelConfirmed);

  // 11 dB attenuation gives the full ~0-3.3 V input range. Set explicitly
  // rather than relying on the default, so the reading means the same thing
  // across core versions.
  analogSetPinAttenuation(board::kPinLdr, ADC_11db);

  Serial.println();
  Serial.println(F("=== Ambient light (ADC1) ==="));
  Serial.printf("LDR raw: %u  (%lu mV)\n", readAmbient(),
                (unsigned long)readAmbientMillivolts());
  Serial.println(F("Shine a torch at the sensor: the value must move."));
  Serial.println(F("A reading pinned at 0 mV in a lit room means the LDR"));
  Serial.println(F("position is unpopulated on this unit."));

  ledSelfTest();

  // Green: bring-up finished without hanging.
  ledSet(false, true, false);

  Serial.println();
  Serial.println(F("Bring-up complete. Backlight will now ramp to prove"));
  Serial.println(F("PWM dimming works — this is the basis of the power plan."));
}

void loop() {
  // Ramp the backlight up and down continuously. Two purposes: it proves LEDC
  // dimming is smooth and flicker-free (the largest single power saving
  // available to us), and it gives an unmistakable "the firmware is alive"
  // signal without needing the serial monitor attached.
  static uint8_t percent = 100;
  static int8_t  step    = -2;

  backlightSet(percent);
  percent = static_cast<uint8_t>(percent + step);
  if (percent <= 10 || percent >= 100) step = -step;

  // Report ambient light once a second so auto-brightness behaviour can be
  // eyeballed against real room lighting.
  static uint32_t lastReport = 0;
  static uint16_t ambientMin = 0xFFFF;
  static uint16_t ambientMax = 0;
  if (millis() - lastReport > 1000) {
    lastReport = millis();
    const uint16_t raw = readAmbient();
    ambientMin = min(ambientMin, raw);
    ambientMax = max(ambientMax, raw);
    // The min/max range is the diagnostic that matters: a range of zero after
    // the sensor has been lit and shaded means auto-brightness is not viable
    // on this unit and the feature must come out of the power plan.
    Serial.printf("ambient=%-5u range=[%u..%u] %lu mV  backlight=%u%%  heap=%lu\n",
                  raw, ambientMin, ambientMax,
                  (unsigned long)readAmbientMillivolts(), percent,
                  (unsigned long)ESP.getFreeHeap());
  }

  delay(30);
}
