/**
 * @file store.h
 * @brief Persistent storage: settings in NVS, cached documents in LittleFS.
 *
 * Two stores, deliberately, with a clear rule for which is which:
 *
 *   - **NVS** holds small, precious values: Wi-Fi credentials, API keys, the
 *     team selection, the API call tally. It survives a filesystem format, so
 *     wiping the cache can never cost the user their credentials.
 *   - **LittleFS** holds bulk documents: cached API payloads, the downloaded
 *     stylesheet, crest bitmaps. Cheap to erase and rebuild, because every
 *     byte of it can be re-fetched.
 *
 * LittleFS rather than SPIFFS (rule R3, and hard-won general experience):
 * SPIFFS is deprecated, has no wear levelling worth the name, and corrupts if
 * power is lost mid-write. LittleFS is power-fail safe by design and faster on
 * the small files we deal in.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "touch_input.h"

// Optional build-time key provisioning. Absent by default; see
// include/secrets_example.h. Only ever supplies *defaults* — anything entered
// through the web interface is stored in NVS and takes precedence.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SECRET_FOOTBALL_DATA_KEY
#define SECRET_FOOTBALL_DATA_KEY ""
#endif
#ifndef SECRET_API_SPORTS_KEY
#define SECRET_API_SPORTS_KEY ""
#endif

namespace store {

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

/// The documents we cache. Used as both the filename stem and the metadata key,
/// so the two can never drift apart.
enum class Doc : uint8_t {
  Standings,
  TeamMatches,
  OpponentMatches,
  Scorers,
  LiveMatch,
  Count,  ///< Sentinel; not a document.
};

/// Freshness state of a cached document.
struct DocStatus {
  bool     present   = false;  ///< A parseable file exists.
  bool     fresh     = false;  ///< Present and within its TTL.
  uint32_t fetchedAt = 0;      ///< Unix seconds, 0 if unknown.
  uint32_t ttl       = 0;      ///< Seconds.
  uint32_t size      = 0;      ///< Bytes on disk.
};

/**
 * Mount the filesystem, formatting it if it has never been used.
 *
 * @return false if the filesystem could not be mounted even after formatting,
 *         which means the cache is unavailable but the device can still run
 *         from live fetches. Callers must not treat this as fatal.
 */
bool begin();

/// True once begin() has succeeded.
bool ready();

/**
 * Write a cached document.
 *
 * **Atomic.** The payload goes to a temporary file, is flushed and closed, and
 * only then replaces the live file by rename. A reset or brownout partway
 * through therefore leaves either the previous document or none — never a
 * truncated one that fails to parse. This matters more than it might seem: the
 * device writes cache immediately after a network fetch, which is exactly when
 * current draw peaks and a marginal supply is most likely to sag.
 *
 * @param ttlSeconds how long the document should be considered fresh.
 */
bool writeDoc(Doc doc, const char* json, size_t length, uint32_t ttlSeconds);

/**
 * Read a cached document into a caller-supplied buffer.
 *
 * @param out      destination; always NUL-terminated on success.
 * @param capacity size of `out` including the terminator.
 * @return number of bytes read, or 0 on failure or if the document is absent.
 *         A document too large for the buffer fails rather than truncating,
 *         since a truncated JSON document is worse than no document.
 */
size_t readDoc(Doc doc, char* out, size_t capacity);

/// Freshness and size of a cached document, without reading its contents.
DocStatus statusOf(Doc doc);

/// Delete one cached document and forget its metadata.
bool clearDoc(Doc doc);

/// Delete every cached document. Settings in NVS are untouched.
bool clearAllDocs();

/// Bytes used and total, for the web UI's storage display.
void filesystemUsage(uint32_t& usedBytes, uint32_t& totalBytes);

/// Human-readable name, for logs and the web UI.
const char* docName(Doc doc);

// ---------------------------------------------------------------------------
// Settings (NVS)
// ---------------------------------------------------------------------------

/**
 * Device settings.
 *
 * Deliberately a flat struct of fixed-size fields: it is small enough to pass
 * around by value, and there is no heap involved at any point.
 */
struct Settings {
  static constexpr uint8_t kSsidLen = 33;   ///< 32 + terminator.
  static constexpr uint8_t kPassLen = 65;   ///< WPA2 max 64 + terminator.
  static constexpr uint8_t kKeyLen  = 65;
  static constexpr uint8_t kCodeLen = 8;

  char wifiSsid[kSsidLen] = {0};
  char wifiPass[kPassLen] = {0};

  char apiSportsKey[kKeyLen]    = SECRET_API_SPORTS_KEY;
  char footballDataKey[kKeyLen] = SECRET_FOOTBALL_DATA_KEY;

  /// Provider-qualified, because the two disagree: Bolton is 68 on api-sports
  /// and 60 on football-data, where 68 is Norwich City. A single `teamId`
  /// would fail silently by showing another club's data.
  uint16_t apiSportsTeamId    = 68;
  uint16_t footballDataTeamId = 60;
  char     competitionCode[kCodeLen] = "ELC";

  /// Display name, because the API returns Bolton as plain "Bolton".
  /**
   * Fallback name for the tracked club.
   *
   * Empty by default, deliberately. It used to default to a specific club,
   * which meant any device configured for a different team by id carried a
   * name that contradicted it — and nothing ever corrected the contradiction.
   * The club's real name comes from the standings; this is consulted only
   * before the first fetch has completed.
   */
  char teamDisplayName[24] = {0};

  uint32_t screenDwellMs = 12000;
  uint8_t  brightness    = 100;  ///< Percent.
  bool     autoBrightness = false;
  bool     soundEnabled   = false;
  /// Bit per screen; a clear bit hides that screen from the rotation.
  uint8_t  screenMask = 0xFF;
  bool     includeCups = false;

  /// Where to look for firmware releases.
  ///
  /// Deliberately configurable rather than compiled in: a fork should update
  /// from its own releases, not from this repository's.
  static constexpr uint8_t kUrlLen = 160;
  char otaManifestUrl[kUrlLen] = {0};
  /// Check for updates automatically. Checking is harmless; *applying* always
  /// needs an explicit action, because silently replacing firmware someone is
  /// relying on is not a decision to make on their behalf.
  bool otaAutoCheck = true;

  /// Persisted touch calibration, so a calibrated device stays calibrated.
  touch::Calibration touch{};

  bool hasWifi() const { return wifiSsid[0] != '\0'; }
  bool hasFootballDataKey() const { return footballDataKey[0] != '\0'; }
};

/// Load settings from NVS, leaving defaults in place for anything unset.
void loadSettings(Settings& out);

/// Persist settings to NVS.
bool saveSettings(const Settings& in);

/// Erase all settings, returning the device to first-boot state.
bool factoryReset();

// ---------------------------------------------------------------------------
// API quota tracking
// ---------------------------------------------------------------------------

/**
 * Daily API call tally, persisted so a reboot does not lose the count.
 *
 * The server's own figure is authoritative — every api-sports response carries
 * the remaining daily allowance — so this exists for the *pre-flight* decision
 * of whether a call can be afforded before it is made, and is reconciled to
 * the server's number afterwards.
 */
struct Quota {
  uint16_t used  = 0;
  uint16_t limit = 100;
  /// Days since the epoch, so the midnight-UTC reset is a simple comparison
  /// rather than date arithmetic.
  uint32_t dayNumber = 0;
};

void loadQuota(Quota& out);
bool saveQuota(const Quota& in);

}  // namespace store
