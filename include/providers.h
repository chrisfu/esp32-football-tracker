/**
 * @file providers.h
 * @brief Fetch and parse, per data source.
 *
 * The layer that keeps the provider split invisible to everything above it
 * (SPEC.md §6). Screens see only a `model::Snapshot`; nothing outside this
 * file knows that the league table comes from football-data.org while the live
 * match comes from api-sports.
 *
 * Each function fetches, parses into the model, and writes the filtered result
 * to the cache. A failure leaves the previous data untouched — stale data
 * being far better than an empty screen.
 */

#pragma once

#include "api_client.h"
#include "model.h"
#include "store.h"

namespace providers {

/// Configure from settings. Keys and ids are read on every call, so changing
/// them at runtime takes effect without a restart.
void begin(const store::Settings& settings);

/// League table, and with it our season record and league position.
api::Result fetchStandings(model::Snapshot& out);

/**
 * Our team's matches: the last result, and our form.
 *
 * One request serves both, plus the form guide — `limit` selects the most
 * recent N and returns them oldest-first, so the last result is the final
 * element and the form is the tail of the same list.
 */
api::Result fetchOurMatches(model::Snapshot& out);

/// The next scheduled fixture, skipping any match currently in progress.
api::Result fetchNextFixture(model::Snapshot& out);

/// The next opponent's form. Only worth calling when the opponent changes.
api::Result fetchOpponentForm(model::Snapshot& out);

/// Top scorers: our team's, and the league leaders, from one request.
api::Result fetchScorers(model::Snapshot& out);

/**
 * Any match involving our team that is in progress.
 *
 * Clears `liveActive` when there is none, which is what returns the screen
 * rotation to normal — the manager watches that flag, never a clock, because
 * a result can still change in stoppage time.
 */
api::Result fetchLiveMatch(model::Snapshot& out);

/// Parse an ISO-8601 UTC timestamp ("2026-09-12T11:30:00Z") to Unix seconds.
/// Returns 0 if it cannot be parsed.
uint32_t parseIso8601(const char* iso);

}  // namespace providers
