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
#include "api_client.h"
#include "network.h"
#include "ota.h"
#include "power.h"
#include "providers.h"
#include "refresh.h"
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

/// Print club name widths at boot. Only needed when changing the fixture
/// layout or the font it uses.
constexpr bool kMeasureNameWidths = false;

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
  const uint8_t clamped = min<uint8_t>(percent, 100);
  ledcWrite(board::kBacklightChannel,
            (static_cast<uint32_t>(clamped) * board::kBacklightMaxDuty) / 100);
  power::noteBacklight(clamped);
}

// --- Inactivity dimming -----------------------------------------------------
//
// The backlight is the largest single consumer, so dimming it when nobody is
// looking is the cheapest real saving available — and unlike deep sleep it
// costs nothing in responsiveness, because the panel keeps its image and a
// touch restores full brightness instantly.

/// Dim after this long without a touch.
constexpr uint32_t kDimAfterMs = 120000;
/// Duty while dimmed. Not zero: the screen stays readable, which is the whole
/// point of a display you glance at, and a dark-but-visible panel draws a
/// fraction of a bright one.
constexpr uint8_t kDimmedPercent = 15;

uint32_t g_lastTouchAt = 0;
bool     g_dimmed      = false;

/// Apply or lift dimming based on how long since the last touch.
void updateDimming() {
  const bool shouldDim = (millis() - g_lastTouchAt) > kDimAfterMs;
  if (shouldDim == g_dimmed) return;
  g_dimmed = shouldDim;

  // Never dim brighter than the configured level: the setting is a ceiling.
  const uint8_t target =
      shouldDim ? min<uint8_t>(kDimmedPercent, g_settings.brightness)
                : g_settings.brightness;
  backlightSet(target);
  // Stop the per-second live refreshes while dimmed; rotation continues.
  g_screens.setLowPower(shouldDim);
  // Release the UI's speed claim when dimming, retake it when waking. The
  // fetch task's own claims are independent, so a refresh happening while the
  // screen is dark still runs at full speed.
  // Frequency scaling is deliberately NOT applied here. See power.h: changing
  // the CPU clock at runtime with Wi-Fi associated made the web interface
  // unreliable, and it bought nothing measurable anyway.
  if (!shouldDim) g_screens.refresh();  // Waking gets a clean, current screen.
  Serial.printf("[power] backlight %s (%u%%)\n",
                shouldDim ? "dimmed" : "restored", target);
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
 * Check the version rules against a table of cases.
 *
 * These two functions decide whether firmware installs, and getting either
 * wrong is expensive in both directions — a device that refuses genuine
 * updates, or one that installs a release candidate on its own. Run on the
 * target rather than the host so it exercises the code that actually ships,
 * including this compiler's sscanf.
 */
void versionSelfTest() {
  Serial.println();
  Serial.println(F("=== Version rules self-test ==="));
  uint8_t failures = 0;

  struct StableCase { const char* v; bool stable; };
  static const StableCase kStable[] = {
      {"0.2.0", true},        {"v0.2.0", true},
      {"1.10.3", true},       {"0.0.0", true},
      {"0.2.0-rc1", false},   {"0.2.0-alpha2", false},
      {"0.2.0-beta", false},  {"v1.0.0-rc.1", false},
      // Build metadata is not a stable release identifier either: only a
      // published X.Y.Z should ever be installed.
      {"0.2.0+4.gabc123", false},
      {"0.2", false},         {"0.2.0.1", false},
      {"", false},            {"garbage", false},
  };
  for (const StableCase& c : kStable) {
    const bool got = ota::isStableVersion(c.v);
    if (got != c.stable) {
      Serial.printf("  FAIL isStable(\"%s\") = %d, want %d\n", c.v, got,
                    c.stable);
      ++failures;
    }
  }

  struct CompareCase { const char* a; const char* b; int sign; };
  static const CompareCase kCompare[] = {
      {"0.2.0", "0.1.0", 1},
      {"0.1.0", "0.2.0", -1},
      {"1.0.0", "0.9.9", 1},
      {"0.2.0", "0.2.0", 0},
      // Numeric, not lexical: a string compare would put 0.9.0 above 0.10.0.
      {"0.10.0", "0.9.0", 1},
      {"1.2.10", "1.2.9", 1},
      // A pre-release ranks below the plain release of the same triple, so a
      // device on a release candidate is offered the finished version.
      {"0.2.0", "0.2.0-rc1", 1},
      {"0.2.0-rc1", "0.2.0", -1},
      {"0.2.0-rc1", "0.2.0-rc2", 0},  // Both pre-release; triples equal.
      // Build metadata never affects precedence, which is what stops a local
      // dirty build looking newer than the release it follows.
      {"0.2.0+4.gabc123", "0.2.0", 0},
      {"v0.3.0", "0.2.0", 1},
  };
  for (const CompareCase& c : kCompare) {
    const int got = ota::compareVersions(c.a, c.b);
    const int gotSign = (got > 0) ? 1 : (got < 0 ? -1 : 0);
    if (gotSign != c.sign) {
      Serial.printf("  FAIL compare(\"%s\",\"%s\") = %d, want sign %d\n",
                    c.a, c.b, got, c.sign);
      ++failures;
    }
  }

  const uint8_t total = (sizeof(kStable) / sizeof(kStable[0])) +
                        (sizeof(kCompare) / sizeof(kCompare[0]));
  Serial.printf("[selftest] version rules: %u of %u passed\n",
                total - failures, total);
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
  if (kRunStorageSelfTest) {
    storageSelfTest();
    versionSelfTest();
  }

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
  // Start fetching. The scheduler waits for the clock on its own, since TLS
  // certificate validation needs one, so there is nothing to sequence here.
  refresh::begin(g_settings, g_data);

  g_menu.begin(g_settings);
  // The live match screen takes priority whenever a match is in progress: the
  // device returns to it after the dwell rather than continuing the carousel.
  g_screens.setPriority(&g_liveMatch);
  g_screens.setEnabledMask(g_settings.screenMask);
  g_screens.begin(tft, g_data, g_settings.screenDwellMs);

  // Club name widths, off by default. Kept because the layout constants in
  // screens.cpp are derived from these numbers, so anyone changing the
  // columns or the font can re-measure rather than guess — guessing is what
  // truncated a name for the sake of one pixel.
  if (kMeasureNameWidths) {
    Serial.println();
    Serial.println(F("=== Club name widths (Font 2) ==="));
    static const char* kNames[] = {
        "Bolton Wanderers", "Wolverhampton Wanderers", "Cardiff City",
        "West Ham United", "Queens Park Rangers", "Sheffield United",
        "Preston North End", "Norwich City", "Stoke City", "Watford",
    };
    for (const char* n : kNames) {
      Serial.printf("  %-24s %3d px\n", n, tft.textWidth(n, 2));
    }
    Serial.printf("  --- score \"2-3\" in Font 4: %d px\n",
                  tft.textWidth("2-3", 4));
    Serial.printf("  --- \"v\" in Font 4: %d px\n", tft.textWidth("v", 4));
  }

  power::begin();
  power::reportSleepCapabilities();
  power::noteBacklight(g_settings.brightness);
  g_lastTouchAt = millis();

  ledSet(false, true, false);  // Green: running.
  Serial.println();
  Serial.println(F("Running. Swipe left/right to change screen,"));
  Serial.println(F("DOUBLE-TAP to hold or resume cycling, and swipe"));
  Serial.println(F("up/down in the table to scroll."));
}

void loop() {
  const uint32_t loopStart = millis();

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
  // Any touch at all counts as activity, including one that decodes to no
  // gesture — someone prodding the screen wants the light on, whether or not
  // the prod resolved into anything.
  if (gesture != touch::Gesture::None || touchInput.isPressed()) {
    g_lastTouchAt = millis();
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

  // Adopt any completed fetch between frames, which is the one moment nothing
  // is mid-draw. See refresh.h for why this needs no lock.
  if (refresh::adopt(g_data)) {
    power::noteFetch();
    Serial.printf("[main] new data adopted (%u rows, live=%d, heap %lu)\n",
                  g_data.tableRows, g_data.liveActive,
                  (unsigned long)ESP.getFreeHeap());
    g_screens.refresh();
  }

  // --- Web UI requests ----------------------------------------------------
  // Applied here, not in the web layer, so a page handler cannot reboot or
  // wipe the device on its own — the same separation the settings menu uses.
  switch (web::takeAction()) {
    case web::Action::RefreshNow:
      Serial.println(F("[main] manual refresh requested"));
      refresh::invalidateAll();
      break;
    case web::Action::ResetWifi:
      Serial.println(F("[main] Wi-Fi reset requested from web UI"));
      g_settings.wifiSsid[0] = '\0';
      g_settings.wifiPass[0] = '\0';
      store::saveSettings(g_settings);
      ui::drawStatusScreen(tft, "Wi-Fi reset", "restarting into setup...",
                           ui::colour::kDraw);
      delay(1200);  // Let the confirmation page reach the browser.
      ESP.restart();
      break;
    case web::Action::FactoryReset:
      Serial.println(F("[main] factory reset requested from web UI"));
      store::factoryReset();
      ui::drawStatusScreen(tft, "Factory reset", "restarting...",
                           ui::colour::kLoss);
      delay(1200);
      ESP.restart();
      break;
    case web::Action::None:
      break;
  }

  // Settings changed in the browser take effect without a restart wherever
  // that is possible, which is everything except the Wi-Fi credentials.
  if (web::settingsDirty()) {
    web::clearSettingsDirty();
    Serial.println(F("[main] applying changed settings"));
    backlightSet(g_settings.brightness);
    g_screens.setDwell(g_settings.screenDwellMs);
    g_screens.setEnabledMask(g_settings.screenMask);
    api::begin(g_settings.apiSportsKey, g_settings.footballDataKey);
    // A changed team or competition makes the cache wrong rather than merely
    // stale, so it is discarded rather than left to expire.
    store::clearAllDocs();
    refresh::invalidateAll();
    g_screens.refresh();
  }

  g_screens.handleGesture(gesture);
  g_screens.tick();

  // --- Power ---------------------------------------------------------------
  updateDimming();
  power::markBusy(millis() - loopStart);
  power::tick();

  static uint32_t lastPowerLog = 0;
  if (millis() - lastPowerLog > 60000) {
    lastPowerLog = millis();
    power::logSummary();
  }

  // Idle interval, chosen by what the screen is actually doing.
  //
  // A dimmed screen showing static content has nothing to redraw and nobody
  // watching, so polling it 125 times a second is waste. Touch is still
  // sampled often enough to feel immediate — 60 ms is well under the point at
  // which a tap feels delayed — and light sleep wakes early on the touch IRQ
  // anyway, so a tap is answered at once rather than at the end of the
  // interval.
  // Kept short in both states, because the web server is polled from here and
  // a longer interval starves it. The saving comes from doing less work per
  // iteration and from the lower clock, not from iterating less often.
  // A dimmed screen with nobody watching does not need polling 125 times a
  // second. 60 ms took CPU busy time from 11% to 1% and, measured across
  // eight requests, left the web interface entirely reliable — an earlier
  // failure there turned out to be the frequency scaling, not this.
  const uint32_t idleMs = g_dimmed ? 60 : 8;
  power::markIdle(idleMs);

  // Light sleep would be the better idle still, since it retains pin state and
  // so keeps the backlight lit and the panel's image visible. It is attempted
  // only when enabled, which by default it is not — see power.cpp for the RTC
  // watchdog reset that made it unusable on this build.
  if (!g_dimmed || !power::lightSleep(idleMs)) {
    delay(idleMs);
  }
}
