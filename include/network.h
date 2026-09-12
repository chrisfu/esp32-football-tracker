/**
 * @file network.h
 * @brief Wi-Fi connection and first-boot provisioning.
 *
 * Two modes, and the device chooses between them without being asked:
 *
 *   - **Station**: credentials exist in NVS, so join that network.
 *   - **Access point**: no credentials, joining failed, or the user asked for
 *     setup — so run a SoftAP with a captive portal and wait to be configured.
 *
 * The device must be usable by someone who has only the device: no serial
 * console, no companion app, no printed manual. That is why the display shows
 * the AP name and the portal address (§7 of SPEC.md) — the screen is right
 * there, so nothing has to be guessed.
 */

#pragma once

#include <stdint.h>

#include "store.h"

namespace net {

enum class Mode : uint8_t {
  Idle,          ///< begin() not yet called.
  Connecting,    ///< Joining a stored network.
  Connected,     ///< Station mode, associated, has an IP.
  AccessPoint,   ///< Serving the setup portal.
  Failed,        ///< Join failed and AP fallback also failed. Cache only.
};

/// Where things stand, for the UI and the web dashboard.
struct Status {
  Mode     mode    = Mode::Idle;
  char     ssid[33] = {0};
  char     ip[16]   = {0};
  int8_t   rssi     = 0;
  /// Signal quality as a percentage, which is friendlier than dBm on screen.
  uint8_t  quality  = 0;
  uint32_t connectedAt = 0;  ///< millis() when the link came up.
  bool     timeSynced  = false;
};

/**
 * Bring up networking.
 *
 * Attempts a station join if credentials exist; falls back to the setup AP if
 * that fails. Never blocks longer than the join timeout, because a device that
 * hangs on a dead network is worse than one that shows stale data.
 *
 * @param forceSetup start the AP regardless of stored credentials.
 */
void begin(const store::Settings& settings, bool forceSetup = false);

/// Pump background work: DNS for the captive portal, reconnection, NTP.
/// Must be called regularly from the main loop.
void tick();

const Status& status();

/// True when a station link is up and usable for fetching.
bool online();

/// Access point SSID, valid whenever mode is AccessPoint.
const char* apSsid();

/// The AP's password. Shown on screen, since there is nowhere else to find it.
const char* apPassword();

/// The URL to open once joined to the AP.
const char* portalUrl();

/**
 * The device's mDNS hostname, without the ".local" suffix.
 *
 * Unique per device: the base name with the last two bytes of the MAC, so two
 * trackers on one network do not fight over the same name. Valid before a join
 * too, where it is still the unsuffixed base.
 */
const char* hostname();

/**
 * The address of this device's own web interface, ready to show a human.
 *
 * In station mode this is "http://<hostname>.local". In access-point mode it
 * is the portal address instead, because .local does not resolve there — the
 * device is serving its own network and has no mDNS responder for clients to
 * query.
 *
 * Anything displaying the web address must use this rather than composing one,
 * which is what left the on-device settings advertising a fixed
 * "football.local" that stopped resolving once names became unique.
 */
const char* webUrl();

/**
 * Start an NTP sync.
 *
 * Time matters more here than it might seem: cache freshness, the fixture
 * countdown, and the midnight-UTC quota reset all depend on it. Until it
 * completes, the rest of the firmware deliberately says "unknown" rather than
 * guessing (see store::statusOf and ui::formatCountdown).
 */
void beginTimeSync();

}  // namespace net
