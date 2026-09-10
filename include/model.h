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

/**
 * Simulate a match in progress, for developing the live screen.
 *
 * Defined here rather than in model.cpp because refresh.cpp needs it too —
 * it has to skip the real live fetch, which would otherwise overwrite the
 * simulated fixture. Having it in one .cpp meant the project compiled only
 * when the flag happened to be passed on the command line, which is how a
 * broken default build went unnoticed through two rounds of testing.
 *
 *   PLATFORMIO_BUILD_FLAGS="-DSIMULATE_LIVE_MATCH=1" pio run --target upload
 */
#ifndef SIMULATE_LIVE_MATCH
#define SIMULATE_LIVE_MATCH 0
#endif

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
  /**
   * football-data team id.
   *
   * Retained so the web interface can list every club in the competition
   * alongside its id — which is the whole answer to "how do I find the id of
   * the team I want to track", at least for a club in the league already
   * being followed. Two bytes per row, 48 for a full table.
   */
  uint16_t id            = 0;
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

/// Recent form: five results, **oldest first**, so it reads left to right in
/// chronological order. Five characters plus a terminator.
constexpr uint8_t kFormLen = 6;

/// A fixture, past, present or future.
struct Fixture {
  char       homeTla[kTlaLen]   = {0};
  char       awayTla[kTlaLen]   = {0};
  char       homeName[kNameLen] = {0};
  char       awayName[kNameLen] = {0};
  char       competition[kCompLen] = {0};
  /**
   * Provider team ids, retained rather than discarded after matching.
   *
   * Needed for two things that were previously stubbed out: fetching the
   * opponent's form, and knowing which crest to download. These are
   * football-data ids specifically — api-sports numbers teams differently.
   */
  uint16_t   homeId = 0;
  uint16_t   awayId = 0;
  /// Unix seconds, UTC. 32 bits is good until 2106.
  uint32_t   kickoffUtc = 0;
  /// -1 means "no score yet" rather than 0-0, which is a real scoreline.
  int8_t     homeGoals  = -1;
  int8_t     awayGoals  = -1;
  MatchState state      = MatchState::Unknown;
  uint8_t    matchday   = 0;
  bool       weAreHome  = false;
  bool       valid      = false;

  /**
   * Each club's recent form, as 'W'/'D'/'L' characters, oldest first.
   *
   * Derived on-device rather than read from the API: football-data.org's
   * standings response carries a `form` field, but it is **null for every team
   * on the free tier**. It is computed instead from the finished-matches
   * response — which for our own team costs nothing, being the same request
   * that supplies the last result. The opponent's costs one additional
   * request, and only when the fixture changes.
   *
   * Empty when unknown, which the renderer shows as blank rather than guessing.
   */
  char homeForm[kFormLen] = {0};
  char awayForm[kFormLen] = {0};
};

/**
 * A notable thing that happened in a match.
 *
 * Only goals and cards are kept. Substitutions are available from the API but
 * deliberately excluded: on a 320x240 panel screen space is the scarce
 * resource, and a substitution tells a casual viewer far less than a goal or a
 * card. Recorded oldest-first, as the API returns them.
 */
enum class EventKind : uint8_t {
  Goal,
  Penalty,     ///< Scored from the spot; worth distinguishing.
  /**
   * A penalty that was *not* scored.
   *
   * The API reports this as type "Goal" with detail "Missed Penalty", which is
   * a trap: matching "Penalty" anywhere in the detail classifies a miss as a
   * scored penalty, and the screen then shows a goal that never happened while
   * the scoreline says otherwise. Confirmed present in live data — two of
   * them across 29 fixtures.
   */
  MissedPenalty,
  OwnGoal,     ///< Counts for the *other* side, so the side shown is flipped.
  YellowCard,
  RedCard,
  Other,       ///< Recognised but not specially rendered.
};

struct MatchEvent {
  uint8_t   minute = 0;
  /// Added time, e.g. 90+3 stores minute 90 and extra 3. Zero when none.
  uint8_t   extra  = 0;
  EventKind kind   = EventKind::Other;
  /// Which column this belongs under. For an own goal this is the side that
  /// *benefits*, not the side that scored it, because that is where a reader
  /// looks for the goal that changed the score.
  bool      home   = false;
  char      player[20] = {0};
};

/// A live match in progress. Extends a fixture with in-play detail.
struct LiveMatch {
  static constexpr uint8_t kMaxEvents = 16;

  Fixture fixture;
  /// Minutes elapsed, as reported by the provider.
  uint8_t minute = 0;
  /// Added time being played, 0 if none.
  uint8_t extra  = 0;

  MatchEvent events[kMaxEvents];
  uint8_t    eventCount = 0;

  /**
   * Our provisional result if the match ended right now: 'W', 'D', 'L'.
   *
   * Shown as an extra, visually distinct chip on the form guide. Provisional
   * on purpose — it must not be mistaken for a settled result, because it can
   * still change, including deep into stoppage time.
   */
  char provisionalResult = 0;
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

  /// Top scorers across the whole competition.
  Scorer  leagueScorers[kMaxScorers];
  uint8_t leagueScorerCount = 0;

  /**
   * Our own team's scorers.
   *
   * Obtained from the same single request as the league list: asking for
   * `?limit=100` returns everyone who has scored at all (the list bottoms out
   * at one goal), so our players are filtered out of that response locally.
   * One request serves both views — and our team's scorers would otherwise
   * never appear, since a side near the foot of the table has nobody in the
   * league top ten.
   */
  Scorer  teamScorers[kMaxScorers];
  uint8_t teamScorerCount = 0;

  /**
   * The opponent of a match in progress, as a football-data id.
   *
   * Captured when the fixture list identifies which upcoming match has gone
   * live, because the live feed comes from api-sports and its team ids are
   * from a different space entirely — there is no way to derive one from the
   * other, so the association has to be recorded when it is known.
   */
  uint16_t liveOpponentId = 0;

  char competitionName[kCompLen] = {0};
  /// Season's current matchday, for context on the table screen.
  uint8_t matchday = 0;

  /// Convenience: our team's table row, or nullptr if we do not have it.
  const TableRow* ourTeam() const {
    return ourRow < tableRows ? &table[ourRow] : nullptr;
  }

  /// The opponent in a fixture, from our point of view.
  static uint16_t opponentOf(const Fixture& f) {
    return f.weAreHome ? f.awayId : f.homeId;
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
