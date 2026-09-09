/**
 * @file model.h
 * @brief The data the screens render, and nothing about where it came from.
 *
 * This is the boundary that keeps the provider abstraction honest (SPEC.md §6):
 * screens consume these structs, so they never learn whether a value arrived
 * from api-sports, football-data.org, a cached file, or a placeholder.
 *
 * Every field is sized for the job rather than for convenience, per rule R3.
 * A 46-game season caps points at 138 and goals comfortably under 200, so
 * uint8_t is genuinely sufficient — and with 24 table rows resident, the
 * difference between this and a naive layout of ints and String objects is
 * kilobytes we do not have to spare.
 */

#pragma once

#include <stdint.h>

namespace model {

/// Fixed-size strings throughout: no heap, no fragmentation, predictable cost.
/// Lengths come from the longest real values seen in the live API responses
/// ("Wolverhampton Wanderers FC" is 26 characters, hence 28).
constexpr uint8_t kTlaLen  = 4;   ///< "BOL" + terminator.
constexpr uint8_t kNameLen = 28;
constexpr uint8_t kCompLen = 20;

/// One row of the league table. Mirrors the columns the brief asks for.
struct TableRow {
  char    tla[kTlaLen]   = {0};
  char    name[kNameLen] = {0};
  uint8_t position       = 0;
  uint8_t played         = 0;
  uint8_t won            = 0;
  uint8_t drawn          = 0;
  uint8_t lost           = 0;
  uint8_t goalsFor       = 0;
  uint8_t goalsAgainst   = 0;
  int16_t goalDifference = 0;
  uint8_t points         = 0;
  /// Highlighted, and the table opens centred on it.
  bool    isOurTeam      = false;
};

/// Match status, kept as an enum rather than the provider's status string.
/// Note football-data.org reports upcoming matches as TIMED, not SCHEDULED —
/// mapping to our own enum at the provider boundary stops that leaking here.
enum class MatchState : uint8_t {
  Unknown,
  Scheduled,
  InPlay,
  Paused,    ///< Half time.
  Finished,
  Postponed,
};

/// A fixture, past, present or future.
struct Fixture {
  char       homeTla[kTlaLen]   = {0};
  char       awayTla[kTlaLen]   = {0};
  char       homeName[kNameLen] = {0};
  char       awayName[kNameLen] = {0};
  char       competition[kCompLen] = {0};
  /// Unix seconds, UTC. 32 bits is good until 2106.
  uint32_t   kickoffUtc = 0;
  /// -1 means "no score yet" rather than 0-0, which is a real scoreline.
  int8_t     homeGoals  = -1;
  int8_t     awayGoals  = -1;
  MatchState state      = MatchState::Unknown;
  uint8_t    matchday   = 0;
  bool       weAreHome  = false;
  bool       valid      = false;
};

/// A live match in progress. Extends a fixture with in-play detail.
struct LiveMatch {
  Fixture fixture;
  /// Minutes elapsed, as reported by the provider.
  uint8_t minute = 0;
};

/// One entry in the top-scorer chart. `assists` is absent on the free tier.
struct Scorer {
  char    name[kNameLen] = {0};
  char    tla[kTlaLen]   = {0};
  uint8_t goals          = 0;
  uint8_t played         = 0;
};

/// Everything the screens can display, in one place.
///
/// Held as a single struct so a screen's data source is a const reference,
/// which makes it impossible for a screen to mutate shared state while
/// drawing. The refresh scheduler owns the only mutable path.
struct Snapshot {
  static constexpr uint8_t kMaxTableRows = 24;  ///< Championship size.
  static constexpr uint8_t kMaxScorers   = 5;

  TableRow table[kMaxTableRows];
  uint8_t  tableRows = 0;
  /// Index into `table` of our team, or 0xFF if absent.
  uint8_t  ourRow = 0xFF;

  Fixture   lastResult;
  Fixture   nextFixture;
  LiveMatch live;
  bool      liveActive = false;

  Scorer  scorers[kMaxScorers];
  uint8_t scorerCount = 0;

  char competitionName[kCompLen] = {0};
  /// Season's current matchday, for context on the table screen.
  uint8_t matchday = 0;

  /// Convenience: our team's table row, or nullptr if we do not have it.
  const TableRow* ourTeam() const {
    return ourRow < tableRows ? &table[ourRow] : nullptr;
  }
};

/**
 * Fill a snapshot with the real data captured from the live APIs during
 * development.
 *
 * Deliberately real rather than invented: actual club names and scorelines
 * exercise the true string lengths and column widths, so layout problems show
 * up now instead of on the first live fetch. This is replaced by the cache
 * layer and disappears once providers exist.
 */
void loadPlaceholder(Snapshot& out);

}  // namespace model
