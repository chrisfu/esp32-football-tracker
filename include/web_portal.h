/**
 * @file web_portal.h
 * @brief The on-device web interface.
 *
 * Serves the setup portal in AP mode and the configuration UI in station
 * mode. Uses the synchronous `WebServer` rather than an async one: this is a
 * low-traffic configuration surface, and async would add flash and a second
 * task's stack for no benefit at this scale.
 */

#pragma once

#include "model.h"
#include "store.h"

namespace web {

/// Bring up the HTTP server and register routes.
/// @param captive also answer every unknown host, for the AP setup portal.
void begin(store::Settings& settings, const model::Snapshot& data,
           bool captive);

/// Service pending requests. Call from the main loop.
void tick();

/// True if settings were changed via the web UI and need applying.
bool settingsDirty();

/// Clear the dirty flag once the caller has applied the new settings.
void clearSettingsDirty();

/// True once the user has submitted Wi-Fi credentials, so the caller can
/// reboot into station mode.
bool credentialsSubmitted();

/// Something the web UI is asking the caller to do.
///
/// Returned rather than performed here, so the web layer has no power to
/// reboot or wipe the device by itself — the same separation the on-device
/// settings menu uses.
enum class Action : uint8_t {
  None,
  RefreshNow,    ///< Treat all data as stale and re-fetch.
  ResetWifi,     ///< Clear credentials and restart into setup.
  FactoryReset,  ///< Erase everything and restart.
};

/// Take any pending action, clearing it.
Action takeAction();

}  // namespace web
