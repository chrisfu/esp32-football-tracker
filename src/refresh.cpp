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
#include "crest_cache.h"
#include "model.h"
#include "ota.h"
#include "power.h"
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

/// The window in which a match might be under way. Opens shortly before the
/// scheduled kick-off and stays open long enough to cover a delayed start,
/// a full match and extra time.
constexpr uint32_t kLiveWindowBeforeS = 5 * 60;
constexpr uint32_t kLiveWindowAfterS  = 4 * 3600;

// Live polling.
//
// A club plays once or twice a week and never twice in a day, so a match is
// the one time spending quota is plainly worth it. The in-play interval is a
// setting; these are the cases that deviate from it.
//
// The waiting-for-kick-off interval matters more than it looks: it decides how
// long after the whistle the screen notices. It was 5 minutes, which combined
// with a window opening 15 minutes early meant a match could be a good while
// old before appearing. 60 seconds catches it within a minute of kick-off,
// and only for the short stretch around the scheduled time.
constexpr uint32_t kLivePollPreKickS = 60;
constexpr uint32_t kLivePollActiveS  = 120;  ///< After a goal, or late on.
constexpr uint32_t kLivePollPausedS  = 600;  ///< Half time: nothing happens.

/// After a match finishes, everything derived from it is stale: the table, the
/// scorers, our record, and the next fixture. Refreshed once, shortly after,
/// rather than waiting hours for their own intervals to come round.
constexpr uint32_t kPostMatchDelayS = 5 * 60;

struct Task {
  const char* name;
  uint32_t    intervalS;
  uint32_t    lastOkAt;   ///< Unix seconds; 0 means never.
  api::Result (*run)(model::Snapshot&);
  /**
   * The cached document whose timestamp seeds this task's schedule.
   *
   * Held here rather than in a parallel array, which is how this went wrong
   * before: a `Doc[kTaskCount]` with one initialiser too few zero-filled its
   * last element to `Doc::Standings`, so the opponent-form task inherited the
   * standings document's freshness and was considered permanently up to date.
   * It therefore never ran, and the opponent's form never appeared.
   *
   * `Doc::Count` means "this task caches nothing", which is the honest state
   * for a task whose result is folded into another document.
   */
  store::Doc  cacheDoc;
};

Task g_tasks[] = {
    {"standings", kStandingsIntervalS, 0, providers::fetchStandings,
     store::Doc::Standings},
    {"matches",   kMatchesIntervalS,   0, providers::fetchOurMatches,
     store::Doc::TeamMatches},
    {"fixture",   kFixtureIntervalS,   0, providers::fetchNextFixture,
     store::Doc::OpponentMatches},
    {"scorers",   kScorersIntervalS,   0, providers::fetchScorers,
     store::Doc::Scorers},
    // The opponent's form is written into the next fixture rather than stored
    // as its own document, so there is no cached timestamp to seed from. It
    // runs once after every boot, which is cheap and keeps the form correct
    // even though it is not itself persisted.
    {"oppform",   kMatchesIntervalS,   0, providers::fetchOpponentForm,
     store::Doc::Count},
};
constexpr uint8_t kTaskCount = sizeof(g_tasks) / sizeof(g_tasks[0]);

/// Whether the schedule has been aligned to the cache's timestamps yet.
bool g_seededFromCache = false;

// --- Firmware updates -------------------------------------------------------
volatile bool  g_updateRequested = false;
volatile bool  g_updateApply     = false;
volatile bool  g_updateBusy      = false;
ota::UpdateInfo g_updateInfo;
uint32_t       g_lastUpdateCheckAt = 0;

/// Set at boot when NVS says the last update check never finished — meaning it
/// took the device down with it. Automatic checks stay off until a manual one
/// from the System page succeeds, which is the user deciding to try again.
bool           g_otaCheckDisabled       = false;
bool           g_otaCheckDisabledLogged = false;

/// Automatic checks are daily. Firmware does not appear more often than that,
/// and a device that phones home constantly is impolite.
constexpr uint32_t kUpdateCheckIntervalS = 24 * 3600;

/// Show download progress on the panel, since a 1.2 MB download over a weak
/// signal takes long enough that a frozen-looking screen would worry someone.
void onUpdateProgress(uint8_t percent) {
  static uint8_t lastShown = 255;
  if (percent == lastShown) return;
  lastShown = percent;
  if (percent % 10 == 0) Serial.printf("[ota] %u%%\n", percent);
}

uint32_t g_lastLiveFetchAt = 0;
uint32_t g_lastGoalSeenAt  = 0;
uint8_t  g_lastGoalTotal   = 0;
/// Set when a match is seen to end, so the post-match refresh can be timed.
uint32_t g_postMatchDueAt  = 0;
/// Tracks the live flag between passes, to spot the transitions.
bool     g_wasLive         = false;
/// Reported once per dry spell, so a silent stop is never a mystery.
bool     g_quotaWarned     = false;

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

  // Either the next fixture or the last result can open the window. The last
  // result matters because once a match kicks off, the fixture list moves on
  // to the following game — so relying on nextFixture alone would close the
  // window on the very match being played.
  const uint32_t candidates[2] = {
      s.nextFixture.valid ? s.nextFixture.kickoffUtc : 0,
      s.lastResult.valid ? s.lastResult.kickoffUtc : 0,
  };
  for (uint32_t ko : candidates) {
    if (ko == 0) continue;
    if (now + kLiveWindowBeforeS >= ko && now <= ko + kLiveWindowAfterS) {
      return true;
    }
  }
  return false;
}

/// How long to wait before the next live poll, given the current state.
uint32_t livePollInterval(const model::Snapshot& s, uint32_t now,
                          const store::Settings& settings) {
  if (!s.liveActive) return kLivePollPreKickS;
  if (s.live.fixture.state == model::MatchState::Paused) {
    return kLivePollPausedS;
  }
  // Tighten just after a goal — VAR can reverse one — and for the closing
  // stages, where matches are decided.
  if (g_lastGoalSeenAt != 0 && now - g_lastGoalSeenAt < 300) {
    return kLivePollActiveS;
  }
  if (s.live.minute >= 80) return kLivePollActiveS;
  return settings.livePollSeconds > 0 ? settings.livePollSeconds : 300;
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

/**
 * Keep exactly the crests that are currently worth having.
 *
 * At most four: our team, the last opponent, the next opponent, and whoever
 * we are playing right now. Caching a whole division would be pointless when
 * the team is chosen by the user — and wrong, since the interesting set moves
 * every matchday.
 *
 * Crests cost no API quota (the crest host is static, not the rate-limited
 * API), so this is bounded only by time and flash, both of which are cheap
 * here. It runs after the fixtures are known, since that is what determines
 * the set.
 */
/// Ids synced on the last pass, so unchanged sets are not re-examined.
uint16_t g_syncedCrests[4] = {0, 0, 0, 0};
uint8_t  g_syncedCount     = 0;
uint32_t g_lastCrestTryAt  = 0;

/// Retry interval for a crest that failed to download, so a persistent
/// failure (a team with no crest on file, say) costs one attempt every few
/// minutes rather than one per second.
constexpr uint32_t kCrestRetryS = 300;

void syncCrests(const model::Snapshot& s, const store::Settings& settings) {
  uint16_t wanted[4];
  uint8_t  count = 0;

  const auto add = [&](uint16_t id) {
    if (id == 0 || count >= 4) return;
    for (uint8_t i = 0; i < count; ++i) {
      if (wanted[i] == id) return;  // Already listed.
    }
    wanted[count++] = id;
  };

  add(settings.footballDataTeamId);
  if (s.lastResult.valid)  add(model::Snapshot::opponentOf(s.lastResult));
  if (s.nextFixture.valid) add(model::Snapshot::opponentOf(s.nextFixture));
  add(s.liveOpponentId);

  // Has anything actually changed, and is anything missing?
  bool setChanged = (count != g_syncedCount);
  for (uint8_t i = 0; i < count && !setChanged; ++i) {
    if (wanted[i] != g_syncedCrests[i]) setChanged = true;
  }
  bool anyMissing = false;
  for (uint8_t i = 0; i < count; ++i) {
    if (!crest::available(wanted[i])) anyMissing = true;
  }
  if (!setChanged && !anyMissing) return;  // Nothing to do.

  const uint32_t now = nowUtc();
  // Rate-limit retries. Without this a crest that cannot be downloaded would
  // be attempted on every pass forever.
  if (!setChanged && g_lastCrestTryAt != 0 &&
      now - g_lastCrestTryAt < kCrestRetryS) {
    return;
  }
  g_lastCrestTryAt = now;

  // Prune first: freeing space before downloading matters on a filesystem
  // shared with the JSON cache.
  if (setChanged) crest::prune(wanted, count);

  for (uint8_t i = 0; i < count; ++i) {
    if (!crest::available(wanted[i])) crest::ensure(wanted[i]);
  }

  memcpy(g_syncedCrests, wanted, sizeof(uint16_t) * count);
  g_syncedCount = count;
  Serial.printf("[crest] set synced (%u wanted, %lu bytes on disk)\n", count,
                (unsigned long)crest::bytesUsed());
}

/// One pass of the scheduler. Returns true if anything was fetched.
bool runDue(uint32_t now) {
  bool fetched = false;

  // Live first: it is the most time-sensitive thing we do, and during a match
  // it is the only thing that matters.
  //
  // Skipped entirely in a simulated build — not just to preserve the fake
  // fixture, but because the simulation would otherwise invite the fetch:
  // inLiveWindow() is true precisely when liveActive is set.
  if (SIMULATE_LIVE_MATCH == 0 && inLiveWindow(g_staging, now)) {
    const uint32_t interval = livePollInterval(g_staging, now, *g_settings);
    if (g_lastLiveFetchAt == 0 || now - g_lastLiveFetchAt >= interval) {
      if (api::canAfford(api::Provider::ApiSports)) {
        g_quotaWarned = false;
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
      } else if (api::cooldownRemaining(api::Provider::ApiSports) > 0) {
        // Just the minimum spacing between calls, which passes in seconds.
        // Distinguished from a real shortage because reporting it as one was
        // actively misleading: the first version of this message announced
        // "quota paused" at 13 of 100 used.
      } else {
        // Said out loud, once per dry spell. This used to do nothing at all,
        // silently: when the daily allowance ran out mid-match the screen
        // simply froze on whatever minute it had last seen, with nothing
        // anywhere saying why. A frozen clock that explains itself is a far
        // better failure than one that does not.
        if (!g_quotaWarned) {
          g_quotaWarned = true;
          store::Quota q;
          store::loadQuota(q);
          Serial.printf(
              "[refresh] live polling paused: api-sports %u of %u used today, "
              "holding the last few back for a manual refresh\n",
              q.used, q.limit);
        }
        g_lastLiveFetchAt = now;  // Do not re-check every pass.
      }
    }
  }

  // --- Match transitions --------------------------------------------------
  //
  // Both edges matter, and neither was handled before. A match starting means
  // the fixture list is out of date, because "next" is now the match being
  // played. A match ending means the table, the scorers, our record and the
  // next fixture are all stale at once.
  if (g_staging.liveActive && !g_wasLive) {
    g_wasLive = true;
    g_postMatchDueAt = 0;
    Serial.println(F("[refresh] match under way; refreshing the fixture list"));
    // Force the fixture task so Next advances past the match being played
    // rather than continuing to show it.
    for (Task& t : g_tasks) {
      if (strcmp(t.name, "fixture") == 0) t.lastOkAt = 0;
    }
  } else if (!g_staging.liveActive && g_wasLive) {
    g_wasLive = false;
    g_postMatchDueAt = now + kPostMatchDelayS;
    Serial.printf("[refresh] match over; full refresh due in %lus\n",
                  (unsigned long)kPostMatchDelayS);
  }

  // The provider may also report the match as finished while still listing it.
  if (g_staging.liveActive &&
      g_staging.live.fixture.state == model::MatchState::Finished &&
      g_postMatchDueAt == 0) {
    g_postMatchDueAt = now + kPostMatchDelayS;
    Serial.println(F("[refresh] full time reported; refresh scheduled"));
  }

  if (g_postMatchDueAt != 0 && now >= g_postMatchDueAt) {
    g_postMatchDueAt = 0;
    Serial.println(F("[refresh] post-match refresh"));
    for (Task& t : g_tasks) t.lastOkAt = 0;
    // The match is finished, so stop treating it as live even if the last
    // poll still listed it — otherwise the screen keeps a completed match on
    // display indefinitely, which is exactly what it did.
    g_staging.liveActive = false;
    fetched = true;
  }

  // Then one scheduled task per pass, so a burst of due work is spread out
  // rather than issued as a rapid series the provider would rate-limit.
  for (uint8_t i = 0; i < kTaskCount; ++i) {
    Task& t = g_tasks[i];
    // football-data has no daily cap, so the interval is a setting rather
    // than a per-task constant chosen to be frugal with something free.
    const uint32_t intervalS =
        (g_settings != nullptr && g_settings->freeRefreshMinutes > 0)
            ? static_cast<uint32_t>(g_settings->freeRefreshMinutes) * 60
            : t.intervalS;
    const bool due = (t.lastOkAt == 0) || (now - t.lastOkAt >= intervalS);
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
      t.lastOkAt = now - intervalS + 300;
    }
    break;
  }
  return fetched;
}

/**
 * Align the schedule with what the cache already holds.
 *
 * A document still inside its TTL is treated as though it had just been
 * fetched, so it is not requested again. This is what makes a reboot cost
 * nothing: without it the cache would still populate the screens, but every
 * endpoint would be re-fetched moments later anyway, which rather defeats the
 * point of having one.
 *
 * Requires a valid clock, hence being called from the fetch task rather than
 * from begin().
 */
void seedScheduleFromCache(uint32_t now) {
  for (uint8_t i = 0; i < kTaskCount; ++i) {
    // A task with no cached document has nothing to seed from and should run.
    if (g_tasks[i].cacheDoc == store::Doc::Count) continue;
    const store::DocStatus st = store::statusOf(g_tasks[i].cacheDoc);
    if (!st.present || st.fetchedAt == 0) continue;
    // Guard against a timestamp from the future, which would otherwise defer
    // a fetch indefinitely — possible if the clock was wrong when it was
    // written, or the device moved timezone-agnostic data between builds.
    if (st.fetchedAt > now) continue;
    const uint32_t intervalS =
        (g_settings != nullptr && g_settings->freeRefreshMinutes > 0)
            ? static_cast<uint32_t>(g_settings->freeRefreshMinutes) * 60
            : g_tasks[i].intervalS;
    const uint32_t age = now - st.fetchedAt;
    if (age >= intervalS) continue;  // Genuinely due.

    g_tasks[i].lastOkAt = st.fetchedAt;
    Serial.printf("[refresh] %s cached %lus ago; next in %lus\n",
                  g_tasks[i].name, (unsigned long)age,
                  (unsigned long)(intervalS - age));
  }
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

    if (!g_seededFromCache) {
      g_seededFromCache = true;
      seedScheduleFromCache(now);
    }

    // Firmware updates take priority over data: there is no point spending
    // requests on a build that is about to be replaced.
    if (g_updateRequested && g_settings != nullptr) {
      g_updateRequested = false;
      g_updateBusy      = true;
      const bool apply  = g_updateApply;
      // Someone asked for this one, so it runs even if a previous attempt
      // crashed — but it is still marked, so a crash here disables only the
      // *automatic* check rather than looping the device.
      store::markOtaCheckStarted();
      if (ota::checkForUpdate(g_settings->otaManifestUrl, g_updateInfo)) {
        g_lastUpdateCheckAt = now;
        if (apply && g_updateInfo.available) {
          // Reboots on success, so nothing after this runs.
          ota::applyUpdate(g_updateInfo, onUpdateProgress);
        }
      } else {
        Serial.printf("[ota] check failed: %s\n", ota::lastError());
        g_lastUpdateCheckAt = now;  // Back off rather than retrying at once.
      }
      store::markOtaCheckFinished();
      // It came back, so whatever took the device down before is no longer
      // happening: automatic checks can resume.
      if (g_otaCheckDisabled) {
        g_otaCheckDisabled       = false;
        g_otaCheckDisabledLogged = false;
        Serial.println("[ota] check completed; automatic checks re-enabled");
      }
      g_updateBusy = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Automatic checks report availability but never install. Replacing
    // firmware someone is relying on is not a decision to make for them.
    if (g_settings != nullptr && g_settings->otaAutoCheck &&
        g_settings->otaManifestUrl[0] != '\0' &&
        (g_lastUpdateCheckAt == 0 ||
         now - g_lastUpdateCheckAt >= kUpdateCheckIntervalS)) {
      // g_lastUpdateCheckAt lives in RAM, so it is 0 after every reset and
      // this fires within seconds of each boot. That is fine when the check
      // works and catastrophic when it does not, which is why the marker below
      // is consulted first.
      if (g_otaCheckDisabled) {
        // Said once, not every pass.
        if (!g_otaCheckDisabledLogged) {
          g_otaCheckDisabledLogged = true;
          Serial.println(
              "[ota] a previous update check did not finish; automatic checks "
              "are disabled until one is run from the System page");
        }
      } else {
        g_lastUpdateCheckAt = now;
        store::markOtaCheckStarted();
        ota::checkForUpdate(g_settings->otaManifestUrl, g_updateInfo);
        store::markOtaCheckFinished();
      }
    }

    const bool fetched = runDue(now);

    // Crests are synced whether or not anything was fetched. An earlier
    // version only did this after a fetch, so a device starting with a warm
    // cache — the normal case after any reboot — never downloaded a crest at
    // all, because nothing was due.
    if (g_settings != nullptr) syncCrests(g_staging, *g_settings);

    if (fetched) {
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

  // Before anything touches the network: did the last update check return?
  // If not, it crashed, and repeating it on this boot would simply repeat the
  // crash.
  g_otaCheckDisabled = store::otaCheckWasInterrupted();
  if (g_otaCheckDisabled) {
    Serial.println(
        "[ota] previous update check did not finish -- automatic checks are "
        "off until one succeeds from the System page");
  }

  api::begin(settings.apiSportsKey, settings.footballDataKey);
  providers::begin(settings);
  crest::begin();

  // Restore whatever the cache holds before any fetching. Two benefits: the
  // screens show real data almost immediately rather than after six seconds
  // per endpoint, and a restart costs nothing against the daily quota.
  if (providers::loadFromCache(g_staging) > 0) {
    g_ready  = true;   // Hand it to the UI at once.
    g_primed = true;
  }

  // Seeding the schedule from the cache is deliberately NOT done here. This
  // runs before NTP has completed, so there is no clock to judge freshness
  // against — an earlier version checked here, always saw an unset clock, and
  // therefore re-fetched everything on every reboot despite a perfectly good
  // cache. It is done on the fetch task's first pass with a valid clock
  // instead; see seedScheduleFromCache().


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
  // 12 KB, not 8 KB.
  //
  // 8 KB was already marginal — a TLS handshake plus mbedTLS certificate chain
  // verification is several KB on its own — and the OTA check overflowed it
  // outright ("Stack canary watchpoint triggered (fetch)"). The updater's big
  // buffers have moved to the heap, which fixes that; this is the margin, since
  // the task also runs the JSON fetches and RAM is the resource this board has
  // plenty of (17% used).
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
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

void requestUpdateCheck(bool applyIfFound) {
  g_updateApply     = applyIfFound;
  g_updateRequested = true;
}

const ota::UpdateInfo& updateInfo() { return g_updateInfo; }

bool updateInProgress() { return g_updateBusy; }

}  // namespace refresh
