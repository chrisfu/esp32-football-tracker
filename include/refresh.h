/**
 * @file refresh.h
 * @brief Decides what to fetch and when, and keeps it off the UI's core.
 *
 * A TLS handshake plus a parse takes seconds. Doing that in the main loop
 * would freeze the display and swallow touches for the duration, so fetching
 * runs as its own task pinned to core 0 while the UI keeps core 1.
 *
 * Handoff between them is deliberately lock-free. The task fills a staging
 * snapshot that only it touches, then raises a flag; the UI copies that into
 * the live snapshot between frames and lowers the flag; the task will not
 * begin another fetch until it is lowered. The flag *is* the ownership
 * transfer, so neither side ever needs a mutex, and no draw can ever observe a
 * half-updated table.
 */

#pragma once

#include "model.h"
#include "ota.h"
#include "store.h"

namespace refresh {

/// Start the background fetch task.
void begin(const store::Settings& settings, const model::Snapshot& initial);

/**
 * Adopt any completed fetch. Call from the main loop.
 * @param live the snapshot the UI draws from.
 * @return true if new data was adopted, so the caller should redraw.
 */
bool adopt(model::Snapshot& live);

/// Force everything to be treated as stale, e.g. after a settings change.
void invalidateAll();

/// Seconds until the next scheduled fetch, for the web dashboard.
uint32_t secondsToNextFetch();

/// Whether the first full load has completed.
bool primed();

/**
 * Ask the fetch task to check for a firmware update, and apply it if one is
 * found.
 *
 * Runs there rather than in a web handler because it downloads 1.2 MB over
 * TLS — which would hold the HTTP connection open for minutes and block the
 * UI besides.
 */
void requestUpdateCheck(bool applyIfFound);

/// The most recent update check's result.
const ota::UpdateInfo& updateInfo();

/// True while a check or download is in progress.
bool updateInProgress();

}  // namespace refresh
