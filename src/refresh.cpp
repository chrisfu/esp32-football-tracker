/**
 * @file refresh.cpp
 * @brief Fetch scheduling. See refresh.h.
 */

#include "refresh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "api_client.h"
#include "providers.h"

namespace refresh {
namespace {

/// Staging snapshot, written only by the fetch task.
model::Snapshot g_staging;
/// Raised by the task when g_staging holds new data; lowered by the UI once
/// adopted. Doubles as the ownership transfer — see the header.
volatile bool g_ready = false;
volatile bool g_primed = false;

const store::Settings* g_settings = nullptr;

// --- Refresh intervals ------------------------------------------------------
//
// Chosen against what actually changes rather than by round numbers. The table
// cannot move except after a match, so polling it hourly would waste requests
// on a Tuesday; the fixture list changes even less often.

constexpr uint32_t kStandingsIntervalS = 6 * 3600;
constexpr uint32_t kMatchesIntervalS   = 6 * 3600;
constexpr uint32_t kFixtureIntervalS   = 12 * 3600;
constexpr uint32_t kScorersIntervalS   = 24 * 3600;

/// How long before kick-off to start watching for the match to begin, and how
/// long after to keep watching. Three hours covers a delayed start plus a full
/// match and a generous stoppage period.
constexpr uint32_t kLiveWindowBeforeS = 15 * 60;
constexpr uint32_t kLiveWindowAfterS  = 3 * 3600;

// Adaptive live polling (SPEC.md §6). Tighter when the match is likely to
// change, slack when it is not, so a 2-hour match does not consume the day.
constexpr uint32_t kLivePollQuietS   = 180;  ///< In play, nothing recent.
constexpr uint32_t kLivePollActiveS  = 90;   ///< Just after a goal, or late on.
constexpr uint32_t kLivePollPausedS  = 600;  ///< Half time: nothing happens.
constexpr uint32_t kLivePollPreKickS = 300;  ///< Waiting for kick-off.

struct Task {
  const char* name;
  uint32_t    intervalS;
  uint32_t    lastOkAt;   ///< Unix seconds; 0 means never.
  api::Result (*run)(model::Snapshot&);
};

Task g_tasks[] = {
    {"standings", kStandingsIntervalS, 0, providers::fetchStandings},
    {"matches",   kMatchesIntervalS,   0, providers::fetchOurMatches},
    {"fixture",   kFixtureIntervalS,   0, providers::fetchNextFixture},
    {"scorers",   kScorersIntervalS,   0, providers::fetchScorers},
};
constexpr uint8_t kTaskCount = sizeof(g_tasks) / sizeof(g_tasks[0]);

uint32_t g_lastLiveFetchAt = 0;
uint32_t g_lastGoalSeenAt  = 0;
uint8_t  g_lastGoalTotal   = 0;

uint32_t nowUtc() {
  const time_t t = time(nullptr);
  return t > 1600000000L ? static_cast<uint32_t>(t) : 0;
}

/// Whether we are inside the window where a match might be in progress.
///
/// This is the whole reason idle days cost almost nothing: the fixture list is
/// cached, so knowing there is no match on is free, and api-sports is not
/// touched at all.
bool inLiveWindow(const model::Snapshot& s, uint32_t now) {
  if (now == 0) return false;
  // A match already known to be live keeps the window open regardless of the
  // scheduled time, which covers a kick-off delayed beyond our estimate.
  if (s.liveActive) return true;
  if (!s.nextFixture.valid || s.nextFixture.kickoffUtc == 0) return false;
  const uint32_t ko = s.nextFixture.kickoffUtc;
  return now + kLiveWindowBeforeS >= ko && now <= ko + kLiveWindowAfterS;
}

/// How long to wait before the next live poll, given the current state.
uint32_t livePollInterval(const model::Snapshot& s, uint32_t now) {
  if (!s.liveActive) return kLivePollPreKickS;
  if (s.live.fixture.state == model::MatchState::Paused) {
    return kLivePollPausedS;
  }
  // Tighten just after a goal — VAR can reverse one — and for the closing
  // stages, where matches are decided.
  if (g_lastGoalSeenAt != 0 && now - g_lastGoalSeenAt < 180) {
    return kLivePollActiveS;
  }
  if (s.live.minute >= 80) return kLivePollActiveS;
  return kLivePollQuietS;
}

/// Note goals so polling can tighten after one.
void noteGoals(const model::Snapshot& s, uint32_t now) {
  if (!s.liveActive) {
    g_lastGoalTotal = 0;
    return;
  }
  const uint8_t total = static_cast<uint8_t>(
      max<int8_t>(s.live.fixture.homeGoals, 0) +
      max<int8_t>(s.live.fixture.awayGoals, 0));
  if (total != g_lastGoalTotal) {
    g_lastGoalTotal = total;
    g_lastGoalSeenAt = now;
  }
}

/// One pass of the scheduler. Returns true if anything was fetched.
bool runDue(uint32_t now) {
  bool fetched = false;

  // Live first: it is the most time-sensitive thing we do, and during a match
  // it is the only thing that matters.
  if (inLiveWindow(g_staging, now)) {
    const uint32_t interval = livePollInterval(g_staging, now);
    if (g_lastLiveFetchAt == 0 || now - g_lastLiveFetchAt >= interval) {
      if (api::canAfford(api::Provider::ApiSports)) {
        const api::Result r = providers::fetchLiveMatch(g_staging);
        if (r == api::Result::Ok) {
          g_lastLiveFetchAt = now;
          noteGoals(g_staging, now);
          fetched = true;
        } else {
          Serial.printf("[refresh] live fetch: %s\n", api::resultName(r));
          // Back off on failure so a persistent error cannot spin.
          g_lastLiveFetchAt = now;
        }
      }
    }
  } else if (g_staging.liveActive) {
    // Outside the window with a stale live flag: the match is long over and
    // the provider stopped reporting it. Clear it so the rotation returns to
    // normal even if the final poll was missed.
    g_staging.liveActive = false;
    fetched = true;
  }

  // Then one scheduled task per pass, so a burst of due work is spread out
  // rather than issued as a rapid series the provider would rate-limit.
  for (uint8_t i = 0; i < kTaskCount; ++i) {
    Task& t = g_tasks[i];
    const bool due = (t.lastOkAt == 0) || (now - t.lastOkAt >= t.intervalS);
    if (!due) continue;
    if (!api::canAfford(api::Provider::FootballData)) break;

    const api::Result r = t.run(g_staging);
    if (r == api::Result::Ok) {
      t.lastOkAt = now;
      fetched = true;
    } else {
      Serial.printf("[refresh] %s: %s\n", t.name, api::resultName(r));
      // Do not hammer a failing endpoint: wait a short interval before
      // retrying by pretending it succeeded a while ago.
      t.lastOkAt = now - t.intervalS + 300;
    }
    break;
  }
  return fetched;
}

void fetchTask(void*) {
  for (;;) {
    // Only work when there is somewhere to put the result. If the UI has not
    // yet adopted the last batch, the staging buffer is not ours to touch.
    if (g_ready) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    const uint32_t now = nowUtc();
    if (now == 0) {
      // No clock means TLS cannot validate certificate dates, so there is no
      // point attempting a fetch yet.
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    if (runDue(now)) {
      g_ready = true;
      if (!g_primed) g_primed = true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

void begin(const store::Settings& settings, const model::Snapshot& initial) {
  g_settings = &settings;
  g_staging  = initial;

  api::begin(settings.apiSportsKey, settings.footballDataKey);
  providers::begin(settings);

  // A TLS handshake is several seconds of solid computation on this chip, and
  // it happens inside mbedTLS where we cannot yield. With core 0's idle task
  // subscribed to the task watchdog, that reads as a hang and panics the whole
  // device — which it did, as a boot loop, the first time a real fetch ran.
  //
  // Unsubscribing IDLE0 is the accepted remedy for a core running network
  // work. The cost is real and worth stating: a genuine hang on core 0 will no
  // longer be caught automatically. Core 1, which runs the UI, keeps its
  // watchdog, so a hang there is still detected — and that is the core whose
  // health the user would actually notice.
  esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));

  // Core 0 alongside the radio; the UI keeps core 1 to itself. 8 KB of stack
  // is generous, but TLS and the JSON parser both use a fair amount and a
  // stack overflow here would present as a mysterious reboot.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 8192, nullptr, 1, nullptr, 0);
  Serial.println(F("[refresh] fetch task started on core 0"));
}

bool adopt(model::Snapshot& live) {
  if (!g_ready) return false;
  live = g_staging;   // Safe: the task will not touch staging while g_ready.
  g_ready = false;    // Hands ownership back.
  return true;
}

void invalidateAll() {
  for (uint8_t i = 0; i < kTaskCount; ++i) g_tasks[i].lastOkAt = 0;
  g_lastLiveFetchAt = 0;
}

uint32_t secondsToNextFetch() {
  const uint32_t now = nowUtc();
  if (now == 0) return 0;
  uint32_t soonest = UINT32_MAX;
  for (uint8_t i = 0; i < kTaskCount; ++i) {
    const Task& t = g_tasks[i];
    if (t.lastOkAt == 0) return 0;
    const uint32_t due = t.lastOkAt + t.intervalS;
    if (due > now && due - now < soonest) soonest = due - now;
  }
  return soonest == UINT32_MAX ? 0 : soonest;
}

bool primed() { return g_primed; }

}  // namespace refresh
