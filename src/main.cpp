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

#include <time.h>

#include "board_config.h"
#include "model.h"
#include "network.h"
#include "screen.h"
#include "screen_manager.h"
#include "screens.h"
#include "settings_menu.h"
#include "store.h"
#include "touch_calibration.h"
#include "web_portal.h"
#include "touch_input.h"

namespace {

TFT_eSPI          tft;
touch::TouchInput touchInput;

/// Loaded from NVS; the struct's own defaults apply on a first boot.
store::Settings g_settings;

/**
 * Whether to exercise the storage layer on boot.
 *
 * On while the storage layer is new. It is the only way to verify atomic
 * writes and freshness tracking on the real filesystem — there is no host-side
 * test harness for LittleFS on this board — and it costs one write cycle per
 * boot, which is nothing against LittleFS wear levelling.
 */
constexpr bool kRunStorageSelfTest = true;

/// The single mutable copy of the data. The refresh scheduler will own writes;
/// screens only ever see it as const.
model::Snapshot g_data;

/// True while the device is running the setup access point, in which case the
/// carousel is not running at all — setup is a mode, not a screen.
bool g_setupMode = false;

ui::SettingsMenu       g_menu;
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

/**
 * Prove the cache round-trips on the real filesystem.
 *
 * Checks the parts that are easy to get subtly wrong: that a write is
 * immediately readable, that freshness is reported correctly, that a
 * zero-TTL document reports stale rather than fresh, and that a document too
 * large for the caller's buffer is refused instead of silently truncated.
 */
void storageSelfTest() {
  if (!store::ready()) {
    Serial.println(F("[selftest] store not ready -- skipping"));
    return;
  }

  Serial.println();
  Serial.println(F("=== Storage self-test ==="));

  const char* payload = "{\"t\":\"selftest\",\"rows\":24}";
  const size_t len = strlen(payload);

  // Write, then read back.
  if (!store::writeDoc(store::Doc::LiveMatch, payload, len, 3600)) {
    Serial.println(F("[selftest] FAIL: write rejected"));
    return;
  }

  char buf[64] = {0};
  const size_t read = store::readDoc(store::Doc::LiveMatch, buf, sizeof(buf));
  const bool roundTripped = (read == len) && (strcmp(buf, payload) == 0);
  Serial.printf("[selftest] round-trip: %s (%u bytes)\n",
                roundTripped ? "PASS" : "FAIL", (unsigned)read);

  // Freshness, with a generous TTL. Note that before NTP the clock is unset,
  // so freshness cannot be judged and the store deliberately reports stale —
  // called out here so a fresh=0 does not read as a failure.
  const bool clockSet = time(nullptr) > 1600000000L;
  store::DocStatus st = store::statusOf(store::Doc::LiveMatch);
  Serial.printf("[selftest] present=%d fresh=%d size=%lu ttl=%lu\n",
                st.present, st.fresh, (unsigned long)st.size,
                (unsigned long)st.ttl);
  Serial.printf("[selftest] clock %s -- fresh=%d is %s here\n",
                clockSet ? "set" : "NOT set (pre-NTP)", st.fresh,
                clockSet ? "meaningful" : "expected: freshness needs a clock");

  // A buffer deliberately too small must be refused, not truncated.
  char tiny[8] = {0};
  const size_t refused = store::readDoc(store::Doc::LiveMatch, tiny, sizeof(tiny));
  Serial.printf("[selftest] oversize read refused: %s\n",
                refused == 0 ? "PASS" : "FAIL");

  // Zero TTL must read as stale immediately.
  store::writeDoc(store::Doc::LiveMatch, payload, len, 0);
  st = store::statusOf(store::Doc::LiveMatch);
  Serial.printf("[selftest] zero-ttl reports stale: %s\n",
                st.present && !st.fresh ? "PASS" : "FAIL");

  // Leave nothing behind: this is a placeholder document, not real data.
  store::clearDoc(store::Doc::LiveMatch);
  st = store::statusOf(store::Doc::LiveMatch);
  Serial.printf("[selftest] cleared: %s\n", !st.present ? "PASS" : "FAIL");

  uint32_t used = 0, total = 0;
  store::filesystemUsage(used, total);
  Serial.printf("[selftest] filesystem %lu KB used of %lu KB\n",
                (unsigned long)(used / 1024), (unsigned long)(total / 1024));
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

  // Storage before anything that depends on configuration. A failure here is
  // not fatal: the device runs from live fetches with defaults.
  store::begin();
  store::loadSettings(g_settings);
  if (kRunStorageSelfTest) storageSelfTest();

  backlightSet(g_settings.brightness);

  touchInput.begin();
  // Apply stored calibration if this device has been calibrated; otherwise the
  // compiled-in reference values stand in.
  if (g_settings.touch.valid) {
    touchInput.setCalibration(g_settings.touch);
    Serial.println(F("Using stored touch calibration from NVS."));
  }

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
      // Persist before verifying: the calibration is worth keeping even if the
      // operator walks away before completing the direction check.
      g_settings.touch = cal;
      store::saveSettings(g_settings);
      touch::verifyOrientation(tft, touchInput);
    } else {
      Serial.println(F("Calibration failed -- keeping compiled-in defaults."));
    }
  } else if (!g_settings.touch.valid) {
    Serial.println(F("Using compiled-in touch calibration."));
    Serial.println(F("Hold BOOT during reset to recalibrate."));
  }

  // Networking. A held BOOT button also forces setup mode, so a device joined
  // to a network the user no longer has is recoverable without a serial cable.
  const bool forceSetup = (digitalRead(board::kPinBootButton) == LOW);
  ui::drawStatusScreen(tft, "Connecting...", g_settings.wifiSsid,
                       ui::colour::kAccent);
  net::begin(g_settings, forceSetup);

  g_setupMode = (net::status().mode == net::Mode::AccessPoint);
  web::begin(g_settings, g_data, g_setupMode);

  if (g_setupMode) {
    // Show the credentials on the panel and stop here: there is nothing
    // meaningful to display until the device is configured.
    ledSet(false, false, true);  // Blue: awaiting configuration.
    ui::drawSetupScreen(tft, net::apSsid(), net::apPassword(),
                        net::portalUrl());
    Serial.println();
    Serial.println(F("Setup mode. Join the access point shown on screen."));
    return;
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
  g_menu.begin(g_settings);
  g_screens.begin(tft, g_data, g_settings.screenDwellMs);

  ledSet(false, true, false);  // Green: running.
  Serial.println();
  Serial.println(F("Running. Swipe left/right to change screen,"));
  Serial.println(F("DOUBLE-TAP to hold or resume cycling, and swipe"));
  Serial.println(F("up/down in the table to scroll."));
}

void loop() {
  // Networking and the web server are pumped in both modes.
  net::tick();
  web::tick();

  if (g_setupMode) {
    // Restart once credentials are in, so the join happens from a clean
    // radio state rather than by reconfiguring a running AP.
    if (web::credentialsSubmitted()) {
      Serial.println(F("Credentials received -- restarting"));
      ui::drawStatusScreen(tft, "Saved", "restarting...", ui::colour::kWin);
      delay(1500);  // Let the browser receive the confirmation page first.
      ESP.restart();
    }
    delay(10);
    return;
  }

  // One gesture poll per iteration feeds the menu, the manager and its
  // screens — in that order of priority.
  const touch::Gesture gesture = touchInput.poll();
  if (gesture != touch::Gesture::None) {
    Serial.printf("gesture: %s%s\n", touch::gestureName(gesture),
                  g_screens.isPinned() ? "  [held]" : "");
  }

  // --- Settings menu ------------------------------------------------------
  if (g_menu.isOpen()) {
    const bool stillOpen = g_menu.handleGesture(
        tft, gesture, touchInput.gestureX(), touchInput.gestureY());

    // Destructive actions are applied here rather than inside the menu, so
    // the menu stays a pure UI component with no power to reboot the device
    // on its own.
    switch (g_menu.takeAction()) {
      case ui::SettingsMenu::Action::ResetWifi: {
        Serial.println(F("[menu] clearing Wi-Fi credentials"));
        g_settings.wifiSsid[0] = '\0';
        g_settings.wifiPass[0] = '\0';
        store::saveSettings(g_settings);
        ui::drawStatusScreen(tft, "Wi-Fi reset", "restarting into setup...",
                             ui::colour::kDraw);
        delay(1500);
        ESP.restart();
        break;
      }
      case ui::SettingsMenu::Action::FactoryReset:
        Serial.println(F("[menu] factory reset"));
        store::factoryReset();
        ui::drawStatusScreen(tft, "Factory reset", "restarting...",
                             ui::colour::kLoss);
        delay(1500);
        ESP.restart();
        break;
      case ui::SettingsMenu::Action::None:
        break;
    }

    if (!stillOpen) {
      // Returning to the carousel redraws it in full: the menu overwrote the
      // whole panel, chrome included.
      Serial.println(F("[menu] closed"));
      g_screens.refresh();
    }
    delay(8);
    return;
  }

  // A long press opens the menu instead of reaching the carousel.
  if (gesture == touch::Gesture::LongPress) {
    Serial.println(F("[menu] opened by long press"));
    g_menu.open(tft);
    delay(8);
    return;
  }

  g_screens.handleGesture(gesture);
  g_screens.tick();

  // 8 ms keeps gesture decoding responsive without spinning the CPU. The power
  // phase replaces this with a light-sleep wait on the touch IRQ, since a
  // static screen needs no polling at all.
  delay(8);
}
