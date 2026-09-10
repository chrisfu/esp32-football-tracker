/**
 * @file providers.cpp
 * @brief Fetch and parse. See providers.h.
 */

#include "providers.h"

#include <Arduino.h>
#include <string.h>

namespace providers {
namespace {

const store::Settings* g_settings = nullptr;

/// Copy into a fixed-size field, always terminated.
template <size_t N>
void setField(char (&dest)[N], const char* src) {
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, N - 1);
  dest[N - 1] = '\0';
}

/// Days since 1970-01-01 for a civil date. Howard Hinnant's algorithm, which
/// avoids needing mktime and its timezone assumptions — we want pure UTC here.
int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

/// The competition we track, e.g. "ELC".
const char* competition() {
  return (g_settings != nullptr) ? g_settings->competitionCode : "ELC";
}

uint16_t footballDataTeam() {
  return (g_settings != nullptr) ? g_settings->footballDataTeamId : 60;
}

uint16_t apiSportsTeam() {
  return (g_settings != nullptr) ? g_settings->apiSportsTeamId : 68;
}

/// Map football-data's status strings onto our own enum.
///
/// Note TIMED and SCHEDULED both mean "not started". Treating only SCHEDULED
/// as upcoming would make the next fixture vanish, since the API reports
/// imminent matches as TIMED.
model::MatchState parseState(const char* status) {
  if (status == nullptr) return model::MatchState::Unknown;
  if (strcmp(status, "FINISHED") == 0)  return model::MatchState::Finished;
  if (strcmp(status, "IN_PLAY") == 0)   return model::MatchState::InPlay;
  if (strcmp(status, "PAUSED") == 0)    return model::MatchState::Paused;
  if (strcmp(status, "POSTPONED") == 0) return model::MatchState::Postponed;
  if (strcmp(status, "TIMED") == 0 || strcmp(status, "SCHEDULED") == 0) {
    return model::MatchState::Scheduled;
  }
  return model::MatchState::Unknown;
}

/// Fill a Fixture from one football-data match object.
void readFixture(JsonObjectConst m, uint16_t ourId, model::Fixture& f) {
  JsonObjectConst home = m["homeTeam"];
  JsonObjectConst away = m["awayTeam"];

  setField(f.homeTla, home["tla"] | "");
  setField(f.awayTla, away["tla"] | "");
  setField(f.homeName, home["name"] | "");
  setField(f.awayName, away["name"] | "");
  setField(f.competition, m["competition"]["name"] | "");
  f.kickoffUtc = parseIso8601(m["utcDate"] | "");
  f.matchday   = m["matchday"] | 0;
  f.state      = parseState(m["status"] | "");
  f.homeId     = home["id"] | 0;
  f.awayId     = away["id"] | 0;
  f.weAreHome  = (f.homeId == ourId);

  JsonVariantConst ft = m["score"]["fullTime"];
  // A goal count of null means "not played", which is a different thing from
  // 0-0 — hence -1 rather than defaulting to zero.
  f.homeGoals = ft["home"].isNull() ? -1 : static_cast<int8_t>(ft["home"] | 0);
  f.awayGoals = ft["away"].isNull() ? -1 : static_cast<int8_t>(ft["away"] | 0);
  f.valid = true;
}

/// Our result in a completed fixture: 'W', 'D' or 'L'.
char resultChar(const model::Fixture& f) {
  const int8_t ours   = f.weAreHome ? f.homeGoals : f.awayGoals;
  const int8_t theirs = f.weAreHome ? f.awayGoals : f.homeGoals;
  if (ours < 0 || theirs < 0) return 0;
  return ours > theirs ? 'W' : (ours < theirs ? 'L' : 'D');
}

}  // namespace

void begin(const store::Settings& settings) { g_settings = &settings; }

uint32_t parseIso8601(const char* iso) {
  // Expected form: 2026-09-12T11:30:00Z
  if (iso == nullptr || strlen(iso) < 17) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5) {
    return 0;
  }
  if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  const int32_t days = daysFromCivil(y, static_cast<uint32_t>(mo),
                                     static_cast<uint32_t>(d));
  return static_cast<uint32_t>(days) * 86400UL + h * 3600UL + mi * 60UL + sec;
}

// ---------------------------------------------------------------------------
// League table
// ---------------------------------------------------------------------------

namespace {

/// Map a parsed standings document into the model. Shared by the network and
/// cache paths, so a cached document can never diverge from a fresh one.
void parseStandings(const JsonDocument& doc, model::Snapshot& out) {
  setField(out.competitionName, doc["competition"]["name"] | "");
  out.matchday = doc["season"]["currentMatchday"] | 0;

  uint8_t written = 0;
  out.ourRow = 0xFF;
  for (JsonObjectConst t : doc["standings"].as<JsonArrayConst>()) {
    // Only the TOTAL table; the API also returns HOME and AWAY splits.
    if (strcmp(t["type"] | "", "TOTAL") != 0) continue;
    for (JsonObjectConst e : t["table"].as<JsonArrayConst>()) {
      if (written >= model::Snapshot::kMaxTableRows) break;
      model::TableRow& dst = out.table[written];
      setField(dst.tla, e["team"]["tla"] | "");
      setField(dst.name, e["team"]["name"] | "");
      dst.id             = e["team"]["id"] | 0;
      dst.position       = e["position"] | 0;
      dst.played         = e["playedGames"] | 0;
      dst.won            = e["won"] | 0;
      dst.drawn          = e["draw"] | 0;
      dst.lost           = e["lost"] | 0;
      dst.goalsFor       = e["goalsFor"] | 0;
      dst.goalsAgainst   = e["goalsAgainst"] | 0;
      dst.goalDifference = e["goalDifference"] | 0;
      dst.points         = e["points"] | 0;
      // Matched on the provider's id, not on the display name.
      //
      // This used to be strstr(clubName, teamDisplayName) — a substring test
      // against a name the *user* typed. It happened to work for "Bolton
      // Wanderers" inside "Bolton Wanderers FC" and failed silently for
      // anything else: enter "Man City" for "Manchester City FC" and the row
      // is never flagged, so the Season screen reports no data and disappears
      // entirely. The id is exact and is what every other lookup already uses.
      dst.isOurTeam = (g_settings != nullptr) && dst.id != 0 &&
                      dst.id == g_settings->footballDataTeamId;
      if (dst.isOurTeam) out.ourRow = written;
      ++written;
    }
    break;
  }
  out.tableRows = written;
}

void parseOurMatchesImpl(const JsonDocument& doc, model::Snapshot& out);
void parseNextFixtureImpl(const JsonDocument& doc, model::Snapshot& out);
void parseScorersImpl(const JsonDocument& doc, model::Snapshot& out);

}  // namespace

// Wrappers giving the shared parsers external linkage with stable names, so
// both the network path and the cache path call exactly the same code. That
// equivalence is the point: a cached document can never render differently
// from a freshly fetched one.
static void parseOurMatches(const JsonDocument& d, model::Snapshot& o) {
  parseOurMatchesImpl(d, o);
}
static void parseNextFixture(const JsonDocument& d, model::Snapshot& o) {
  parseNextFixtureImpl(d, o);
}
static void parseScorers(const JsonDocument& d, model::Snapshot& o) {
  parseScorersImpl(d, o);
}
static void parseStandingsShared(const JsonDocument& d, model::Snapshot& o) {
  parseStandings(d, o);
}
static void parseOurMatchesShared(const JsonDocument& d, model::Snapshot& o) {
  parseOurMatchesImpl(d, o);
}
static void parseNextFixtureShared(const JsonDocument& d, model::Snapshot& o) {
  parseNextFixtureImpl(d, o);
}
static void parseScorersShared(const JsonDocument& d, model::Snapshot& o) {
  parseScorersImpl(d, o);
}

api::Result fetchStandings(model::Snapshot& out) {
  // The filter is the whole trick: of a response listing every team with
  // nested objects, only these leaves are ever materialised.
  JsonDocument filter;
  filter["competition"]["name"] = true;
  filter["season"]["currentMatchday"] = true;
  JsonObject row = filter["standings"][0]["table"].add<JsonObject>();
  row["position"]       = true;
  row["playedGames"]    = true;
  row["won"]            = true;
  row["draw"]           = true;
  row["lost"]           = true;
  row["goalsFor"]       = true;
  row["goalsAgainst"]   = true;
  row["goalDifference"] = true;
  row["points"]         = true;
  row["team"]["tla"]    = true;
  row["team"]["name"]   = true;
  row["team"]["id"]     = true;
  filter["standings"][0]["type"] = true;

  char path[64];
  snprintf(path, sizeof(path), "/v4/competitions/%s/standings", competition());

  JsonDocument doc;
  const api::Response r =
      api::fetch(api::Provider::FootballData, path, doc, filter);
  if (!r.ok()) return r.result;

  parseStandings(doc, out);
  api::persist(store::Doc::Standings, doc, 6 * 3600);
  Serial.printf("[prov] standings: %u rows, our row %u\n", out.tableRows,
                out.ourRow);
  return api::Result::Ok;
}

// ---------------------------------------------------------------------------
// Our matches: last result and form
// ---------------------------------------------------------------------------

namespace {

/// Filter shared by both match queries.
void buildMatchFilter(JsonDocument& filter) {
  JsonObject m = filter["matches"].add<JsonObject>();
  m["utcDate"]  = true;
  m["status"]   = true;
  m["matchday"] = true;
  m["competition"]["name"] = true;
  m["homeTeam"]["id"]   = true;
  m["homeTeam"]["tla"]  = true;
  m["homeTeam"]["name"] = true;
  m["awayTeam"]["id"]   = true;
  m["awayTeam"]["tla"]  = true;
  m["awayTeam"]["name"] = true;
  m["score"]["fullTime"]["home"] = true;
  m["score"]["fullTime"]["away"] = true;
}

}  // namespace

api::Result fetchOurMatches(model::Snapshot& out) {
  JsonDocument filter;
  buildMatchFilter(filter);

  char path[80];
  snprintf(path, sizeof(path),
           "/v4/teams/%u/matches?status=FINISHED&limit=6", footballDataTeam());

  JsonDocument doc;
  const api::Response r =
      api::fetch(api::Provider::FootballData, path, doc, filter);
  if (!r.ok()) return r.result;

  parseOurMatches(doc, out);
  api::persist(store::Doc::TeamMatches, doc, 6 * 3600);
  Serial.printf("[prov] our matches: form \"%s\", last %s %d-%d\n",
                out.lastResult.weAreHome ? out.lastResult.homeForm
                                         : out.lastResult.awayForm,
                out.lastResult.homeTla, out.lastResult.homeGoals,
                out.lastResult.awayGoals);
  return api::Result::Ok;
}

namespace {

void parseOurMatchesImpl(const JsonDocument& doc, model::Snapshot& out) {
  JsonArrayConst matches = doc["matches"];
  if (matches.size() == 0) return;  // Season not started.

  // Oldest first, so the most recent is last.
  char form[model::kFormLen] = {0};
  uint8_t formLen = 0;
  model::Fixture last;

  for (JsonObjectConst m : matches) {
    model::Fixture f;
    readFixture(m, footballDataTeam(), f);
    if (f.state != model::MatchState::Finished) continue;
    last = f;

    const char c = resultChar(f);
    if (c == 0) continue;
    if (formLen < model::kFormLen - 1) {
      form[formLen++] = c;
    } else {
      // Keep the newest five by shifting the oldest out.
      memmove(form, form + 1, model::kFormLen - 2);
      form[model::kFormLen - 2] = c;
    }
  }
  form[formLen < model::kFormLen ? formLen : model::kFormLen - 1] = '\0';

  if (last.valid) {
    out.lastResult = last;
    // Our form belongs on whichever side of each fixture we are.
    setField(out.lastResult.weAreHome ? out.lastResult.homeForm
                                      : out.lastResult.awayForm,
             form);
    if (out.nextFixture.valid) {
      setField(out.nextFixture.weAreHome ? out.nextFixture.homeForm
                                         : out.nextFixture.awayForm,
               form);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Next fixture
// ---------------------------------------------------------------------------

api::Result fetchNextFixture(model::Snapshot& out) {
  JsonDocument filter;
  buildMatchFilter(filter);

  char path[80];
  snprintf(path, sizeof(path),
           "/v4/teams/%u/matches?status=SCHEDULED&limit=3", footballDataTeam());

  JsonDocument doc;
  const api::Response r =
      api::fetch(api::Provider::FootballData, path, doc, filter);
  if (!r.ok()) return r.result;

  parseNextFixture(doc, out);
  api::persist(store::Doc::OpponentMatches, doc, 12 * 3600);
  return api::Result::Ok;
}

namespace {

void parseNextFixtureImpl(const JsonDocument& doc, model::Snapshot& out) {
  const uint32_t liveKickoff =
      out.liveActive ? out.live.fixture.kickoffUtc : 0;

  for (JsonObjectConst m : doc["matches"].as<JsonArrayConst>()) {
    model::Fixture f;
    readFixture(m, footballDataTeam(), f);
    // Skip the match being played, so Next never duplicates Live. Matched on
    // kick-off rather than id because the two providers number fixtures
    // differently and only the time is common to both.
    if (liveKickoff != 0 && f.kickoffUtc == liveKickoff) {
      // This is the match being played. Remember the opponent before moving
      // on: the live feed is api-sports, whose team ids are unrelated to
      // football-data's, so this is the only point at which the two can be
      // associated.
      out.liveOpponentId = model::Snapshot::opponentOf(f);
      continue;
    }
    // Preserve any form already derived for our side.
    char ourForm[model::kFormLen];
    strncpy(ourForm,
            out.nextFixture.weAreHome ? out.nextFixture.homeForm
                                      : out.nextFixture.awayForm,
            sizeof(ourForm));
    ourForm[sizeof(ourForm) - 1] = '\0';
    out.nextFixture = f;
    setField(out.nextFixture.weAreHome ? out.nextFixture.homeForm
                                       : out.nextFixture.awayForm,
             ourForm);
    Serial.printf("[prov] next: %s v %s md%u\n", f.homeTla, f.awayTla,
                  f.matchday);
    return;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Opponent form
// ---------------------------------------------------------------------------

api::Result fetchOpponentForm(model::Snapshot& out) {
  if (!out.nextFixture.valid) return api::Result::Ok;

  const uint16_t opponentId = model::Snapshot::opponentOf(out.nextFixture);
  if (opponentId == 0) return api::Result::Ok;

  // Only refetch when the opponent actually changes, which is roughly once a
  // matchday. Their form is not going to move between our own fixtures.
  static uint16_t lastFetchedFor = 0;
  if (opponentId == lastFetchedFor) return api::Result::Ok;

  JsonDocument filter;
  buildMatchFilter(filter);

  char path[80];
  snprintf(path, sizeof(path), "/v4/teams/%u/matches?status=FINISHED&limit=6",
           opponentId);

  JsonDocument doc;
  const api::Response r =
      api::fetch(api::Provider::FootballData, path, doc, filter);
  if (!r.ok()) return r.result;

  // Same derivation as our own form, from the opponent's point of view.
  char form[model::kFormLen] = {0};
  uint8_t formLen = 0;
  for (JsonObjectConst m : doc["matches"].as<JsonArrayConst>()) {
    model::Fixture f;
    readFixture(m, opponentId, f);  // "We" are the opponent here.
    if (f.state != model::MatchState::Finished) continue;
    const char c = resultChar(f);
    if (c == 0) continue;
    if (formLen < model::kFormLen - 1) {
      form[formLen++] = c;
    } else {
      memmove(form, form + 1, model::kFormLen - 2);
      form[model::kFormLen - 2] = c;
    }
  }
  form[formLen < model::kFormLen ? formLen : model::kFormLen - 1] = '\0';

  // Their form belongs on whichever side of our fixture they occupy.
  setField(out.nextFixture.weAreHome ? out.nextFixture.awayForm
                                     : out.nextFixture.homeForm,
           form);
  lastFetchedFor = opponentId;
  Serial.printf("[prov] opponent %u form \"%s\"\n", opponentId, form);
  return api::Result::Ok;
}

// ---------------------------------------------------------------------------
// Scorers
// ---------------------------------------------------------------------------

api::Result fetchScorers(model::Snapshot& out) {
  JsonDocument filter;
  JsonObject sc = filter["scorers"].add<JsonObject>();
  sc["goals"]         = true;
  sc["playedMatches"] = true;
  sc["player"]["name"] = true;
  sc["team"]["tla"]    = true;
  sc["team"]["name"]   = true;
  // Needed to tell our own players from everyone else's; the name comparison
  // it replaces was unreliable for the same reason as in the standings.
  sc["team"]["id"]     = true;

  // 100 rather than the default 10: the list bottoms out at one goal, so this
  // is what makes our own team's scorers reachable at all. A side near the
  // foot of the table has nobody in the league top ten.
  char path[80];
  snprintf(path, sizeof(path), "/v4/competitions/%s/scorers?limit=100",
           competition());

  JsonDocument doc;
  const api::Response r =
      api::fetch(api::Provider::FootballData, path, doc, filter);
  if (!r.ok()) return r.result;

  parseScorers(doc, out);
  api::persist(store::Doc::Scorers, doc, 24 * 3600);
  Serial.printf("[prov] scorers: %u league, %u ours\n", out.leagueScorerCount,
                out.teamScorerCount);
  return api::Result::Ok;
}

namespace {

void parseScorersImpl(const JsonDocument& doc, model::Snapshot& out) {
  out.leagueScorerCount = 0;
  out.teamScorerCount   = 0;
  const uint16_t ourId =
      (g_settings != nullptr) ? g_settings->footballDataTeamId : 0;

  for (JsonObjectConst e : doc["scorers"].as<JsonArrayConst>()) {
    // By id, for the same reason as the standings: a display name the user
    // typed is not a reliable key. When this failed, teamScorerCount stayed
    // at zero and the screen quietly fell back to the league-wide chart —
    // which looks like a design choice rather than a lookup that missed.
    const bool ours = (ourId != 0) && ((e["team"]["id"] | 0) == ourId);

    // The response is ordered by goals, so the first few are the league
    // leaders and the first few of ours are our leaders. No sorting needed.
    model::Scorer* dst = nullptr;
    if (out.leagueScorerCount < model::Snapshot::kMaxScorers) {
      dst = &out.leagueScorers[out.leagueScorerCount++];
    }
    if (dst != nullptr) {
      setField(dst->name, e["player"]["name"] | "");
      setField(dst->tla, e["team"]["tla"] | "");
      dst->goals  = e["goals"] | 0;
      dst->played = e["playedMatches"] | 0;
    }
    if (ours && out.teamScorerCount < model::Snapshot::kMaxScorers) {
      model::Scorer& t = out.teamScorers[out.teamScorerCount++];
      setField(t.name, e["player"]["name"] | "");
      setField(t.tla, e["team"]["tla"] | "");
      t.goals  = e["goals"] | 0;
      t.played = e["playedMatches"] | 0;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Live match
// ---------------------------------------------------------------------------

api::Result fetchLiveMatch(model::Snapshot& out) {
  JsonDocument filter;
  JsonObject fx = filter["response"].add<JsonObject>();
  fx["fixture"]["status"]["elapsed"] = true;
  fx["fixture"]["status"]["short"]   = true;
  fx["fixture"]["date"]              = true;
  fx["teams"]["home"]["id"]   = true;
  fx["teams"]["home"]["name"] = true;
  fx["teams"]["away"]["id"]   = true;
  fx["teams"]["away"]["name"] = true;
  fx["goals"]["home"] = true;
  fx["goals"]["away"] = true;
  fx["league"]["name"] = true;
  JsonObject ev = fx["events"].add<JsonObject>();
  ev["time"]["elapsed"] = true;
  ev["time"]["extra"]   = true;
  ev["type"]            = true;
  ev["detail"]          = true;
  ev["team"]["id"]      = true;
  ev["player"]["name"]  = true;

  JsonDocument doc;
  const api::Response r = api::fetch(api::Provider::ApiSports,
                                     "/fixtures?live=all", doc, filter);
  if (!r.ok()) return r.result;

  const uint16_t ourId = apiSportsTeam();
  out.liveActive = false;

  for (JsonObjectConst fxo : doc["response"].as<JsonArrayConst>()) {
    const uint16_t homeId = fxo["teams"]["home"]["id"] | 0;
    const uint16_t awayId = fxo["teams"]["away"]["id"] | 0;
    if (homeId != ourId && awayId != ourId) continue;

    model::LiveMatch& m = out.live;
    m = model::LiveMatch{};
    model::Fixture& f = m.fixture;
    setField(f.homeName, fxo["teams"]["home"]["name"] | "");
    setField(f.awayName, fxo["teams"]["away"]["name"] | "");
    setField(f.competition, fxo["league"]["name"] | "");
    f.kickoffUtc = parseIso8601(fxo["fixture"]["date"] | "");
    f.homeGoals  = fxo["goals"]["home"] | 0;
    f.awayGoals  = fxo["goals"]["away"] | 0;
    f.weAreHome  = (homeId == ourId);
    f.valid      = true;

    // Crest ids, which cannot simply be copied from this response: the live
    // feed is api-sports and numbers teams in its own space, while crests are
    // keyed on football-data ids. Ours is known from settings; the opponent's
    // is whatever the fixture list recorded when it spotted which upcoming
    // match had gone live.
    //
    // liveOpponentId may still be 0 if the fixture list has not been fetched
    // yet this session, in which case the opponent's crest simply appears
    // once it has. Drawing nothing is the correct behaviour for id 0.
    const uint16_t ourFootballDataId = footballDataTeam();
    f.homeId = f.weAreHome ? ourFootballDataId : out.liveOpponentId;
    f.awayId = f.weAreHome ? out.liveOpponentId : ourFootballDataId;

    const char* shortStatus = fxo["fixture"]["status"]["short"] | "";
    f.state = (strcmp(shortStatus, "HT") == 0) ? model::MatchState::Paused
                                               : model::MatchState::InPlay;
    m.minute = fxo["fixture"]["status"]["elapsed"] | 0;

    for (JsonObjectConst e : fxo["events"].as<JsonArrayConst>()) {
      if (m.eventCount >= model::LiveMatch::kMaxEvents) break;
      const char* type   = e["type"] | "";
      const char* detail = e["detail"] | "";

      model::EventKind kind;
      if (strcmp(type, "Goal") == 0) {
        // "Missed Penalty" is tested before "Penalty", because it contains it.
        // Checking for "Penalty" first classified a miss as a scored penalty
        // and drew a goal that never happened.
        if (strstr(detail, "Missed") != nullptr) {
          kind = model::EventKind::MissedPenalty;
        } else if (strstr(detail, "Penalty") != nullptr) {
          kind = model::EventKind::Penalty;
        } else if (strstr(detail, "Own") != nullptr) {
          kind = model::EventKind::OwnGoal;
        } else {
          kind = model::EventKind::Goal;
        }
      } else if (strcmp(type, "Card") == 0) {
        kind = (strstr(detail, "Red") != nullptr) ? model::EventKind::RedCard
                                                  : model::EventKind::YellowCard;
      } else {
        continue;  // Substitutions and anything else are skipped.
      }

      model::MatchEvent& dst = m.events[m.eventCount++];
      dst.minute = e["time"]["elapsed"] | 0;
      dst.extra  = e["time"]["extra"] | 0;
      dst.kind   = kind;
      const uint16_t eventTeam = e["team"]["id"] | 0;
      // An own goal is filed under the side that benefits, which is the side
      // that did *not* score it.
      const bool scoredByHome = (eventTeam == homeId);
      // An own goal is filed under the side that benefits. A *missed* penalty
      // is not — it stays with the team that missed it, because nobody
      // benefits from it in a way worth showing under their name.
      dst.home = (kind == model::EventKind::OwnGoal) ? !scoredByHome
                                                     : scoredByHome;
      setField(dst.player, e["player"]["name"] | "");
    }

    const int8_t ours   = f.weAreHome ? f.homeGoals : f.awayGoals;
    const int8_t theirs = f.weAreHome ? f.awayGoals : f.homeGoals;
    m.provisionalResult = ours > theirs ? 'W' : (ours < theirs ? 'L' : 'D');

    out.liveActive = true;
    Serial.printf("[prov] LIVE %s %d-%d %s at %u' (%u events)\n", f.homeName,
                  f.homeGoals, f.awayGoals, f.awayName, m.minute,
                  m.eventCount);
    return api::Result::Ok;
  }

  Serial.println(F("[prov] no live match for our team"));
  return api::Result::Ok;
}

// ---------------------------------------------------------------------------
// Cache restore
// ---------------------------------------------------------------------------

bool cacheIsFresh(store::Doc doc) {
  return store::statusOf(doc).fresh;
}

namespace {

/// Read one cached document and hand it to a parser.
///
/// The buffer is sized from the file itself rather than fixed, so a document
/// growing (a longer scorers list, say) cannot silently start failing.
bool restore(store::Doc doc, model::Snapshot& out,
             void (*parse)(const JsonDocument&, model::Snapshot&)) {
  const store::DocStatus st = store::statusOf(doc);
  if (!st.present || st.size == 0) return false;

  char* buffer = static_cast<char*>(malloc(st.size + 1));
  if (buffer == nullptr) {
    Serial.printf("[prov] cannot allocate %lu bytes for cached %s\n",
                  (unsigned long)(st.size + 1), store::docName(doc));
    return false;
  }
  const size_t read = store::readDoc(doc, buffer, st.size + 1);
  bool ok = false;
  if (read > 0) {
    JsonDocument parsed;
    // No filter: what was written was already filtered, which is the point of
    // storing the reduced document rather than the response.
    const DeserializationError err = deserializeJson(parsed, buffer, read);
    if (err) {
      // A cached document that will not parse is worse than none: delete it so
      // the next fetch replaces it instead of failing here on every boot.
      Serial.printf("[prov] cached %s unparseable (%s) -- discarding\n",
                    store::docName(doc), err.c_str());
      store::clearDoc(doc);
    } else {
      parse(parsed, out);
      ok = true;
    }
  }
  free(buffer);
  return ok;
}

}  // namespace

uint8_t loadFromCache(model::Snapshot& out) {
  uint8_t restored = 0;
  // Order matters: standings first so the table and our row exist, then the
  // fixtures, which want our form attached to the right side.
  if (restore(store::Doc::Standings, out, parseStandingsShared)) ++restored;
  if (restore(store::Doc::TeamMatches, out, parseOurMatchesShared)) ++restored;
  if (restore(store::Doc::OpponentMatches, out, parseNextFixtureShared)) {
    ++restored;
  }
  if (restore(store::Doc::Scorers, out, parseScorersShared)) ++restored;

  // No per-document shape check here: the cache schema version in store.cpp
  // covers this generally, and two mechanisms doing the same job is how one
  // of them ends up forgotten.
  if (restored > 0) {
    Serial.printf("[prov] restored %u cached documents, no API calls spent\n",
                  restored);
  } else {
    Serial.println(F("[prov] no usable cache; will fetch"));
  }
  return restored;
}

}  // namespace providers
