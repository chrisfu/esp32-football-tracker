/**
 * @file ota.h
 * @brief Firmware updates: by web upload, and by pulling a published release.
 *
 * Two paths, because they fail in different circumstances:
 *
 *   - **Web upload.** The user posts a `.bin` from a browser. Works with no
 *     internet at all, which matters when the device is on an isolated network
 *     or the release host is unreachable.
 *   - **Pull.** The device reads a small JSON manifest, compares SemVer, and
 *     downloads the binary itself. This is the path that makes releases
 *     actually reach a device sitting on a shelf.
 *
 * **The downloaded image is hashed as it is written and the hash checked
 * before the update is committed.** ESP-IDF only switches the boot partition
 * when `Update.end()` succeeds, so aborting on a mismatch leaves the running
 * firmware untouched — which is why no staging space is needed for a 1.2 MB
 * image on a 1.4 MB filesystem.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ota {

/// What a manifest describes.
struct UpdateInfo {
  char     version[16] = {0};
  char     url[192]    = {0};
  char     sha256[65]  = {0};  ///< Hex, lowercase. 64 chars + terminator.
  uint32_t size        = 0;
  /// True when `version` is strictly newer than what is running.
  bool     available   = false;
};

/// Progress reporting, 0-100. Called from the fetch task, not the UI.
using ProgressFn = void (*)(uint8_t percent);

/// The running firmware's SemVer, e.g. "0.1.0".
const char* currentVersion();

/// The running firmware's full build description, e.g. "0.1.0+4.gab12cd".
const char* currentBuild();

/**
 * Compare two SemVer strings.
 *
 * @return >0 if `a` is newer, <0 if older, 0 if equal.
 *
 * Only the major.minor.patch triple is compared. Build metadata after `+` is
 * ignored, which is what SemVer requires and what we want: an untagged local
 * build must never appear newer than the release it follows, or the device
 * would refuse a genuine update.
 */
int compareVersions(const char* a, const char* b);

/**
 * Read the manifest and decide whether an update is available.
 *
 * Costs no API quota — the release host is unrelated to the football APIs.
 * @return false on any network or parse failure, leaving `out.available` false.
 */
bool checkForUpdate(const char* manifestUrl, UpdateInfo& out);

/**
 * Download, verify and apply an update. **Reboots on success.**
 * @return false on failure, with the running firmware untouched.
 */
bool applyUpdate(const UpdateInfo& info, ProgressFn progress);

// --- Web upload ------------------------------------------------------------
// Driven by the web server's upload handler, which delivers the body in
// chunks, so this is a small state machine rather than one call.

bool uploadBegin(size_t expectedSize);
bool uploadWrite(const uint8_t* data, size_t length);
bool uploadEnd();
void uploadAbort();

/// Last failure, for reporting back to the browser. Never null.
const char* lastError();

}  // namespace ota
