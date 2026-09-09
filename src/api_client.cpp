/**
 * @file api_client.cpp
 * @brief HTTPS + streaming JSON. See api_client.h for the design.
 */

#include "api_client.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "assets_root_certs.h"
#include "store.h"

namespace api {
namespace {

const char* g_apiSportsKey    = nullptr;
const char* g_footballDataKey = nullptr;

/// Never spend the last of the daily allowance automatically. The reserve is
/// kept for a refresh the user explicitly asks for from the web UI, so a
/// device that has quietly exhausted itself can still be made to show
/// something current on demand.
constexpr uint16_t kDailyReserve = 10;

/// Minimum gap between calls to one provider. Both allow 10/min; 7 s is
/// comfortably inside that while still letting a burst of screens refresh in
/// reasonable time.
constexpr uint32_t kMinSpacingMs = 7000;

/// Cap on a single response. Nothing we request legitimately approaches this;
/// it exists so a provider malfunction cannot stream indefinitely into a
/// parser and hang the device.
constexpr uint32_t kMaxResponseBytes = 192UL * 1024UL;

/// Overall deadline for one request. A stalled TLS socket must not wedge the
/// UI, which shares this core.
constexpr uint32_t kRequestTimeoutMs = 12000;

uint32_t g_lastCallMs[2] = {0, 0};

struct Endpoint {
  const char* host;
  const char* authHeader;
};

Endpoint endpointFor(Provider p) {
  switch (p) {
    case Provider::ApiSports:
      return {"v3.football.api-sports.io", "x-apisports-key"};
    case Provider::FootballData:
    default:
      return {"api.football-data.org", "X-Auth-Token"};
  }
}

const char* keyFor(Provider p) {
  return p == Provider::ApiSports ? g_apiSportsKey : g_footballDataKey;
}

uint8_t indexOf(Provider p) { return p == Provider::ApiSports ? 1 : 0; }

/// True once NTP has produced a plausible time.
bool clockIsSet() { return time(nullptr) > 1600000000L; }

/// Read one header line into `out`; returns false at the end of the headers.
bool readHeaderLine(WiFiClientSecure& client, char* out, size_t capacity,
                    uint32_t deadline) {
  size_t len = 0;
  while (millis() < deadline) {
    if (!client.available()) {
      if (!client.connected()) return false;
      delay(2);
      continue;
    }
    const int c = client.read();
    if (c < 0) continue;
    if (c == '\n') {
      // Trim the CR and terminate.
      while (len > 0 && (out[len - 1] == '\r')) --len;
      out[len] = '\0';
      return true;
    }
    if (len < capacity - 1) out[len++] = static_cast<char>(c);
  }
  return false;
}

/**
 * A Stream that yields while waiting, and un-chunks the body.
 *
 * Two problems solved in one place.
 *
 * **Yielding.** Arduino's Stream::timedRead() — which ArduinoJson uses to read
 * the socket — busy-loops calling read() until data arrives or the timeout
 * expires, with no yield anywhere in it:
 *
 *     do { c = read(); if (c >= 0) return c; }
 *     while (millis() - _startMillis < _timeout);
 *
 * On core 0 at priority 1 that starves the idle task and the task watchdog
 * panics. Every wait here yields instead.
 *
 * **De-chunking.** football-data.org replies with `Transfer-Encoding:
 * chunked`, and the raw body is therefore a sequence of hex length lines
 * wrapping the JSON. Handing that to a parser fails in a genuinely deceptive
 * way rather than an obvious one: ArduinoJson reads the leading digits of the
 * first chunk header as a JSON *number*, reports success, and yields a
 * document containing an integer. The symptom is HTTP 200, no parse error, and
 * zero rows — which looks like a filter bug and is not one. This strips the
 * chunk framing so the parser sees only the body.
 */
class YieldingStream : public Stream {
 public:
  YieldingStream(WiFiClientSecure& client, uint32_t deadline, bool chunked)
      : client_(client), deadline_(deadline), chunked_(chunked) {}

  int available() override { return client_.available(); }
  int peek() override { return client_.peek(); }
  size_t write(uint8_t) override { return 0; }  // Read-only.

  int read() override {
    if (!chunked_) return rawRead();
    if (!ensureChunk()) return -1;
    const int b = rawRead();
    if (b >= 0 && chunkRemaining_ > 0) --chunkRemaining_;
    if (chunkRemaining_ == 0) consumeChunkTrailer();
    return b;
  }

  size_t readBytes(char* buffer, size_t length) override {
    size_t total = 0;
    while (total < length) {
      if (chunked_) {
        if (!ensureChunk()) break;
        const size_t want = min(length - total, chunkRemaining_);
        const size_t got = rawReadBytes(buffer + total, want);
        if (got == 0) break;
        total           += got;
        chunkRemaining_ -= got;
        if (chunkRemaining_ == 0) consumeChunkTrailer();
      } else {
        const size_t got = rawReadBytes(buffer + total, length - total);
        if (got == 0) break;
        total += got;
      }
    }
    bytesRead_ += total;
    return total;
  }

  uint32_t bytesRead() const { return bytesRead_; }

 private:
  /// True when no more data can arrive: peer closed and buffer drained, or
  /// the deadline has passed.
  bool exhausted() const {
    if (millis() > deadline_) return true;
    return !client_.connected() && client_.available() == 0;
  }

  int rawRead() {
    for (;;) {
      const int b = client_.read();
      if (b >= 0) {
        ++bytesRead_;
        return b;
      }
      if (exhausted()) return -1;
      vTaskDelay(1);  // The yield that keeps the idle task alive.
    }
  }

  size_t rawReadBytes(char* buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
      const int avail = client_.available();
      if (avail > 0) {
        const size_t want = min(static_cast<size_t>(avail), length - total);
        const int got =
            client_.read(reinterpret_cast<uint8_t*>(buffer + total), want);
        if (got > 0) {
          total += static_cast<size_t>(got);
          continue;
        }
      }
      if (exhausted()) break;
      vTaskDelay(1);
    }
    return total;
  }

  /// Make sure chunkRemaining_ describes a chunk with data left in it.
  /// @return false at the terminating zero-length chunk or on error.
  bool ensureChunk() {
    if (finished_) return false;
    if (chunkRemaining_ > 0) return true;

    // Chunk header: hex length, optional ";extension", then CRLF.
    uint32_t size = 0;
    bool sawDigit = false;
    for (;;) {
      const int c = rawRead();
      if (c < 0) {
        finished_ = true;
        return false;
      }
      if (c == '\n') break;
      if (c == '\r' || c == ';') continue;
      const int v = hexValue(c);
      if (v < 0) continue;  // Skip anything unexpected rather than abort.
      size = size * 16 + static_cast<uint32_t>(v);
      sawDigit = true;
    }
    if (!sawDigit || size == 0) {
      finished_ = true;  // Terminating chunk.
      return false;
    }
    chunkRemaining_ = size;
    return true;
  }

  /// Consume the CRLF that follows a chunk's data.
  void consumeChunkTrailer() {
    for (uint8_t i = 0; i < 2; ++i) {
      const int c = client_.peek();
      if (c == '\r' || c == '\n') {
        client_.read();
      } else {
        break;
      }
    }
  }

  static int hexValue(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  WiFiClientSecure& client_;
  uint32_t deadline_;
  bool     chunked_;
  size_t   chunkRemaining_ = 0;
  bool     finished_       = false;
  uint32_t bytesRead_      = 0;
};

/// Case-insensitive prefix test, since header names are not case-sensitive.
bool headerIs(const char* line, const char* name) {
  while (*name != '\0') {
    if (tolower(static_cast<unsigned char>(*line)) !=
        tolower(static_cast<unsigned char>(*name))) {
      return false;
    }
    ++line;
    ++name;
  }
  return *line == ':';
}

/// Value portion of a header line, with leading spaces skipped.
const char* headerValue(const char* line) {
  const char* colon = strchr(line, ':');
  if (colon == nullptr) return "";
  ++colon;
  while (*colon == ' ') ++colon;
  return colon;
}

}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::Ok:             return "ok";
    case Result::NoNetwork:      return "no network";
    case Result::ClockNotSet:    return "clock not set";
    case Result::QuotaExhausted: return "quota exhausted";
    case Result::RateLimited:    return "rate limited";
    case Result::HttpError:      return "http error";
    case Result::TlsError:       return "tls error";
    case Result::ParseError:     return "parse error";
    default:                     return "unknown";
  }
}

void begin(const char* apiSportsKey, const char* footballDataKey) {
  g_apiSportsKey    = apiSportsKey;
  g_footballDataKey = footballDataKey;
}

uint16_t cooldownRemaining(Provider provider) {
  const uint32_t since = millis() - g_lastCallMs[indexOf(provider)];
  if (since >= kMinSpacingMs) return 0;
  return static_cast<uint16_t>((kMinSpacingMs - since + 999) / 1000);
}

bool canAfford(Provider provider) {
  if (cooldownRemaining(provider) > 0) return false;

  // Only api-sports has a daily cap worth rationing. football-data.org has no
  // documented daily limit, so the per-minute spacing above is the whole of
  // its budget management.
  if (provider != Provider::ApiSports) return true;

  store::Quota q;
  store::loadQuota(q);
  const uint16_t remaining = q.limit > q.used ? q.limit - q.used : 0;
  return remaining > kDailyReserve;
}

Response fetch(Provider provider, const char* path, JsonDocument& doc,
               JsonDocument& filter) {
  Response resp;
  const uint32_t started = millis();

  if (WiFi.status() != WL_CONNECTED) {
    resp.result = Result::NoNetwork;
    return resp;
  }
  // Certificate validation compares against the certificate's validity dates,
  // so it cannot succeed before NTP. Reported distinctly rather than as a
  // generic TLS failure, because the fix is entirely different: wait.
  if (!clockIsSet()) {
    resp.result = Result::ClockNotSet;
    return resp;
  }
  if (cooldownRemaining(provider) > 0) {
    resp.result = Result::RateLimited;
    return resp;
  }
  if (!canAfford(provider)) {
    resp.result = Result::QuotaExhausted;
    return resp;
  }

  const Endpoint ep = endpointFor(provider);
  const char* key = keyFor(provider);
  if (key == nullptr || key[0] == '\0') {
    resp.result = Result::HttpError;
    Serial.printf("[api] no key configured for %s\n", ep.host);
    return resp;
  }

  // Scoped so the client — and its ~45 KB of TLS buffers — is destroyed
  // promptly. Holding one open between requests would keep that memory
  // reserved for the ~99.9%% of the time we are not fetching.
  WiFiClientSecure client;
  client.setCACert(assets::kRootCaBundle);
  // Short, because a socket timeout is also how long a stalled read can hold
  // core 0. The yielding stream above means that no longer starves the idle
  // task, but a shorter deadline still fails faster and more predictably.
  client.setTimeout(5);

  Serial.printf("[api] connecting %s (heap %luK)\n", ep.host,
                (unsigned long)(ESP.getFreeHeap() / 1024));
  const uint32_t connectStart = millis();
  if (!client.connect(ep.host, 443)) {
    resp.result = Result::TlsError;
    Serial.printf("[api] TLS connect failed: %s\n", ep.host);
    return resp;
  }
  const uint32_t handshakeMs = millis() - connectStart;
  const uint32_t heapAfterTls = ESP.getFreeHeap();
  Serial.printf("[api] TLS up in %lums (heap %luK)\n",
                (unsigned long)handshakeMs,
                (unsigned long)(heapAfterTls / 1024));

  // Count the call the moment it is actually sent. Counting earlier would
  // penalise failures that never reached the provider; counting later would
  // miss a call that succeeded but whose response we failed to read.
  g_lastCallMs[indexOf(provider)] = millis();

  client.printf(
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "%s: %s\r\n"
      "Accept: application/json\r\n"
      "User-Agent: ESP32-FootballTracker/1.0\r\n"
      "Connection: close\r\n\r\n",
      path, ep.host, ep.authHeader, key);

  const uint32_t deadline = millis() + kRequestTimeoutMs;

  // --- Status line --------------------------------------------------------
  char line[192];
  if (!readHeaderLine(client, line, sizeof(line), deadline)) {
    resp.result = Result::HttpError;
    client.stop();
    return resp;
  }
  // "HTTP/1.1 200 OK"
  const char* space = strchr(line, ' ');
  resp.httpStatus = (space != nullptr) ? atoi(space + 1) : 0;

  // --- Headers ------------------------------------------------------------
  // Quota is read from the provider's own headers and treated as
  // authoritative. A local tally drifts — across reboots, the midnight reset,
  // requests that failed after being counted, or another client using the same
  // key — whereas the provider always knows.
  bool chunked = false;
  while (readHeaderLine(client, line, sizeof(line), deadline)) {
    if (line[0] == '\0') break;  // Blank line ends the headers.
    if (headerIs(line, "transfer-encoding")) {
      chunked = (strstr(headerValue(line), "chunked") != nullptr);
    } else if (headerIs(line, "x-ratelimit-requests-remaining")) {
      resp.quotaRemaining = atoi(headerValue(line));
    } else if (headerIs(line, "x-ratelimit-remaining")) {
      resp.minuteRemaining = atoi(headerValue(line));
    } else if (headerIs(line, "x-requests-available-minute")) {
      resp.minuteRemaining = atoi(headerValue(line));
    }
  }

  if (resp.httpStatus == 429) {
    resp.result = Result::RateLimited;
    client.stop();
    Serial.printf("[api] rate limited by %s\n", ep.host);
    return resp;
  }
  if (resp.httpStatus < 200 || resp.httpStatus >= 300) {
    resp.result = Result::HttpError;
    client.stop();
    Serial.printf("[api] HTTP %u from %s%s\n", resp.httpStatus, ep.host, path);
    return resp;
  }

  // --- Body, streamed straight into the parser ----------------------------
  doc.clear();
  YieldingStream stream(client, deadline, chunked);
  const DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));

  resp.bytesReceived = stream.bytesRead();
  client.stop();
  resp.elapsedMs = millis() - started;

  if (err) {
    resp.result = Result::ParseError;
    Serial.printf("[api] parse failed: %s\n", err.c_str());
    return resp;
  }

  // --- Reconcile the quota ------------------------------------------------
  if (provider == Provider::ApiSports) {
    store::Quota q;
    store::loadQuota(q);
    if (resp.quotaRemaining >= 0 && resp.quotaRemaining <= q.limit) {
      // Trust the provider over our own count.
      const uint16_t used = q.limit - static_cast<uint16_t>(resp.quotaRemaining);
      if (used != q.used) {
        Serial.printf("[api] quota reconciled: %u -> %u used\n", q.used, used);
        q.used = used;
      }
    } else {
      ++q.used;  // No header; fall back to counting.
    }
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    q.dayNumber = now / 86400UL;
    store::saveQuota(q);
  }

  resp.result = Result::Ok;
  Serial.printf(
      "[api] %s -> %u, %lu bytes in %lums (tls %lums, heap %luK, quota %d)\n",
      path, resp.httpStatus, (unsigned long)resp.bytesReceived,
      (unsigned long)resp.elapsedMs, (unsigned long)handshakeMs,
      (unsigned long)(heapAfterTls / 1024), resp.quotaRemaining);
  return resp;
}

}  // namespace api
