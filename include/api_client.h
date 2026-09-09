/**
 * @file api_client.h
 * @brief HTTPS requests that stream directly into a filtered JSON parser.
 *
 * The central constraint (SPEC.md §5): a standings response can be ~100 KB of
 * JSON, and the largest contiguous heap block on this board is about 112 KB —
 * less once TLS has taken its ~45 KB. Buffering a response and then parsing it
 * would need the body twice over and is simply not affordable.
 *
 * So nothing is buffered. The TLS stream is handed straight to ArduinoJson,
 * which is given a filter describing the handful of fields we actually want.
 * Everything else is discarded as it arrives, and only the filtered result is
 * ever resident.
 */

#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#include "store.h"

namespace api {

/// Which provider a request goes to. They differ in host, auth header and
/// — importantly — in whether they have a daily quota worth rationing.
enum class Provider : uint8_t {
  FootballData,  ///< football-data.org: 10/min, no daily cap.
  ApiSports,     ///< api-sports.io: 100/day and 10/min. Rationed.
};

/// Why a request did not produce data. Distinguished because the right
/// response differs: a quota block should not be retried, a network error
/// should.
enum class Result : uint8_t {
  Ok,
  NoNetwork,
  ClockNotSet,    ///< TLS cannot validate certificate dates without a clock.
  QuotaExhausted, ///< Refused before sending, to protect the reserve.
  RateLimited,    ///< Provider said slow down.
  HttpError,
  TlsError,
  ParseError,
};

/// Human-readable result, for logs and the web UI.
const char* resultName(Result r);

/// Outcome of a request, including what the provider said about our quota.
struct Response {
  Result   result      = Result::NoNetwork;
  uint16_t httpStatus  = 0;
  /// Daily allowance remaining, as reported by the provider. -1 if unstated.
  int16_t  quotaRemaining = -1;
  /// Per-minute allowance remaining, or -1.
  int16_t  minuteRemaining = -1;
  uint32_t bytesReceived = 0;
  uint32_t elapsedMs     = 0;

  bool ok() const { return result == Result::Ok; }
};

/// Configure keys. Held by pointer to the caller's settings, not copied.
void begin(const char* apiSportsKey, const char* footballDataKey);

/**
 * Perform a GET and parse the response into `doc` through `filter`.
 *
 * @param path  path and query, e.g. "/v4/competitions/ELC/standings".
 * @param doc   destination document, cleared before use.
 * @param filter ArduinoJson filter selecting the fields to keep. Passing an
 *        empty filter would keep everything, which is exactly what we cannot
 *        afford, so a filter is required rather than optional.
 * @param capacityHint reserved for future use by callers that know their size.
 */
Response fetch(Provider provider, const char* path, JsonDocument& doc,
               JsonDocument& filter);

/**
 * Serialise an already-filtered document to the cache.
 *
 * Kept separate from fetch() so the caller decides what is worth persisting,
 * and so the same document can be parsed once and stored once. What is written
 * is the *filtered* result, not the response — a 62 KB scorers reply reduces to
 * a couple of kilobytes on disk, which is rule R3 in practice.
 */
bool persist(store::Doc doc, const JsonDocument& parsed, uint32_t ttlSeconds);

/**
 * Whether a request to this provider can be afforded right now.
 *
 * Checks the daily reserve and the minimum spacing between calls. Callers
 * should consult this before doing expensive preparation, though fetch()
 * enforces it again — the check is advisory, the enforcement is not.
 */
bool canAfford(Provider provider);

/// Seconds until the next call to this provider would be permitted, 0 if now.
uint16_t cooldownRemaining(Provider provider);

}  // namespace api
