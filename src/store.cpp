/**
 * @file store.cpp
 * @brief Storage implementation. See store.h for the design.
 */

#include "store.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace store {
namespace {

bool g_ready = false;

/// NVS namespaces. Settings and quota are separated so clearing the tally can
/// never risk the credentials.
constexpr const char* kNsSettings = "fbtracker";
constexpr const char* kNsQuota    = "fbquota";

/// Cache lives in its own directory so it can be cleared wholesale without
/// touching the stylesheet or crest caches that will sit alongside it.
constexpr const char* kCacheDir = "/cache";

/// VFS mount point for the LittleFS partition — Arduino's default.
///
/// Needed because the POSIX calls below address the mounted path, while the
/// Arduino LittleFS API takes partition-relative paths.
constexpr const char* kFsBase = "/littlefs";

/**
 * Delete a file, silently, whether or not it exists.
 *
 * Uses POSIX unlink() rather than LittleFS.remove() deliberately. Both the
 * Arduino remove() *and* exists() log at ERROR level when a file is absent —
 * so guarding a remove with an exists check merely swaps one spurious error
 * line for another. Since deleting a not-yet-existing file is the normal case
 * on every first write, that noise would appear on every boot and make the log
 * untrustworthy. unlink() simply returns -1 and says nothing.
 */
void removeQuietly(const char* path) {
  char full[80];
  snprintf(full, sizeof(full), "%s%s", kFsBase, path);
  ::unlink(full);  // Absence is expected; there is nothing to report.
}

/**
 * Whether a file exists, and how large it is — without logging.
 *
 * stat() for the same reason as unlink() above.
 */
bool statQuietly(const char* path, uint32_t& sizeOut) {
  char full[80];
  snprintf(full, sizeof(full), "%s%s", kFsBase, path);
  struct stat st;
  if (::stat(full, &st) != 0) return false;
  sizeOut = static_cast<uint32_t>(st.st_size);
  return true;
}

/// Metadata is one small JSON-ish file rather than a sidecar per document:
/// five documents means five extra files and five extra directory entries for
/// what amounts to 40 bytes of data.
constexpr const char* kMetaPath = "/cache/meta.bin";

struct MetaEntry {
  uint32_t fetchedAt = 0;
  uint32_t ttl       = 0;
};

/// Identifies the metadata file, so a foreign or corrupt file is not read as
/// timestamps.
constexpr uint32_t kMetaMagic = 0x46425431;  // "FBT1"

/**
 * Cache schema version. **Bump this whenever a stored document's shape
 * changes** — which means whenever a deserialisation filter gains or loses a
 * field.
 *
 * Cached documents are filtered down to the fields the screens use, so a
 * filter change makes every existing document subtly wrong rather than
 * unreadable: it parses, and the new field is simply absent. That has now
 * happened twice — team ids added to the standings, then to the scorers — and
 * both times the symptom was a screen quietly showing nothing while a
 * perfectly valid-looking cache sat on disk until its TTL expired.
 *
 * Version 2: team ids retained in the standings and scorers documents.
 */
constexpr uint16_t kCacheSchemaVersion = 2;

/// Header written ahead of the entries.
struct MetaHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t entryCount;
};

/// In-RAM mirror of the metadata file. Cheap (40 bytes) and it means a
/// freshness check costs no filesystem access at all — which matters because
/// the screen rotation checks freshness far more often than data is fetched.
MetaEntry g_meta[static_cast<uint8_t>(Doc::Count)];

const char* docStem(Doc doc) {
  switch (doc) {
    case Doc::Standings:       return "standings";
    case Doc::TeamMatches:     return "team_matches";
    case Doc::OpponentMatches: return "opp_matches";
    case Doc::Scorers:         return "scorers";
    case Doc::LiveMatch:       return "live";
    default:                   return "unknown";
  }
}

/// Full path for a document, into a caller-supplied buffer.
void docPath(Doc doc, char* out, size_t len) {
  snprintf(out, len, "%s/%s.json", kCacheDir, docStem(doc));
}

void tempPath(Doc doc, char* out, size_t len) {
  snprintf(out, len, "%s/%s.tmp", kCacheDir, docStem(doc));
}

/// Current time as Unix seconds, or 0 if the clock has not been set.
///
/// Returning 0 rather than a bogus small number matters: a cache entry stamped
/// with a pre-NTP timestamp would appear absurdly stale (or absurdly fresh)
/// once the clock jumps, so callers treat 0 as "unknown" explicitly.
uint32_t nowOrZero() {
  const time_t t = time(nullptr);
  return t > 1600000000L ? static_cast<uint32_t>(t) : 0;
}

/// Set when the stored cache was written by a different schema, so begin()
/// knows to discard the documents as well as the metadata.
bool g_schemaMismatch = false;

void loadMeta() {
  for (auto& e : g_meta) e = MetaEntry{};
  g_schemaMismatch = false;

  uint32_t metaSize = 0;
  if (!statQuietly(kMetaPath, metaSize)) return;  // Never written yet.

  File f = LittleFS.open(kMetaPath, "r");
  if (!f) return;

  MetaHeader header{};
  const bool sizeOk = (f.size() == sizeof(MetaHeader) + sizeof(g_meta));
  if (sizeOk) f.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));

  // A short or oversized file means a partial write or an older format;
  // discard it rather than trusting half of it. Losing freshness metadata
  // costs one refresh, so failing safe is cheap.
  if (!sizeOk || header.magic != kMetaMagic ||
      header.entryCount != static_cast<uint16_t>(Doc::Count)) {
    Serial.println(F("[store] cache metadata unrecognised; discarding cache"));
    g_schemaMismatch = true;
    f.close();
    return;
  }
  if (header.version != kCacheSchemaVersion) {
    Serial.printf("[store] cache schema %u, expected %u; discarding cache\n",
                  header.version, kCacheSchemaVersion);
    g_schemaMismatch = true;
    f.close();
    return;
  }

  f.read(reinterpret_cast<uint8_t*>(g_meta), sizeof(g_meta));
  f.close();
}

bool saveMeta() {
  // Written the same atomic way as documents: the metadata is what makes the
  // documents interpretable, so a torn write here would be worse than a torn
  // document.
  const char* tmp = "/cache/meta.tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;

  const MetaHeader header{kMetaMagic, kCacheSchemaVersion,
                          static_cast<uint16_t>(Doc::Count)};
  size_t written =
      f.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  written += f.write(reinterpret_cast<const uint8_t*>(g_meta), sizeof(g_meta));
  f.flush();
  f.close();
  if (written != sizeof(header) + sizeof(g_meta)) {
    LittleFS.remove(tmp);
    return false;
  }
  removeQuietly(kMetaPath);
  return LittleFS.rename(tmp, kMetaPath);
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool begin() {
  // The `true` formats on failure, which is right for a cache: a filesystem
  // that will not mount holds nothing we cannot re-fetch, and a device that
  // refuses to boot over it would be far worse.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    Serial.println(F("[store] LittleFS mount failed even after format"));
    Serial.println(F("[store] continuing without cache -- live fetches only"));
    g_ready = false;
    return false;
  }

  uint32_t ignored = 0;
  if (!statQuietly(kCacheDir, ignored)) LittleFS.mkdir(kCacheDir);
  loadMeta();
  g_ready = true;

  // Documents written under a different schema are discarded here rather than
  // left to expire. They would parse perfectly and simply lack whatever field
  // the new filter added, which is a far more confusing failure than an empty
  // cache: a screen shows nothing while the cache page reports it fresh.
  if (g_schemaMismatch) {
    clearAllDocs();
    g_schemaMismatch = false;
  }

  uint32_t used = 0, total = 0;
  filesystemUsage(used, total);
  Serial.printf("[store] LittleFS mounted: %lu of %lu KB used\n",
                (unsigned long)(used / 1024), (unsigned long)(total / 1024));
  return true;
}

bool ready() { return g_ready; }

void filesystemUsage(uint32_t& usedBytes, uint32_t& totalBytes) {
  usedBytes  = LittleFS.usedBytes();
  totalBytes = LittleFS.totalBytes();
}

const char* docName(Doc doc) {
  switch (doc) {
    case Doc::Standings:       return "League table";
    case Doc::TeamMatches:     return "Our matches";
    case Doc::OpponentMatches: return "Opponent matches";
    case Doc::Scorers:         return "Top scorers";
    case Doc::LiveMatch:       return "Live match";
    default:                   return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

bool writeDoc(Doc doc, const char* json, size_t length, uint32_t ttlSeconds) {
  if (!g_ready || doc >= Doc::Count || json == nullptr) return false;

  char tmp[48], live[48];
  tempPath(doc, tmp, sizeof(tmp));
  docPath(doc, live, sizeof(live));

  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Serial.printf("[store] cannot open %s for writing\n", tmp);
    return false;
  }
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(json), length);
  f.flush();
  f.close();

  // A short write means the filesystem is full or failing. Removing the
  // temporary file leaves the previous good document in place, which is the
  // whole point of writing to a temporary in the first place.
  if (written != length) {
    Serial.printf("[store] short write to %s (%u of %u bytes)\n", tmp,
                  (unsigned)written, (unsigned)length);
    LittleFS.remove(tmp);
    return false;
  }

  // rename() will not overwrite, so the old file goes first. This is the one
  // instant at which no document exists — unavoidable, and harmless, because
  // the metadata is updated only after the rename succeeds, so a failure here
  // leaves the entry marked stale rather than falsely fresh.
  removeQuietly(live);
  if (!LittleFS.rename(tmp, live)) {
    Serial.printf("[store] rename %s -> %s failed\n", tmp, live);
    LittleFS.remove(tmp);
    return false;
  }

  MetaEntry& e = g_meta[static_cast<uint8_t>(doc)];
  e.fetchedAt  = nowOrZero();
  e.ttl        = ttlSeconds;
  saveMeta();

  Serial.printf("[store] wrote %s (%u bytes, ttl %lus)\n", docName(doc),
                (unsigned)length, (unsigned long)ttlSeconds);
  return true;
}

size_t readDoc(Doc doc, char* out, size_t capacity) {
  if (!g_ready || doc >= Doc::Count || out == nullptr || capacity == 0) {
    return 0;
  }

  char path[48];
  docPath(doc, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  const size_t size = f.size();
  // Refuse rather than truncate. A truncated JSON document does not fail
  // cleanly at the parser — it fails confusingly, and looks like corrupt data
  // from the API rather than a buffer that was too small.
  if (size + 1 > capacity) {
    Serial.printf("[store] %s is %u bytes, buffer holds %u -- refusing\n",
                  docName(doc), (unsigned)size, (unsigned)capacity);
    f.close();
    return 0;
  }

  const size_t read = f.readBytes(out, size);
  f.close();
  out[read] = '\0';
  return read;
}

DocStatus statusOf(Doc doc) {
  DocStatus st;
  if (!g_ready || doc >= Doc::Count) return st;

  char path[48];
  docPath(doc, path, sizeof(path));
  // stat() rather than opening the file: freshness is checked far more often
  // than data is fetched (the screen rotation asks constantly), so this path
  // should not open and close a file each time.
  if (!statQuietly(path, st.size)) return st;
  // A zero-length file is not a document. It should not happen given atomic
  // writes, but treating it as absent costs nothing and avoids handing the
  // parser an empty string.
  if (st.size == 0) return st;

  st.present = true;
  const MetaEntry& e = g_meta[static_cast<uint8_t>(doc)];
  st.fetchedAt = e.fetchedAt;
  st.ttl       = e.ttl;

  const uint32_t now = nowOrZero();
  if (now == 0 || e.fetchedAt == 0) {
    // Without a trustworthy clock, freshness cannot be judged. Reporting stale
    // means the document is still served (better than a blank screen) but a
    // refresh is attempted when possible — the safe direction to err in.
    st.fresh = false;
  } else {
    st.fresh = (now - e.fetchedAt) < e.ttl;
  }
  return st;
}

bool clearDoc(Doc doc) {
  if (!g_ready || doc >= Doc::Count) return false;
  char path[48];
  docPath(doc, path, sizeof(path));
  removeQuietly(path);
  g_meta[static_cast<uint8_t>(doc)] = MetaEntry{};
  return saveMeta();
}

bool clearAllDocs() {
  if (!g_ready) return false;
  for (uint8_t i = 0; i < static_cast<uint8_t>(Doc::Count); ++i) {
    char path[48];
    docPath(static_cast<Doc>(i), path, sizeof(path));
    removeQuietly(path);
    g_meta[i] = MetaEntry{};
  }
  Serial.println(F("[store] cache cleared"));
  return saveMeta();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void loadSettings(Settings& out) {
  Preferences p;
  // Read-only open: nothing is created if the namespace has never been
  // written, so a first boot leaves every default intact.
  //
  // On that first boot the ESP-IDF logs "nvs_open failed: NOT_FOUND" at ERROR
  // level from inside Preferences. That is expected and not a fault — there is
  // genuinely nothing saved yet — and it cannot be suppressed without hiding
  // real NVS errors too, so it is explained by the line logged immediately
  // afterwards instead.
  if (!p.begin(kNsSettings, /*readOnly=*/true)) {
    Serial.println(F("[store] no saved settings yet -- using defaults"));
    Serial.println(F("[store] (the NOT_FOUND above is expected on first boot)"));
    return;
  }

  // Only overwrite a field when NVS actually holds something for it.
  // getString() writes an empty string for a missing key, which would
  // otherwise wipe the compiled-in defaults (notably the API keys) on a device
  // that has saved Wi-Fi credentials but nothing else.
  //
  // Read into a temporary first. Preferences::getString(key, dest, len) writes
  // into dest *before* returning the length, so testing its return value is
  // too late — an empty stored value has already overwritten the default by
  // then. This is exactly how the API keys were silently blanked on a device
  // that had saved Wi-Fi credentials while the key fields were still empty.
  const auto loadStr = [&p](const char* key, char* dest, size_t len) {
    if (!p.isKey(key)) return;
    const String value = p.getString(key, "");
    if (value.length() == 0) return;
    strncpy(dest, value.c_str(), len - 1);
    dest[len - 1] = '\0';
  };
  loadStr("ssid", out.wifiSsid, sizeof(out.wifiSsid));
  loadStr("pass", out.wifiPass, sizeof(out.wifiPass));
  loadStr("apisKey", out.apiSportsKey, sizeof(out.apiSportsKey));
  loadStr("fdKey", out.footballDataKey, sizeof(out.footballDataKey));
  loadStr("comp", out.competitionCode, sizeof(out.competitionCode));
  loadStr("teamName", out.teamDisplayName, sizeof(out.teamDisplayName));
  loadStr("otaUrl", out.otaManifestUrl, sizeof(out.otaManifestUrl));

  out.apiSportsTeamId    = p.getUShort("apisTeam", out.apiSportsTeamId);
  out.footballDataTeamId = p.getUShort("fdTeam", out.footballDataTeamId);
  out.freeRefreshMinutes = p.getUShort("freeMin", out.freeRefreshMinutes);
  out.livePollSeconds    = p.getUShort("liveSec", out.livePollSeconds);
  out.screenDwellMs      = p.getULong("dwell", out.screenDwellMs);
  out.brightness         = p.getUChar("bright", out.brightness);
  out.autoBrightness     = p.getBool("autoBright", out.autoBrightness);
  out.soundEnabled       = p.getBool("sound", out.soundEnabled);
  out.screenMask         = p.getUChar("screens", out.screenMask);
  out.includeCups        = p.getBool("cups", out.includeCups);
  out.otaAutoCheck       = p.getBool("otaAuto", out.otaAutoCheck);

  // Calibration is stored as one blob rather than seven keys: it is only ever
  // read and written as a unit, and a partial calibration is meaningless.
  if (p.getBytesLength("touchCal") == sizeof(touch::Calibration)) {
    p.getBytes("touchCal", &out.touch, sizeof(out.touch));
  }

  p.end();
  Serial.printf("[store] settings loaded (wifi=%s, fd key=%s, cal=%s)\n",
                out.hasWifi() ? "set" : "unset",
                out.hasFootballDataKey() ? "set" : "unset",
                out.touch.valid ? "stored" : "default");
}

bool saveSettings(const Settings& in) {
  Preferences p;
  if (!p.begin(kNsSettings, /*readOnly=*/false)) {
    Serial.println(F("[store] cannot open settings for writing"));
    return false;
  }

  p.putString("ssid", in.wifiSsid);
  p.putString("pass", in.wifiPass);
  p.putString("apisKey", in.apiSportsKey);
  p.putString("fdKey", in.footballDataKey);
  p.putString("comp", in.competitionCode);
  p.putString("teamName", in.teamDisplayName);
  p.putUShort("apisTeam", in.apiSportsTeamId);
  p.putUShort("fdTeam", in.footballDataTeamId);
  p.putUShort("freeMin", in.freeRefreshMinutes);
  p.putUShort("liveSec", in.livePollSeconds);
  p.putULong("dwell", in.screenDwellMs);
  p.putUChar("bright", in.brightness);
  p.putBool("autoBright", in.autoBrightness);
  p.putBool("sound", in.soundEnabled);
  p.putUChar("screens", in.screenMask);
  p.putBool("cups", in.includeCups);
  p.putString("otaUrl", in.otaManifestUrl);
  p.putBool("otaAuto", in.otaAutoCheck);
  p.putBytes("touchCal", &in.touch, sizeof(in.touch));
  p.end();

  Serial.println(F("[store] settings saved"));
  return true;
}

bool factoryReset() {
  Preferences p;
  bool ok = true;
  if (p.begin(kNsSettings, false)) {
    ok &= p.clear();
    p.end();
  }
  if (p.begin(kNsQuota, false)) {
    ok &= p.clear();
    p.end();
  }
  // The cache goes too. It is keyed to a team and competition that are no
  // longer configured, so keeping it would only risk showing the previous
  // team's data after reconfiguration.
  if (g_ready) clearAllDocs();
  Serial.println(F("[store] factory reset"));
  return ok;
}

// ---------------------------------------------------------------------------
// Quota
// ---------------------------------------------------------------------------

namespace {
/// Key for the in-flight marker. Kept in the quota namespace rather than
/// settings so that clearing settings does not clear it, and vice versa.
constexpr const char* kOtaInFlight = "otaBusy";
}  // namespace

bool otaCheckWasInterrupted() {
  Preferences p;
  if (!p.begin(kNsQuota, /*readOnly=*/true)) return false;
  const bool busy = p.getBool(kOtaInFlight, false);
  p.end();
  return busy;
}

void markOtaCheckStarted() {
  Preferences p;
  if (!p.begin(kNsQuota, /*readOnly=*/false)) return;
  p.putBool(kOtaInFlight, true);
  p.end();
}

void markOtaCheckFinished() {
  Preferences p;
  if (!p.begin(kNsQuota, /*readOnly=*/false)) return;
  // Removed rather than set false, so the key does not linger once the
  // situation it describes is over.
  p.remove(kOtaInFlight);
  p.end();
}

void loadQuota(Quota& out) {
  Preferences p;
  if (!p.begin(kNsQuota, /*readOnly=*/true)) return;
  out.used      = p.getUShort("used", out.used);
  out.limit     = p.getUShort("limit", out.limit);
  out.dayNumber = p.getULong("day", out.dayNumber);
  p.end();

  // Roll over if the stored day is not today. Comparing day numbers rather
  // than doing date arithmetic keeps the midnight-UTC reset trivial — and
  // because the API's reset is also midnight UTC, the two agree by
  // construction rather than by coincidence.
  const uint32_t now = nowOrZero();
  if (now != 0) {
    const uint32_t today = now / 86400UL;
    if (today != out.dayNumber) {
      Serial.printf("[store] quota day rolled (%lu -> %lu), resetting tally\n",
                    (unsigned long)out.dayNumber, (unsigned long)today);
      out.used      = 0;
      out.dayNumber = today;
      saveQuota(out);
    }
  }
}

bool saveQuota(const Quota& in) {
  Preferences p;
  if (!p.begin(kNsQuota, /*readOnly=*/false)) return false;
  p.putUShort("used", in.used);
  p.putUShort("limit", in.limit);
  p.putULong("day", in.dayNumber);
  p.end();
  return true;
}

}  // namespace store
