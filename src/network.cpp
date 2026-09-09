/**
 * @file network.cpp
 * @brief Wi-Fi connection and provisioning. See network.h.
 */

#include "network.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

namespace net {
namespace {

Status    g_status;
DNSServer g_dns;
bool      g_dnsRunning = false;

char g_apSsid[33]     = {0};
char g_portalUrl[32]  = {0};

/**
 * The setup AP's password.
 *
 * An open network would be simpler, but a device advertising an unsecured AP
 * with a configuration portal on it is a device anyone in range can
 * reconfigure. A fixed, printed-on-screen password is the right trade for
 * hardware with no display of its own to show a random one — and this one has
 * a display, so it is shown there.
 */
constexpr const char* kApPassword = "football";

/// Captive portals are conventionally found at a fixed address.
const IPAddress kApIp(4, 3, 2, 1);
const IPAddress kApMask(255, 255, 255, 0);

/// Give up joining after this long and fall back to the AP. Chosen so a
/// genuinely slow router still succeeds, while a wrong password does not leave
/// the device staring at a blank screen for a minute.
constexpr uint32_t kJoinTimeoutMs = 15000;

/// Reconnect backoff. The radio is the second-largest power draw, so retrying
/// constantly would be expensive as well as futile.
constexpr uint32_t kReconnectIntervalMs = 30000;

/// mDNS hostname, so the device is reachable without hunting for its IP.
constexpr const char* kHostname = "football";

/// Convert dBm to a rough percentage. Not linear in reality, but a scale
/// people can read at a glance beats an accurate number they cannot.
uint8_t signalQuality(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return static_cast<uint8_t>(2 * (rssi + 100));
}

void captureStationStatus() {
  strncpy(g_status.ssid, WiFi.SSID().c_str(), sizeof(g_status.ssid) - 1);
  strncpy(g_status.ip, WiFi.localIP().toString().c_str(),
          sizeof(g_status.ip) - 1);
  g_status.rssi        = static_cast<int8_t>(WiFi.RSSI());
  g_status.quality     = signalQuality(WiFi.RSSI());
  g_status.connectedAt = millis();
}

/// Derive an AP name that identifies this specific device.
///
/// Uses the last two MAC bytes so two devices in the same room are
/// distinguishable — which matters as soon as anyone owns a second one.
void buildApName() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(g_apSsid, sizeof(g_apSsid), "FootballTracker-%02X%02X", mac[4],
           mac[5]);
}

bool startAccessPoint() {
  buildApName();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, kApMask);
  if (!WiFi.softAP(g_apSsid, kApPassword)) {
    Serial.println(F("[net] failed to start access point"));
    g_status.mode = Mode::Failed;
    return false;
  }

  snprintf(g_portalUrl, sizeof(g_portalUrl), "http://%s",
           kApIp.toString().c_str());

  // Answer every DNS query with our own address, so any URL a phone tries
  // lands on the portal. This is what makes the "sign in to network" prompt
  // appear rather than requiring the address to be typed.
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dnsRunning = g_dns.start(53, "*", kApIp);

  strncpy(g_status.ssid, g_apSsid, sizeof(g_status.ssid) - 1);
  strncpy(g_status.ip, kApIp.toString().c_str(), sizeof(g_status.ip) - 1);
  g_status.mode = Mode::AccessPoint;

  Serial.println();
  Serial.println(F("=== Setup access point ==="));
  Serial.printf("  SSID    : %s\n", g_apSsid);
  Serial.printf("  Password: %s\n", kApPassword);
  Serial.printf("  Portal  : %s\n", g_portalUrl);
  Serial.printf("  Captive DNS: %s\n", g_dnsRunning ? "running" : "FAILED");
  return true;
}

bool joinNetwork(const store::Settings& settings) {
  Serial.printf("[net] joining \"%s\"\n", settings.wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  // Persisting credentials in the radio's own NVS as well as ours would give
  // two sources of truth; ours is authoritative.
  WiFi.persistent(false);
  // Modem sleep between beacons. A meaningful power saving for a device that
  // is idle most of the time, and it costs only a little latency.
  WiFi.setSleep(true);
  WiFi.begin(settings.wifiSsid, settings.wifiPass);

  g_status.mode = Mode::Connecting;
  const uint32_t deadline = millis() + kJoinTimeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[net] join failed after %lus (status %d)\n",
                  (unsigned long)(kJoinTimeoutMs / 1000), WiFi.status());
    return false;
  }

  captureStationStatus();
  g_status.mode = Mode::Connected;
  Serial.printf("[net] connected: %s  %s  %d dBm (%u%%)\n", g_status.ssid,
                g_status.ip, g_status.rssi, g_status.quality);

  if (MDNS.begin(kHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[net] mDNS: http://%s.local\n", kHostname);
  }
  return true;
}

}  // namespace

void begin(const store::Settings& settings, bool forceSetup) {
  g_status = Status{};

  if (forceSetup || !settings.hasWifi()) {
    Serial.println(forceSetup ? F("[net] setup forced")
                              : F("[net] no stored credentials"));
    startAccessPoint();
    return;
  }

  if (!joinNetwork(settings)) {
    // Falling back to the AP rather than retrying forever: if the credentials
    // are wrong, only the user can fix it, and they need the portal to do so.
    Serial.println(F("[net] falling back to setup access point"));
    startAccessPoint();
    return;
  }

  beginTimeSync();
}

void tick() {
  if (g_status.mode == Mode::AccessPoint) {
    if (g_dnsRunning) g_dns.processNextRequest();
    return;
  }

  if (g_status.mode != Mode::Connected) return;

  // Refresh signal strength and watch for the link dropping.
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[net] link lost -- reconnecting"));
    g_status.mode = Mode::Connecting;
    WiFi.reconnect();
    return;
  }

  g_status.rssi    = static_cast<int8_t>(WiFi.RSSI());
  g_status.quality = signalQuality(WiFi.RSSI());

  // Note whether the clock has actually been set. Everything time-dependent
  // checks this rather than assuming NTP succeeded.
  if (!g_status.timeSynced && time(nullptr) > 1600000000L) {
    g_status.timeSynced = true;
    time_t now = time(nullptr);
    struct tm tmBuf;
    localtime_r(&now, &tmBuf);
    Serial.printf("[net] time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
  }
}

void beginTimeSync() {
  // UK time with automatic BST. Written as a POSIX TZ string so the transition
  // rules live in the C library rather than in our code — and so a kick-off
  // time is never an hour out in summer, which is the specific bug that made
  // formatKickoff refuse to guess before this point.
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2", "pool.ntp.org", "time.nist.gov");
  Serial.println(F("[net] NTP sync requested (Europe/London)"));
}

const Status& status() { return g_status; }

bool online() { return g_status.mode == Mode::Connected; }

const char* apSsid() { return g_apSsid; }
const char* apPassword() { return kApPassword; }
const char* portalUrl() { return g_portalUrl; }

}  // namespace net
