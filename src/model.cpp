/**
 * @file model.cpp
 * @brief Placeholder data, taken verbatim from the live APIs.
 */

#include "model.h"

#include <Arduino.h>
#include <string.h>

namespace model {
namespace {

/// Copy into a fixed-size field, always terminated.
template <size_t N>
void setField(char (&dest)[N], const char* src) {
  strncpy(dest, src, N - 1);
  dest[N - 1] = '\0';
}

/**
 * The Championship as it actually stood at matchday 6, from
 * football-data.org's /competitions/ELC/standings.
 *
 * Real data on purpose. It gives us the genuine longest club name
 * ("Wolverhampton Wanderers FC"), a two-digit-goals spread, negative goal
 * differences, and Bolton down in 22nd — which is the case that matters most,
 * because a table screen that opens at the top would show a Bolton fan nothing
 * they care about.
 */
struct SeedRow {
  const char* tla;
  const char* name;
  uint8_t     mp, w, d, l, gf, ga;
  int16_t     gd;
  uint8_t     pts;
};

constexpr SeedRow kSeedTable[] = {
    {"WHU", "West Ham United FC",        6, 3, 2, 1, 14,  9,  5, 11},
    {"SWA", "Swansea City AFC",          6, 3, 2, 1,  8,  4,  4, 11},
    {"MID", "Middlesbrough FC",          5, 3, 1, 1,  8,  5,  3, 10},
    {"QPR", "Queens Park Rangers FC",    5, 3, 1, 1,  7,  4,  3, 10},
    {"BRI", "Bristol City FC",           5, 3, 1, 1,  9,  7,  2, 10},
    {"WBA", "West Bromwich Albion FC",   5, 3, 1, 1,  8,  6,  2, 10},
    {"CHA", "Charlton Athletic FC",      5, 3, 1, 1,  6,  7, -1, 10},
    {"MIL", "Millwall FC",               5, 3, 0, 2, 10,  8,  2,  9},
    {"SHE", "Sheffield United FC",       6, 2, 3, 1,  8,  8,  0,  9},
    {"WOL", "Wolverhampton Wanderers FC",5, 2, 2, 1, 13, 10,  3,  8},
    {"WAT", "Watford FC",                6, 2, 2, 2,  6,  7, -1,  8},
    {"SOU", "Southampton FC",            6, 3, 2, 1, 14,  7,  7,  7},
    {"WRE", "Wrexham AFC",               6, 1, 4, 1,  7,  5,  2,  7},
    {"BIR", "Birmingham City FC",        5, 1, 4, 0,  7,  6,  1,  7},
    {"STK", "Stoke City FC",             6, 2, 1, 3,  9, 10, -1,  7},
    {"NOR", "Norwich City FC",           5, 2, 0, 3,  8,  8,  0,  6},
    {"POR", "Portsmouth FC",             5, 2, 0, 3,  7,  8, -1,  6},
    {"BLA", "Blackburn Rovers FC",       6, 1, 2, 3,  7,  9, -2,  5},
    {"LIN", "Lincoln City FC",           5, 1, 2, 2,  4,  6, -2,  5},
    {"CAR", "Cardiff City FC",           6, 0, 4, 2,  7, 10, -3,  4},
    {"DER", "Derby County FC",           5, 1, 1, 3,  5, 10, -5,  4},
    {"BOL", "Bolton Wanderers FC",       6, 1, 1, 4,  6, 12, -6,  4},
    {"BUR", "Burnley FC",                6, 0, 3, 3,  7, 13, -6,  3},
    {"PNE", "Preston North End FC",      6, 1, 0, 5,  6, 12, -6,  3},
};

/// Our team, as football-data.org spells it.
constexpr const char* kOurTla = "BOL";

/**
 * Whether to simulate a match in progress.
 *
 * True while the live-match UI is being developed, since a real live match
 * involving one specific club cannot be summoned to order. Set to **false**
 * once the API client supplies genuine live data.
 */
constexpr bool kSimulateLiveMatch = false;

/// Top scorers from /competitions/ELC/scorers. `assists` is null on the free
/// tier, so it is absent from the model entirely rather than shown as zero.
///
/// Seeded through a literal-friendly struct because Scorer holds fixed-size
/// char arrays, which cannot be brace-initialised from string literals.
struct SeedScorer {
  const char* name;
  const char* tla;
  uint8_t     goals;
  uint8_t     played;
};

constexpr SeedScorer kSeedLeagueScorers[] = {
    {"William Lankshear", "MID", 5, 5},
    {"Cyle Larin",        "SOU", 5, 6},
    {"Ethan Galbraith",   "STK", 4, 6},
};

/// Bolton's scorers, filtered from the same /scorers?limit=100 response.
/// Real values: nobody at the foot of the table reaches the league top ten,
/// which is exactly why the wider request is needed.
constexpr SeedScorer kSeedTeamScorers[] = {
    {"Sam Dalby",      "BOL", 2, 6},
    {"Thierry Gale",   "BOL", 1, 6},
    {"Xavier Simons",  "BOL", 1, 3},
};

}  // namespace

void loadPlaceholder(Snapshot& out) {
  // Now only a first-boot stand-in: it fills the screens for the few seconds
  // between power-on and the first successful fetch, so the device never shows
  // an empty carousel. Everything here is overwritten by real data.

  out = Snapshot{};
  setField(out.competitionName, "Championship");
  out.matchday = 6;

  // --- League table -------------------------------------------------------
  constexpr uint8_t kSeedRowCount = sizeof(kSeedTable) / sizeof(kSeedTable[0]);
  static_assert(kSeedRowCount <= Snapshot::kMaxTableRows,
                "seed table exceeds Snapshot capacity");
  const uint8_t rows = kSeedRowCount;
  for (uint8_t i = 0; i < rows; ++i) {
    const SeedRow& src = kSeedTable[i];
    TableRow& dst = out.table[i];
    setField(dst.tla, src.tla);
    setField(dst.name, src.name);
    dst.position       = i + 1;
    dst.played         = src.mp;
    dst.won            = src.w;
    dst.drawn          = src.d;
    dst.lost           = src.l;
    dst.goalsFor       = src.gf;
    dst.goalsAgainst   = src.ga;
    dst.goalDifference = src.gd;
    dst.points         = src.pts;
    dst.isOurTeam      = (strcmp(src.tla, kOurTla) == 0);
    if (dst.isOurTeam) out.ourRow = i;
  }
  out.tableRows = rows;

  // --- Last result: Bolton 2-3 West Ham, 8 Sep 2026 -----------------------
  {
    Fixture& f = out.lastResult;
    setField(f.homeTla, "BOL");
    setField(f.awayTla, "WHU");
    setField(f.homeName, "Bolton Wanderers FC");
    setField(f.awayName, "West Ham United FC");
    setField(f.competition, "Championship");
    f.kickoffUtc = 1788894000UL;  // 2026-09-08T19:00:00Z
    f.homeGoals  = 2;
    f.awayGoals  = 3;
    f.state      = MatchState::Finished;
    f.matchday   = 6;
    f.weAreHome  = true;
    f.valid      = true;
    setField(f.homeForm, "DLLLL");
    setField(f.awayForm, "WWDWD");
  }

  // --- Next fixture: Bolton v Cardiff, 12 Sep 2026 ------------------------
  {
    Fixture& f = out.nextFixture;
    setField(f.homeTla, "BOL");
    setField(f.awayTla, "CAR");
    setField(f.homeName, "Bolton Wanderers FC");
    setField(f.awayName, "Cardiff City FC");
    setField(f.competition, "Championship");
    f.kickoffUtc = 1789212600UL;  // 2026-09-12T11:30:00Z, from the API
    f.state      = MatchState::Scheduled;
    f.matchday   = 7;
    f.weAreHome  = true;
    f.valid      = true;
    // Derived from each club's finished matches, oldest first. Bolton took a
    // win on matchday 1 then D,L,L,L,L; Cardiff D,D,D,L,L,D. Both tally with
    // their table rows, which is the check that the derivation is right.
    setField(f.homeForm, "DLLLL");
    setField(f.awayForm, "DDLLD");
  }

  // --- Live match ---------------------------------------------------------
  //
  // Simulated, and gated by kSimulateLiveMatch, because a live match cannot be
  // conjured on demand for testing and the behaviour around one is the most
  // intricate in the UI: the priority lock-back, the event columns, and the
  // provisional form chip all only appear while a match is on.
  //
  // Player names are the clubs' real scorers from the live API, so name
  // lengths are honest. **Set kSimulateLiveMatch to false once the API client
  // supplies real live data.**
  if (kSimulateLiveMatch) {
    LiveMatch& m = out.live;
    Fixture& f = m.fixture;
    setField(f.homeTla, "BOL");
    setField(f.awayTla, "CAR");
    setField(f.homeName, "Bolton Wanderers FC");
    setField(f.awayName, "Cardiff City FC");
    setField(f.competition, "Championship");
    f.kickoffUtc = 1789212600UL;
    f.homeGoals  = 2;
    f.awayGoals  = 1;
    f.state      = MatchState::InPlay;
    f.matchday   = 7;
    f.weAreHome  = true;
    f.valid      = true;
    setField(f.homeForm, "DLLLL");
    setField(f.awayForm, "DDLLD");

    m.minute = 78;
    m.extra  = 0;

    // Chronological, as the API returns them. Deliberately mixed so every
    // marker the renderer can draw is exercised: goal, penalty, both card
    // colours, and both columns populated.
    struct SeedEvent {
      uint8_t     minute;
      EventKind   kind;
      bool        home;
      const char* player;
    };
    constexpr SeedEvent kSeedEvents[] = {
        {23, EventKind::Goal,       false, "Cian Ashford"},
        {38, EventKind::YellowCard, false, "Perry Ng"},
        {52, EventKind::Goal,       true,  "Sam Dalby"},
        {61, EventKind::YellowCard, true,  "Xavier Simons"},
        {70, EventKind::Penalty,    true,  "Thierry Gale"},
        {76, EventKind::RedCard,    false, "Rubin Colwill"},
    };
    constexpr uint8_t kEventCount =
        sizeof(kSeedEvents) / sizeof(kSeedEvents[0]);
    static_assert(kEventCount <= LiveMatch::kMaxEvents,
                  "seed events exceed LiveMatch capacity");

    for (uint8_t i = 0; i < kEventCount; ++i) {
      MatchEvent& e = m.events[i];
      e.minute = kSeedEvents[i].minute;
      e.extra  = 0;
      e.kind   = kSeedEvents[i].kind;
      e.home   = kSeedEvents[i].home;
      setField(e.player, kSeedEvents[i].player);
    }
    m.eventCount = kEventCount;

    // Provisional result from our point of view, as things stand.
    const int8_t ours   = f.weAreHome ? f.homeGoals : f.awayGoals;
    const int8_t theirs = f.weAreHome ? f.awayGoals : f.homeGoals;
    m.provisionalResult = ours > theirs ? 'W' : (ours < theirs ? 'L' : 'D');

    out.liveActive = true;

    // With a match in progress, Next must show the *following* fixture, not
    // the one being played — otherwise two screens show the same match and
    // the genuinely useful information is lost. Real data: Norwich v Bolton,
    // matchday 8, with Norwich's real derived form.
    Fixture& n = out.nextFixture;
    setField(n.homeTla, "NOR");
    setField(n.awayTla, "BOL");
    setField(n.homeName, "Norwich City FC");
    setField(n.awayName, "Bolton Wanderers FC");
    setField(n.competition, "Championship");
    n.kickoffUtc = 1789907400UL;  // 2026-09-20T12:30:00Z
    n.homeGoals  = -1;
    n.awayGoals  = -1;
    n.state      = MatchState::Scheduled;
    n.matchday   = 8;
    n.weAreHome  = false;
    n.valid      = true;
    setField(n.homeForm, "LWLWW");  // Norwich, derived from their results.
    setField(n.awayForm, "DLLLL");  // Bolton's last five completed.
  }

  // --- Top scorers --------------------------------------------------------
  const auto copyScorers = [](const SeedScorer* src, uint8_t n, Scorer* dst) {
    for (uint8_t i = 0; i < n; ++i) {
      setField(dst[i].name, src[i].name);
      setField(dst[i].tla, src[i].tla);
      dst[i].goals  = src[i].goals;
      dst[i].played = src[i].played;
    }
  };

  constexpr uint8_t kLeagueCount =
      sizeof(kSeedLeagueScorers) / sizeof(kSeedLeagueScorers[0]);
  constexpr uint8_t kTeamCount =
      sizeof(kSeedTeamScorers) / sizeof(kSeedTeamScorers[0]);
  static_assert(kLeagueCount <= Snapshot::kMaxScorers,
                "seed league scorers exceed Snapshot capacity");
  static_assert(kTeamCount <= Snapshot::kMaxScorers,
                "seed team scorers exceed Snapshot capacity");

  copyScorers(kSeedLeagueScorers, kLeagueCount, out.leagueScorers);
  out.leagueScorerCount = kLeagueCount;
  copyScorers(kSeedTeamScorers, kTeamCount, out.teamScorers);
  out.teamScorerCount = kTeamCount;
}

}  // namespace model
