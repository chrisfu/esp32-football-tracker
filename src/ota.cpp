/**
 * @file ota.cpp
 * @brief Firmware update implementation. See ota.h.
 */

#include "ota.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include "assets_root_certs.h"

namespace ota {
namespace {

const char* g_lastError = "";

/// Overall deadline for a firmware download. Generous: a 1.2 MB image over a
/// weak signal is genuinely slow, and giving up early would be worse than
/// waiting.
constexpr uint32_t kDownloadTimeoutMs = 180000;
constexpr uint32_t kManifestTimeoutMs = 15000;

/// Redirects to follow. GitHub release downloads redirect exactly once, from
/// github.com to objects.githubusercontent.com; two is headroom, and a bound
/// stops a redirect loop from hanging the device.
constexpr uint8_t kMaxRedirects = 2;

/// Split a URL into host and path. Returns false for anything not https.
bool parseUrl(const char* url, char* host, size_t hostLen, char* path,
              size_t pathLen) {
  // Only https is accepted. Firmware fetched over plain http could be
  // replaced in flight, and the device executes it.
  if (strncmp(url, "https://", 8) != 0) return false;
  const char* start = url + 8;
  const char* slash = strchr(start, '/');
  const size_t hLen = (slash != nullptr) ? static_cast<size_t>(slash - start)
                                         : strlen(start);
  if (hLen == 0 || hLen >= hostLen) return false;
  memcpy(host, start, hLen);
  host[hLen] = '\0';
  snprintf(path, pathLen, "%s", (slash != nullptr) ? slash : "/");
  return true;
}

/// Read one header line. Returns false at the end of the headers or on error.
bool readLine(WiFiClientSecure& client, char* out, size_t capacity,
              uint32_t deadline) {
  size_t len = 0;
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) return false;
      vTaskDelay(1);
      continue;
    }
    const int c = client.read();
    if (c < 0) continue;
    if (c == '\n') {
      while (len > 0 && out[len - 1] == '\r') --len;
      out[len] = '\0';
      return true;
    }
    if (len < capacity - 1) out[len++] = static_cast<char>(c);
  }
  return false;
}

/**
 * Open a URL, following redirects, and leave the client positioned at the
 * body.
 *
 * @param contentLength filled in when the server states one.
 * @return true with `client` connected and headers consumed.
 */
bool openUrl(WiFiClientSecure& client, const char* url, uint32_t timeoutMs,
             size_t& contentLength, char* finalUrl, size_t finalUrlLen) {
  char current[256];
  snprintf(current, sizeof(current), "%s", url);

  for (uint8_t hop = 0; hop <= kMaxRedirects; ++hop) {
    char host[128], path[224];
    if (!parseUrl(current, host, sizeof(host), path, sizeof(path))) {
      g_lastError = "URL must be https";
      return false;
    }

    client.stop();
    client.setCACert(assets::kRootCaBundle);
    client.setTimeout(10);
    if (!client.connect(host, 443)) {
      g_lastError = "TLS connect failed";
      return false;
    }

    client.printf(
        "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "User-Agent: ESP32-FootballTracker/%s\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n",
        path, host, currentVersion());

    const uint32_t deadline = millis() + timeoutMs;
    char line[512];
    if (!readLine(client, line, sizeof(line), deadline)) {
      g_lastError = "no response";
      return false;
    }
    const char* sp = strchr(line, ' ');
    const int status = (sp != nullptr) ? atoi(sp + 1) : 0;

    char location[256] = {0};
    contentLength = 0;
    while (readLine(client, line, sizeof(line), deadline)) {
      if (line[0] == '\0') break;
      if (strncasecmp(line, "location:", 9) == 0) {
        const char* v = line + 9;
        while (*v == ' ') ++v;
        snprintf(location, sizeof(location), "%s", v);
      } else if (strncasecmp(line, "content-length:", 15) == 0) {
        contentLength = static_cast<size_t>(atol(line + 15));
      }
    }

    if (status >= 300 && status < 400 && location[0] != '\0') {
      Serial.printf("[ota] redirect -> %.60s...\n", location);
      snprintf(current, sizeof(current), "%s", location);
      continue;
    }
    if (status != 200) {
      static char err[48];
      snprintf(err, sizeof(err), "HTTP %d", status);
      g_lastError = err;
      client.stop();
      return false;
    }
    if (finalUrl != nullptr) snprintf(finalUrl, finalUrlLen, "%s", current);
    return true;
  }

  g_lastError = "too many redirects";
  return false;
}

/// Lowercase hex of a digest.
void toHex(const uint8_t* digest, size_t len, char* out) {
  static const char* kHex = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2]     = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

bool g_uploadActive = false;

}  // namespace

const char* currentVersion() {
#ifdef FIRMWARE_VERSION
  return FIRMWARE_VERSION;
#else
  return "0.0.0";
#endif
}

const char* currentBuild() {
#ifdef FIRMWARE_BUILD
  return FIRMWARE_BUILD;
#else
  return "unknown";
#endif
}

const char* lastError() { return g_lastError; }

bool isStableVersion(const char* version) {
  if (version == nullptr) return false;
  if (*version == 'v') ++version;

  int major = -1, minor = -1, patch = -1, consumed = 0;
  if (sscanf(version, "%d.%d.%d%n", &major, &minor, &patch, &consumed) != 3) {
    return false;
  }
  // The %n is the point: it tells us where parsing stopped, so anything
  // trailing — "-rc1", "-alpha2", a stray "+build" — is rejected rather than
  // silently ignored. Without it "0.2.0-rc1" would parse as 0.2.0 and pass.
  if (version[consumed] != '\0') return false;
  return major >= 0 && minor >= 0 && patch >= 0;
}

int compareVersions(const char* a, const char* b) {
  int amaj = 0, amin = 0, apat = 0, bmaj = 0, bmin = 0, bpat = 0;
  int aConsumed = 0, bConsumed = 0;
  // Leading 'v' is tolerated so a tag name works as well as a bare version.
  if (*a == 'v') ++a;
  if (*b == 'v') ++b;
  sscanf(a, "%d.%d.%d%n", &amaj, &amin, &apat, &aConsumed);
  sscanf(b, "%d.%d.%d%n", &bmaj, &bmin, &bpat, &bConsumed);
  if (amaj != bmaj) return amaj > bmaj ? 1 : -1;
  if (amin != bmin) return amin > bmin ? 1 : -1;
  if (apat != bpat) return apat > bpat ? 1 : -1;

  // Triples match. A pre-release ranks below the plain release, per SemVer,
  // so a device running 0.2.0-rc1 is correctly offered 0.2.0. Build metadata
  // (after '+') is not a pre-release and does not affect precedence.
  const bool aPre = (a[aConsumed] == '-');
  const bool bPre = (b[bConsumed] == '-');
  if (aPre != bPre) return aPre ? -1 : 1;
  return 0;
}

bool checkForUpdate(const char* manifestUrl, UpdateInfo& out) {
  out = UpdateInfo{};
  if (manifestUrl == nullptr || manifestUrl[0] == '\0') {
    g_lastError = "no manifest URL configured";
    return false;
  }

  WiFiClientSecure client;
  size_t length = 0;
  if (!openUrl(client, manifestUrl, kManifestTimeoutMs, length, nullptr, 0)) {
    return false;
  }

  // The manifest is tiny, so it is read whole rather than streamed. Bounded
  // regardless, so a wrong URL returning a large page cannot exhaust the heap.
  constexpr size_t kMaxManifest = 2048;
  char buffer[kMaxManifest];
  size_t got = 0;
  const uint32_t deadline = millis() + kManifestTimeoutMs;
  while (got < kMaxManifest - 1 && millis() < deadline) {
    const int avail = client.available();
    if (avail > 0) {
      const int n = client.read(reinterpret_cast<uint8_t*>(buffer + got),
                                min(static_cast<size_t>(avail),
                                    kMaxManifest - 1 - got));
      if (n > 0) {
        got += static_cast<size_t>(n);
        continue;
      }
    }
    if (!client.connected() && client.available() == 0) break;
    vTaskDelay(1);
  }
  client.stop();
  buffer[got] = '\0';

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, buffer, got);
  if (err) {
    g_lastError = "manifest is not valid JSON";
    return false;
  }

  snprintf(out.version, sizeof(out.version), "%s", doc["version"] | "");
  snprintf(out.url, sizeof(out.url), "%s", doc["url"] | "");
  snprintf(out.sha256, sizeof(out.sha256), "%s", doc["sha256"] | "");
  out.size = doc["size"] | 0;

  if (out.version[0] == '\0' || out.url[0] == '\0') {
    g_lastError = "manifest missing version or url";
    return false;
  }
  // Pre-releases are refused here rather than merely not preferred. The
  // manifest should only ever advertise stable builds, but the device does not
  // rely on that: a mistake in the pipeline, or a hand-edited manifest, must
  // not be able to push a release candidate onto a device that is simply
  // sitting on a shelf.
  if (!isStableVersion(out.version)) {
    Serial.printf("[ota] refusing non-stable release \"%s\"\n", out.version);
    g_lastError = "manifest advertises a pre-release; ignoring";
    return false;
  }
  // A manifest without a hash is refused rather than trusted. The hash is the
  // only thing standing between a corrupted download and a bricked device.
  if (strlen(out.sha256) != 64) {
    g_lastError = "manifest missing a sha256";
    return false;
  }

  out.available = compareVersions(out.version, currentVersion()) > 0;
  Serial.printf("[ota] manifest %s, running %s -> %s\n", out.version,
                currentVersion(),
                out.available ? "update available" : "up to date");
  g_lastError = "";
  return true;
}

bool applyUpdate(const UpdateInfo& info, ProgressFn progress) {
  if (!info.available) {
    g_lastError = "no update available";
    return false;
  }
  // Checked again at the point of installing, not just when the manifest was
  // read. The two happen at different times and the struct is reachable from
  // elsewhere; re-asserting the rule costs nothing and closes the gap.
  if (!isStableVersion(info.version)) {
    g_lastError = "refusing to install a pre-release";
    return false;
  }

  WiFiClientSecure client;
  size_t length = 0;
  if (!openUrl(client, info.url, kDownloadTimeoutMs, length, nullptr, 0)) {
    return false;
  }
  // Prefer the server's length; fall back to the manifest's.
  const size_t total = (length > 0) ? length : info.size;
  if (total == 0) {
    g_lastError = "unknown firmware size";
    client.stop();
    return false;
  }
  if (info.size != 0 && length != 0 && info.size != length) {
    // A disagreement means the manifest and the asset are out of step, which
    // is exactly the situation where continuing is unwise.
    g_lastError = "size mismatch between manifest and download";
    client.stop();
    return false;
  }

  Serial.printf("[ota] downloading %u bytes of %s\n", (unsigned)total,
                info.version);
  if (!Update.begin(total)) {
    g_lastError = "not enough space in the OTA partition";
    client.stop();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);  // 0 = SHA-256, not SHA-224.

  uint8_t chunk[1024];
  size_t written = 0;
  uint8_t lastPercent = 255;
  const uint32_t deadline = millis() + kDownloadTimeoutMs;

  while (written < total && millis() < deadline) {
    const int avail = client.available();
    if (avail <= 0) {
      if (!client.connected() && client.available() == 0) break;
      vTaskDelay(1);
      continue;
    }
    const size_t want = min(sizeof(chunk),
                            min(static_cast<size_t>(avail), total - written));
    const int n = client.read(chunk, want);
    if (n <= 0) continue;

    // Hashed and written in the same pass, so the image is never held twice.
    mbedtls_sha256_update_ret(&sha, chunk, static_cast<size_t>(n));
    if (Update.write(chunk, static_cast<size_t>(n)) !=
        static_cast<size_t>(n)) {
      g_lastError = "flash write failed";
      Update.abort();
      mbedtls_sha256_free(&sha);
      client.stop();
      return false;
    }
    written += static_cast<size_t>(n);

    if (progress != nullptr) {
      const uint8_t pct = static_cast<uint8_t>((written * 100) / total);
      if (pct != lastPercent) {
        lastPercent = pct;
        progress(pct);
      }
    }
  }
  client.stop();

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (written != total) {
    Serial.printf("[ota] short download: %u of %u\n", (unsigned)written,
                  (unsigned)total);
    g_lastError = "download incomplete";
    Update.abort();
    return false;
  }

  char hex[65];
  toHex(digest, sizeof(digest), hex);
  if (strcasecmp(hex, info.sha256) != 0) {
    // Aborting here is what makes this safe: ESP-IDF only switches the boot
    // partition when end() succeeds, so a rejected image never runs.
    Serial.printf("[ota] hash mismatch\n  expected %s\n  got      %s\n",
                  info.sha256, hex);
    g_lastError = "sha256 mismatch -- update rejected";
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    static char err[64];
    snprintf(err, sizeof(err), "commit failed (%s)", Update.errorString());
    g_lastError = err;
    return false;
  }

  Serial.printf("[ota] %s verified and installed; restarting\n", info.version);
  g_lastError = "";
  delay(500);
  ESP.restart();
  return true;  // Not reached.
}

// ---------------------------------------------------------------------------
// Web upload
// ---------------------------------------------------------------------------

bool uploadBegin(size_t expectedSize) {
  // Size zero means the browser sent no length; UPDATE_SIZE_UNKNOWN lets
  // ESP-IDF work it out from the image header instead of refusing.
  const size_t size = (expectedSize > 0) ? expectedSize : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(size)) {
    static char err[64];
    snprintf(err, sizeof(err), "cannot start update (%s)",
             Update.errorString());
    g_lastError = err;
    return false;
  }
  g_uploadActive = true;
  g_lastError = "";
  Serial.println(F("[ota] web upload started"));
  return true;
}

bool uploadWrite(const uint8_t* data, size_t length) {
  if (!g_uploadActive) return false;
  if (Update.write(const_cast<uint8_t*>(data), length) != length) {
    g_lastError = "flash write failed";
    Update.abort();
    g_uploadActive = false;
    return false;
  }
  return true;
}

bool uploadEnd() {
  if (!g_uploadActive) return false;
  g_uploadActive = false;
  if (!Update.end(true)) {
    static char err[64];
    snprintf(err, sizeof(err), "commit failed (%s)", Update.errorString());
    g_lastError = err;
    return false;
  }
  // An uploaded image carries no manifest, so there is no hash to check it
  // against. ESP-IDF still validates the image header and checksum, which
  // catches a truncated or corrupt upload — but not a wrong-but-valid binary.
  // That is an accepted difference from the pull path, where a hash is
  // mandatory, and it is why the upload page says what it is asking for.
  Serial.println(F("[ota] web upload installed; restarting"));
  g_lastError = "";
  return true;
}

void uploadAbort() {
  if (g_uploadActive) Update.abort();
  g_uploadActive = false;
}

}  // namespace ota
