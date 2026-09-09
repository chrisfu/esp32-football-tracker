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

namespace web {
namespace {

WebServer               g_server(80);
store::Settings*        g_settings = nullptr;
const model::Snapshot*  g_data     = nullptr;
bool g_captive            = false;
bool g_settingsDirty      = false;
bool g_credentialsSubmitted = false;

/**
 * Send a chunk of HTML.
 *
 * Pages are streamed in pieces rather than assembled into one String. A String
 * built up by concatenation would repeatedly reallocate and fragment a heap
 * whose largest contiguous block is only ~112 KB — and fragmentation is the
 * failure mode that eventually refuses a TLS handshake, which is far harder to
 * diagnose than a slightly more verbose page builder.
 */
void sendChunk(const char* html) { g_server.sendContent(html); }

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

void endPage() {
  sendChunk("</body></html>");
  g_server.sendContent("");  // Terminates the chunked response.
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

void row(const char* label, const char* value, const char* cls = nullptr) {
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

void handleDashboard() {
  beginPage("Football Tracker");

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
  sendChunk("</table></div>");

  sendChunk("<div class=\"card\"><h2>Settings</h2>"
            "<p style=\"font-size:.85rem;color:#aaa\">Team selection, screen "
            "rotation, dwell time, brightness and cache controls arrive with "
            "the next stage of work.</p></div>");
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

}  // namespace web
