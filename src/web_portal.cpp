/**
 * @file web_portal.cpp
 * @brief On-device web interface. See web_portal.h.
 */

#include "web_portal.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "assets_pure_css.h"
#include "network.h"
#include "ota.h"
#include "refresh.h"

namespace web {
namespace {

WebServer               g_server(80);
store::Settings*        g_settings = nullptr;
const model::Snapshot*  g_data     = nullptr;
bool g_captive            = false;
bool g_settingsDirty      = false;
bool g_credentialsSubmitted = false;
/// Requested by the System page; performed by the caller, so the web layer
/// never reboots the device on its own.
Action g_pendingAction = Action::None;

/// One label/value row in a status table. Declared ahead of use so the pages
/// defined above the implementation can call it.
void row(const char* label, const char* value, const char* cls = nullptr);

/**
 * Send a chunk of HTML.
 *
 * Pages are streamed in pieces rather than assembled into one String. A String
 * built up by concatenation would repeatedly reallocate and fragment a heap
 * whose largest contiguous block is only ~112 KB — and fragmentation is the
 * failure mode that eventually refuses a TLS handshake, which is far harder to
 * diagnose than a slightly more verbose page builder.
 */
void sendChunk(const char* html) {
  // An empty string must never reach sendContent(): in a chunked response a
  // zero-length chunk is the *terminator*, which is exactly how endPage()
  // ends the body. Passing "" mid-page therefore truncates the document and
  // every subsequent write lands on a closed socket ("Connection reset by
  // peer"). An unset API key rendering as an empty value field was enough to
  // trigger it.
  if (html == nullptr || html[0] == '\0') return;
  g_server.sendContent(html);
}

/// Escape text for safe interpolation into HTML.
///
/// Network names are chosen by other people and can contain quotes and angle
/// brackets. Interpolating them raw would break the page at best, and at worst
/// let a hostile SSID inject markup into our own configuration form.
void sendEscaped(const char* text) {
  char out[192];
  size_t o = 0;
  for (const char* p = text; *p != '\0' && o < sizeof(out) - 7; ++p) {
    switch (*p) {
      case '&':  memcpy(out + o, "&amp;", 5);  o += 5; break;
      case '<':  memcpy(out + o, "&lt;", 4);   o += 4; break;
      case '>':  memcpy(out + o, "&gt;", 4);   o += 4; break;
      case '"':  memcpy(out + o, "&quot;", 6); o += 6; break;
      case '\'': memcpy(out + o, "&#39;", 5);  o += 5; break;
      default:   out[o++] = *p; break;
    }
  }
  out[o] = '\0';
  // Same hazard as sendChunk(): an empty escaped result would terminate the
  // response rather than writing nothing.
  if (o == 0) return;
  g_server.sendContent(out);
}

void beginPage(const char* title) {
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.send(200, "text/html", "");
  sendChunk(
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<link rel=\"stylesheet\" href=\"/pure-min.css\">"
      "<style>"
      "body{max-width:38rem;margin:0 auto;padding:1rem;"
      "font-family:system-ui,-apple-system,sans-serif;background:#111;color:#eee}"
      "h1{font-size:1.3rem;margin:.2rem 0 1rem}"
      "h2{font-size:1rem;color:#8cf;margin-top:1.6rem}"
      ".card{background:#1c1c1c;border:1px solid #333;border-radius:6px;"
      "padding:.8rem 1rem;margin-bottom:1rem}"
      ".pure-form input,.pure-form select{width:100%;box-sizing:border-box}"
      "label{display:block;margin-top:.6rem;font-size:.85rem;color:#aaa}"
      "table{width:100%}td,th{padding:.25rem .4rem;font-size:.9rem}"
      "th{text-align:left;color:#888;font-weight:400}"
      ".ok{color:#5d5}.warn{color:#fd0}.bad{color:#f66}"
      "</style><title>");
  sendChunk(title);
  sendChunk("</title></head><body><h1>");
  sendChunk(title);
  sendChunk("</h1>");
}

/// Navigation, so every page has a route to every other one.
void nav(const char* current) {
  struct Item { const char* path; const char* label; };
  static const Item kItems[] = {
      {"/", "Status"},
      {"/settings", "Settings"},
      {"/cache", "Cache"},
      {"/system", "System"},
  };
  sendChunk("<div style=\"margin:0 0 1rem\">");
  for (const Item& it : kItems) {
    const bool here = strcmp(it.path, current) == 0;
    sendChunk("<a href=\"");
    sendChunk(it.path);
    sendChunk("\" style=\"margin-right:.9rem;");
    sendChunk(here ? "color:#fff;font-weight:600" : "color:#8cf");
    sendChunk("\">");
    sendChunk(it.label);
    sendChunk("</a>");
  }
  sendChunk("</div>");
}

void endPage() {
  sendChunk("</body></html>");
  // Deliberately g_server.sendContent rather than sendChunk: the zero-length
  // chunk is the terminator, and sendChunk now filters exactly that out.
  g_server.sendContent("");
}

/// Serve the embedded stylesheet.
///
/// Sent gzipped exactly as stored, with Content-Encoding set so the browser
/// inflates it. The device performs no decompression and needs no buffer — the
/// bytes go straight from flash to the socket.
void handleCss() {
  g_server.sendHeader("Content-Encoding", "gzip");
  // Immutable: the asset is versioned by firmware, so a browser never needs to
  // revalidate it.
  g_server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  g_server.send_P(200, "text/css",
                  reinterpret_cast<const char*>(assets::kPureCssGz),
                  assets::kPureCssGzSize);
}

// ---------------------------------------------------------------------------
// Setup portal (AP mode)
// ---------------------------------------------------------------------------

void handleSetup() {
  beginPage("Football Tracker setup");
  sendChunk(
      "<div class=\"card\"><p>Choose your Wi-Fi network and enter its "
      "password. The tracker will restart and connect.</p></div>"
      "<form class=\"pure-form pure-form-stacked\" method=\"POST\" "
      "action=\"/wifi\"><div class=\"card\">"
      "<h2>Wi-Fi</h2><label for=\"ssid\">Network</label>"
      "<select id=\"ssid\" name=\"ssid\">");

  // Scanning here rather than on a timer: the list is only needed when this
  // page is shown, and a scan costs a second of radio time.
  const int found = WiFi.scanNetworks();
  if (found <= 0) {
    sendChunk("<option value=\"\">no networks found</option>");
  } else {
    for (int i = 0; i < found && i < 20; ++i) {
      sendChunk("<option value=\"");
      sendEscaped(WiFi.SSID(i).c_str());
      sendChunk("\">");
      sendEscaped(WiFi.SSID(i).c_str());
      char suffix[32];
      snprintf(suffix, sizeof(suffix), " (%d dBm)%s", WiFi.RSSI(i),
               WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " open" : "");
      sendChunk(suffix);
      sendChunk("</option>");
    }
  }
  WiFi.scanDelete();

  sendChunk(
      "</select>"
      "<label for=\"pass\">Password</label>"
      "<input id=\"pass\" name=\"pass\" type=\"password\" "
      "autocomplete=\"off\">"
      // Reveal option, because a mistyped Wi-Fi password is only discovered
      // after a reboot and a failed join — an expensive way to find a typo.
      // Now commonplace, and the risk is low on a page only reachable from a
      // network the user is already on.
      "<label class=\"pure-checkbox\" style=\"margin-top:.5rem\">"
      "<input type=\"checkbox\" onclick=\"var p=document.getElementById("
      "'pass');p.type=this.checked?'text':'password'\"> Show password</label>"
      "</div><div class=\"card\"><h2>Your team</h2>"
      "<p style=\"font-size:.85rem;color:#aaa\">Defaults to Bolton Wanderers "
      "in the Championship. Changeable later from the main interface.</p>"
      "</div>"
      "<button type=\"submit\" class=\"pure-button pure-button-primary\">"
      "Save and restart</button></form>");
  endPage();
}

void handleWifiPost() {
  if (g_settings == nullptr) {
    g_server.send(500, "text/plain", "not ready");
    return;
  }

  const String ssid = g_server.arg("ssid");
  const String pass = g_server.arg("pass");

  if (ssid.length() == 0) {
    // Rejected rather than saved: storing an empty SSID would send the device
    // into a join attempt that cannot succeed, then back to this page anyway,
    // with a reboot wasted in between.
    beginPage("No network chosen");
    sendChunk("<div class=\"card\"><p class=\"bad\">Please choose a network."
              "</p><p><a href=\"/\">Back</a></p></div>");
    endPage();
    return;
  }

  strncpy(g_settings->wifiSsid, ssid.c_str(), sizeof(g_settings->wifiSsid) - 1);
  strncpy(g_settings->wifiPass, pass.c_str(), sizeof(g_settings->wifiPass) - 1);
  store::saveSettings(*g_settings);

  beginPage("Saved");
  sendChunk("<div class=\"card\"><p class=\"ok\">Credentials saved for <b>");
  sendEscaped(g_settings->wifiSsid);
  sendChunk("</b>.</p><p>The tracker is restarting. This access point will "
            "disappear — rejoin your normal network.</p></div>");
  endPage();

  g_credentialsSubmitted = true;
  Serial.printf("[web] credentials saved for \"%s\"\n", g_settings->wifiSsid);
}

// ---------------------------------------------------------------------------
// Dashboard (station mode)
// ---------------------------------------------------------------------------

void rowImpl(const char* label, const char* value, const char* cls) {
  sendChunk("<tr><th>");
  sendChunk(label);
  sendChunk("</th><td");
  if (cls != nullptr) {
    sendChunk(" class=\"");
    sendChunk(cls);
    sendChunk("\"");
  }
  sendChunk(">");
  sendEscaped(value);
  sendChunk("</td></tr>");
}

void row(const char* label, const char* value, const char* cls) {
  rowImpl(label, value, cls);
}

void handleDashboard() {
  beginPage("Football Tracker");
  nav("/");

  const net::Status& st = net::status();
  char buf[64];

  sendChunk("<div class=\"card\"><h2>Connection</h2><table>");
  row("Network", st.ssid);
  row("Address", st.ip);
  snprintf(buf, sizeof(buf), "%d dBm (%u%%)", st.rssi, st.quality);
  row("Signal", buf, st.quality >= 40 ? "ok" : "warn");
  row("Clock", st.timeSynced ? "synced" : "not synced",
      st.timeSynced ? "ok" : "warn");
  sendChunk("</table></div>");

  // API quota: the number the user most needs to see, per rule R7.
  store::Quota q;
  store::loadQuota(q);
  sendChunk("<div class=\"card\"><h2>API quota today</h2><table>");
  snprintf(buf, sizeof(buf), "%u of %u used", q.used, q.limit);
  const uint16_t remaining = q.limit > q.used ? q.limit - q.used : 0;
  row("api-sports", buf, remaining > 10 ? "ok" : "bad");
  snprintf(buf, sizeof(buf), "%u remaining", remaining);
  row("Remaining", buf);
  row("football-data.org", "no daily cap (10/min)", "ok");
  sendChunk("</table></div>");

  // Cache state, so staleness is visible rather than mysterious.
  sendChunk("<div class=\"card\"><h2>Cache</h2><table>");
  for (uint8_t i = 0; i < static_cast<uint8_t>(store::Doc::Count); ++i) {
    const store::Doc doc = static_cast<store::Doc>(i);
    const store::DocStatus ds = store::statusOf(doc);
    if (!ds.present) {
      row(store::docName(doc), "absent", "warn");
      continue;
    }
    snprintf(buf, sizeof(buf), "%lu bytes, %s", (unsigned long)ds.size,
             ds.fresh ? "fresh" : "stale");
    row(store::docName(doc), buf, ds.fresh ? "ok" : "warn");
  }
  uint32_t used = 0, total = 0;
  store::filesystemUsage(used, total);
  snprintf(buf, sizeof(buf), "%lu KB of %lu KB", (unsigned long)(used / 1024),
           (unsigned long)(total / 1024));
  row("Filesystem", buf);
  sendChunk("</table></div>");

  sendChunk("<div class=\"card\"><h2>Device</h2><table>");
  snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
  row("Free heap", buf);
  snprintf(buf, sizeof(buf), "%lu min", (unsigned long)(millis() / 60000));
  row("Uptime", buf);
  row("Firmware", ota::currentVersion());
  sendChunk("</table></div>");

  endPage();
}

/// Render a text input, with the current value pre-filled.
void textField(const char* id, const char* label, const char* value,
               const char* hint = nullptr, bool password = false) {
  sendChunk("<label for=\"");
  sendChunk(id);
  sendChunk("\">");
  sendChunk(label);
  if (hint != nullptr) {
    sendChunk("<span style=\"color:#777\"> — ");
    sendChunk(hint);
    sendChunk("</span>");
  }
  sendChunk("</label><input id=\"");
  sendChunk(id);
  sendChunk("\" name=\"");
  sendChunk(id);
  sendChunk(password ? "\" type=\"password\" autocomplete=\"off\" value=\""
                     : "\" type=\"text\" value=\"");
  sendEscaped(value);
  sendChunk("\">");
}

void handleSettings() {
  beginPage("Settings");
  nav("/settings");

  if (g_settings == nullptr) {
    sendChunk("<p class=\"bad\">not ready</p>");
    endPage();
    return;
  }
  const store::Settings& c = *g_settings;
  char buf[24];

  sendChunk("<form class=\"pure-form pure-form-stacked\" method=\"POST\" "
            "action=\"/settings\">");

  // --- API keys --------------------------------------------------------
  sendChunk("<div class=\"card\"><h2>API keys</h2>"
            "<p style=\"font-size:.85rem;color:#aaa\">Stored on the device. "
            "Leave a field blank to keep the current key.</p>");
  // Existing keys are never sent back to the browser. There is no legitimate
  // reason for the page to carry a secret it already holds, and a blank field
  // meaning "unchanged" gives the same editing experience without it.
  textField("fdkey", "football-data.org",
            "", "table, fixtures, scorers", true);
  textField("apikey", "api-sports.io", "", "live match data", true);
  sendChunk(
      "<label class=\"pure-checkbox\" style=\"margin-top:.5rem\">"
      "<input type=\"checkbox\" onclick=\"var t=this.checked?'text':"
      "'password';document.getElementById('fdkey').type=t;"
      "document.getElementById('apikey').type=t\"> Show keys</label>");
  sendChunk("<p style=\"font-size:.8rem;color:#777\">Currently: ");
  sendChunk(c.hasFootballDataKey() ? "football-data <span class=\"ok\">set"
                                     "</span>"
                                   : "football-data <span class=\"bad\">"
                                     "missing</span>");
  sendChunk(c.apiSportsKey[0] != '\0'
                ? ", api-sports <span class=\"ok\">set</span>"
                : ", api-sports <span class=\"warn\">missing</span>");
  sendChunk("</p></div>");

  // --- Team ------------------------------------------------------------
  sendChunk("<div class=\"card\"><h2>Team</h2>"
            "<p style=\"font-size:.85rem;color:#aaa\">The two providers use "
            "different id spaces — football-data 68 is Norwich, not Bolton — "
            "so both are set separately and deliberately.</p>");
  textField("teamname", "Display name", c.teamDisplayName,
            "shown on screen");
  snprintf(buf, sizeof(buf), "%u", c.footballDataTeamId);
  textField("fdteam", "football-data team id", buf, "Bolton = 60");
  snprintf(buf, sizeof(buf), "%u", c.apiSportsTeamId);
  textField("apiteam", "api-sports team id", buf, "Bolton = 68");
  textField("comp", "Competition code", c.competitionCode,
            "Championship = ELC");
  sendChunk("</div>");

  // --- Screens ---------------------------------------------------------
  sendChunk("<div class=\"card\"><h2>Screens</h2>");
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)(c.screenDwellMs / 1000));
  textField("dwell", "Seconds per screen", buf, "1-120");

  static const char* kScreenNames[] = {"Live match", "Season record",
                                       "Last result", "Next fixture",
                                       "League table", "Top scorers"};
  sendChunk("<p style=\"margin-top:.8rem;color:#aaa;font-size:.85rem\">"
            "Include in the rotation:</p>");
  for (uint8_t i = 0; i < 6; ++i) {
    char id[12];
    snprintf(id, sizeof(id), "scr%u", i);
    sendChunk("<label class=\"pure-checkbox\"><input type=\"checkbox\" "
              "name=\"");
    sendChunk(id);
    sendChunk("\" value=\"1\"");
    if (c.screenMask & (1u << i)) sendChunk(" checked");
    sendChunk("> ");
    sendChunk(kScreenNames[i]);
    sendChunk("</label>");
  }
  sendChunk("</div>");

  // --- Display ---------------------------------------------------------
  sendChunk("<div class=\"card\"><h2>Display &amp; sound</h2>");
  snprintf(buf, sizeof(buf), "%u", c.brightness);
  textField("bright", "Brightness %", buf, "10-100");
  sendChunk("<label class=\"pure-checkbox\"><input type=\"checkbox\" "
            "name=\"sound\" value=\"1\"");
  if (c.soundEnabled) sendChunk(" checked");
  sendChunk("> Goal chime <span style=\"color:#777\">(off by default; a "
            "device that beeps unbidden gets unplugged)</span></label>");
  sendChunk("<label class=\"pure-checkbox\"><input type=\"checkbox\" "
            "name=\"cups\" value=\"1\"");
  if (c.includeCups) sendChunk(" checked");
  sendChunk("> Include cup competitions <span style=\"color:#777\">(costs "
            "extra API calls)</span></label>");
  sendChunk("</div>");

  // --- Updates ---------------------------------------------------------
  sendChunk("<div class=\"card\"><h2>Firmware updates</h2>"
            "<p style=\"font-size:.85rem;color:#aaa\">A JSON manifest "
            "describing the latest release. Point this at your own fork's "
            "releases if you have one.</p>");
  textField("otaurl", "Manifest URL", c.otaManifestUrl,
            "https://raw.githubusercontent.com/.../manifest.json");
  sendChunk("<label class=\"pure-checkbox\"><input type=\"checkbox\" "
            "name=\"otaauto\" value=\"1\"");
  if (c.otaAutoCheck) sendChunk(" checked");
  sendChunk("> Check daily <span style=\"color:#777\">(checks only; "
            "installing always needs a button press)</span></label>");
  sendChunk("<p style=\"font-size:.8rem;color:#777\">Running ");
  sendEscaped(ota::currentBuild());
  sendChunk("</p></div>");

  sendChunk("<button type=\"submit\" class=\"pure-button "
            "pure-button-primary\">Save settings</button></form>");
  endPage();
}

/// Clamp a submitted number, so a typo cannot make the device unusable.
uint32_t clampedArg(const char* name, uint32_t lo, uint32_t hi,
                    uint32_t fallback) {
  const String raw = g_server.arg(name);
  if (raw.length() == 0) return fallback;
  const long v = raw.toInt();
  if (v < static_cast<long>(lo)) return lo;
  if (v > static_cast<long>(hi)) return hi;
  return static_cast<uint32_t>(v);
}

void handleSettingsPost() {
  if (g_settings == nullptr) {
    g_server.send(500, "text/plain", "not ready");
    return;
  }
  store::Settings& c = *g_settings;

  // Keys: a blank field means "unchanged", which is what lets the form omit
  // the existing secret rather than echoing it back to the browser.
  const String fdKey = g_server.arg("fdkey");
  if (fdKey.length() > 0) {
    strncpy(c.footballDataKey, fdKey.c_str(), sizeof(c.footballDataKey) - 1);
    c.footballDataKey[sizeof(c.footballDataKey) - 1] = '\0';
  }
  const String apiKey = g_server.arg("apikey");
  if (apiKey.length() > 0) {
    strncpy(c.apiSportsKey, apiKey.c_str(), sizeof(c.apiSportsKey) - 1);
    c.apiSportsKey[sizeof(c.apiSportsKey) - 1] = '\0';
  }

  const String teamName = g_server.arg("teamname");
  if (teamName.length() > 0) {
    strncpy(c.teamDisplayName, teamName.c_str(),
            sizeof(c.teamDisplayName) - 1);
    c.teamDisplayName[sizeof(c.teamDisplayName) - 1] = '\0';
  }
  const String otaUrl = g_server.arg("otaurl");
  if (otaUrl.length() > 0) {
    strncpy(c.otaManifestUrl, otaUrl.c_str(), sizeof(c.otaManifestUrl) - 1);
    c.otaManifestUrl[sizeof(c.otaManifestUrl) - 1] = '\0';
  }
  c.otaAutoCheck = g_server.hasArg("otaauto");

  const String comp = g_server.arg("comp");
  if (comp.length() > 0) {
    strncpy(c.competitionCode, comp.c_str(), sizeof(c.competitionCode) - 1);
    c.competitionCode[sizeof(c.competitionCode) - 1] = '\0';
  }

  c.footballDataTeamId =
      static_cast<uint16_t>(clampedArg("fdteam", 1, 65535,
                                       c.footballDataTeamId));
  c.apiSportsTeamId =
      static_cast<uint16_t>(clampedArg("apiteam", 1, 65535,
                                       c.apiSportsTeamId));
  c.screenDwellMs = clampedArg("dwell", 1, 120, c.screenDwellMs / 1000) * 1000;
  c.brightness =
      static_cast<uint8_t>(clampedArg("bright", 10, 100, c.brightness));

  // Unchecked checkboxes are simply absent from a form POST, so each is read
  // as present-or-not rather than by value.
  c.soundEnabled = g_server.hasArg("sound");
  c.includeCups  = g_server.hasArg("cups");

  uint8_t mask = 0;
  for (uint8_t i = 0; i < 6; ++i) {
    char id[12];
    snprintf(id, sizeof(id), "scr%u", i);
    if (g_server.hasArg(id)) mask |= (1u << i);
  }
  // Every screen unticked would leave a blank device. Treated as "all of
  // them", which is far more likely to be what was meant than nothing.
  c.screenMask = (mask == 0) ? 0xFF : mask;

  store::saveSettings(c);
  g_settingsDirty = true;

  g_server.sendHeader("Location", "/settings", true);
  g_server.send(303, "text/plain", "");
  Serial.println(F("[web] settings saved"));
}

// ---------------------------------------------------------------------------
// Cache page
// ---------------------------------------------------------------------------

void handleCache() {
  beginPage("Cache");
  nav("/cache");
  char buf[80];

  sendChunk("<div class=\"card\"><h2>Cached documents</h2><table>");
  for (uint8_t i = 0; i < static_cast<uint8_t>(store::Doc::Count); ++i) {
    const store::Doc doc = static_cast<store::Doc>(i);
    const store::DocStatus ds = store::statusOf(doc);
    if (!ds.present) {
      row(store::docName(doc), "absent", "warn");
      continue;
    }
    snprintf(buf, sizeof(buf), "%lu bytes, %s", (unsigned long)ds.size,
             ds.fresh ? "fresh" : "stale");
    row(store::docName(doc), buf, ds.fresh ? "ok" : "warn");
  }
  uint32_t used = 0, total = 0;
  store::filesystemUsage(used, total);
  snprintf(buf, sizeof(buf), "%lu KB of %lu KB", (unsigned long)(used / 1024),
           (unsigned long)(total / 1024));
  row("Filesystem", buf);
  sendChunk("</table></div>");

  sendChunk(
      "<div class=\"card\"><h2>Actions</h2>"
      "<p style=\"font-size:.85rem;color:#aaa\">Refreshing spends API "
      "requests. The daily allowance keeps a reserve of 10 for exactly "
      "this.</p>"
      "<form method=\"POST\" action=\"/cache/refresh\" "
      "style=\"display:inline\">"
      "<button class=\"pure-button pure-button-primary\">Refresh now"
      "</button></form> "
      "<form method=\"POST\" action=\"/cache/clear\" "
      "style=\"display:inline\" onsubmit=\"return confirm("
      "'Clear all cached data?')\">"
      "<button class=\"pure-button\">Clear cache</button></form></div>");
  endPage();
}

// ---------------------------------------------------------------------------
// System page
// ---------------------------------------------------------------------------

void handleSystem() {
  beginPage("System");
  nav("/system");

  const net::Status& st = net::status();
  char buf[64];
  sendChunk("<div class=\"card\"><h2>Device</h2><table>");
  row("Network", st.ssid);
  row("Address", st.ip);
  snprintf(buf, sizeof(buf), "%d dBm (%u%%)", st.rssi, st.quality);
  row("Signal", buf, st.quality >= 40 ? "ok" : "warn");
  snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
  row("Free heap", buf);
  snprintf(buf, sizeof(buf), "%lu min", (unsigned long)(millis() / 60000));
  row("Uptime", buf);
  snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(ESP.getSketchSize() / 1024));
  row("Firmware size", buf);
  sendChunk("</table></div>");

  // --- Firmware ---------------------------------------------------------
  sendChunk("<div class=\"card\"><h2>Firmware</h2><table>");
  row("Version", ota::currentVersion());
  row("Build", ota::currentBuild());
  const ota::UpdateInfo& up = refresh::updateInfo();
  if (up.version[0] != '\0') {
    row("Latest seen", up.version, up.available ? "warn" : "ok");
  }
  sendChunk("</table>");

  if (refresh::updateInProgress()) {
    sendChunk("<p class=\"warn\">An update is in progress. The device will "
              "restart when it finishes.</p>");
  } else if (up.available) {
    sendChunk("<p class=\"warn\">Version ");
    sendEscaped(up.version);
    sendChunk(" is available.</p>"
              "<form method=\"POST\" action=\"/system/update\" "
              "onsubmit=\"return confirm('Download and install this update? "
              "The device will restart.')\">"
              "<button class=\"pure-button pure-button-primary\">"
              "Install update</button></form>");
  } else {
    sendChunk("<form method=\"POST\" action=\"/system/check-update\">"
              "<button class=\"pure-button\">Check for updates</button>"
              "</form>");
  }

  // Upload is offered alongside the pull path because it is the one that
  // works with no internet — on an isolated network, or if the release host
  // is unreachable.
  sendChunk(
      "<p style=\"font-size:.85rem;color:#aaa;margin-top:1rem\">Or upload a "
      "<code>firmware.bin</code> built locally. Unlike an update pulled from "
      "a release, an upload has no manifest to check its hash against — the "
      "image header is validated, but nothing confirms it is the firmware you "
      "intended.</p>"
      "<form method=\"POST\" action=\"/system/ota\" "
      "enctype=\"multipart/form-data\">"
      "<input type=\"file\" name=\"firmware\" accept=\".bin\" required>"
      "<button class=\"pure-button\" style=\"margin-top:.5rem\">"
      "Upload and install</button></form></div>");

  // Both destructive actions confirm in the browser as well as being
  // separate POSTs, so neither can be triggered by following a link — a
  // crawler or a prefetching browser must not be able to wipe the device.
  sendChunk(
      "<div class=\"card\"><h2>Reset Wi-Fi</h2>"
      "<p style=\"font-size:.85rem;color:#aaa\">Clears the stored network "
      "and restarts into setup mode. Settings and cache are kept.</p>"
      "<form method=\"POST\" action=\"/system/wifi-reset\" "
      "onsubmit=\"return confirm('Reset Wi-Fi and restart into setup?')\">"
      "<button class=\"pure-button\">Reset Wi-Fi</button></form></div>");
  sendChunk(
      "<div class=\"card\"><h2 class=\"bad\">Factory reset</h2>"
      "<p style=\"font-size:.85rem;color:#aaa\">Erases Wi-Fi, API keys, all "
      "settings and the cache. This cannot be undone.</p>"
      "<form method=\"POST\" action=\"/system/factory-reset\" "
      "onsubmit=\"return confirm('Erase everything and restart?')\">"
      "<button class=\"pure-button\" style=\"background:#802\">"
      "Factory reset</button></form></div>");
  endPage();
}

/**
 * Catch-all, which is what makes the captive portal appear.
 *
 * Phones and laptops probe a known URL after joining a network and show a
 * "sign in" notification if the reply is not what they expected. Redirecting
 * everything to the setup page is what triggers that prompt — without it, the
 * user would have to know to type an IP address.
 */
void handleNotFound() {
  if (g_captive) {
    g_server.sendHeader("Location", "/", true);
    g_server.send(302, "text/plain", "");
    return;
  }
  g_server.send(404, "text/plain", "not found");
}

}  // namespace

void begin(store::Settings& settings, const model::Snapshot& data,
           bool captive) {
  g_settings = &settings;
  g_data     = &data;
  g_captive  = captive;

  g_server.on("/pure-min.css", HTTP_GET, handleCss);
  if (captive) {
    g_server.on("/", HTTP_GET, handleSetup);
    g_server.on("/wifi", HTTP_POST, handleWifiPost);
  } else {
    g_server.on("/", HTTP_GET, handleDashboard);
    g_server.on("/settings", HTTP_GET, handleSettings);
    g_server.on("/settings", HTTP_POST, handleSettingsPost);
    g_server.on("/cache", HTTP_GET, handleCache);
    g_server.on("/cache/clear", HTTP_POST, []() {
      store::clearAllDocs();
      g_pendingAction = Action::RefreshNow;  // Refill what was just cleared.
      g_server.sendHeader("Location", "/cache", true);
      g_server.send(303, "text/plain", "");
    });
    g_server.on("/cache/refresh", HTTP_POST, []() {
      g_pendingAction = Action::RefreshNow;
      g_server.sendHeader("Location", "/cache", true);
      g_server.send(303, "text/plain", "");
    });
    g_server.on("/system", HTTP_GET, handleSystem);
    g_server.on("/system/wifi-reset", HTTP_POST, []() {
      beginPage("Resetting Wi-Fi");
      sendChunk("<div class=\"card\"><p>Restarting into setup mode. This "
                "page will stop responding.</p></div>");
      endPage();
      g_pendingAction = Action::ResetWifi;
    });
    g_server.on("/system/check-update", HTTP_POST, []() {
      refresh::requestUpdateCheck(/*applyIfFound=*/false);
      g_server.sendHeader("Location", "/system", true);
      g_server.send(303, "text/plain", "");
    });
    g_server.on("/system/update", HTTP_POST, []() {
      refresh::requestUpdateCheck(/*applyIfFound=*/true);
      beginPage("Installing update");
      sendChunk("<div class=\"card\"><p>Downloading and verifying. The "
                "device restarts on its own when the image passes its hash "
                "check, and keeps running the current firmware if it does "
                "not.</p><p>Progress is on the serial log and the device "
                "screen.</p></div>");
      endPage();
    });
    // Two handlers: the second receives the body in chunks, the first runs
    // once it is complete. That is how Arduino's WebServer does uploads.
    g_server.on(
        "/system/ota", HTTP_POST,
        []() {
          const bool ok = ota::uploadEnd();
          beginPage(ok ? "Update installed" : "Update failed");
          if (ok) {
            sendChunk("<div class=\"card\"><p class=\"ok\">Installed. "
                      "Restarting.</p></div>");
          } else {
            sendChunk("<div class=\"card\"><p class=\"bad\">");
            sendEscaped(ota::lastError());
            sendChunk("</p><p>The current firmware is untouched.</p></div>");
          }
          endPage();
          if (ok) {
            delay(800);  // Let the page reach the browser first.
            ESP.restart();
          }
        },
        []() {
          HTTPUpload& upload = g_server.upload();
          switch (upload.status) {
            case UPLOAD_FILE_START:
              ota::uploadBegin(upload.totalSize);
              break;
            case UPLOAD_FILE_WRITE:
              ota::uploadWrite(upload.buf, upload.currentSize);
              break;
            case UPLOAD_FILE_ABORTED:
              ota::uploadAbort();
              break;
            default:
              break;
          }
        });
    g_server.on("/system/factory-reset", HTTP_POST, []() {
      beginPage("Factory reset");
      sendChunk("<div class=\"card\"><p>Erasing and restarting.</p></div>");
      endPage();
      g_pendingAction = Action::FactoryReset;
    });
  }
  g_server.onNotFound(handleNotFound);
  g_server.begin();

  Serial.printf("[web] server up (%s mode)\n",
                captive ? "captive setup" : "dashboard");
}

void tick() { g_server.handleClient(); }

bool settingsDirty() { return g_settingsDirty; }
void clearSettingsDirty() { g_settingsDirty = false; }
bool credentialsSubmitted() { return g_credentialsSubmitted; }

Action takeAction() {
  const Action a = g_pendingAction;
  g_pendingAction = Action::None;
  return a;
}

}  // namespace web
