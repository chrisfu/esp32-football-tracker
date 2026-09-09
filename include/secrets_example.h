/**
 * @file secrets_example.h
 * @brief Template for build-time API key provisioning.
 *
 * Copy to `include/secrets.h` and fill in. That file is listed in .gitignore,
 * so keys never reach the repository.
 *
 * This is a convenience, not the primary path: keys can always be entered
 * through the web interface and are stored in NVS. It exists so a freshly
 * flashed device is useful immediately rather than requiring a trip to the
 * settings page before it can show anything.
 *
 * Values here are only ever used as *defaults* for a device with nothing
 * saved. Anything entered through the web interface takes precedence and
 * survives reflashing.
 */

#pragma once

// football-data.org — https://www.football-data.org/client/register
#define SECRET_FOOTBALL_DATA_KEY ""

// api-sports.io — https://dashboard.api-football.com
#define SECRET_API_SPORTS_KEY ""
