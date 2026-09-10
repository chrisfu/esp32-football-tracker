# Football Stat Tracker — Specification

A Wi-Fi connected football (soccer) statistics display for the ESP32 "Cheap
Yellow Display" (2.8" ESP32-2432S028R). It tracks one team, cycles through
screens of useful information, and is configured entirely from a web interface.

**Status:** in design / hardware bring-up.
**Living document** — updated as features are added or decisions change. Every
design choice records *why*, so we can revisit it when constraints change.

---

## 1. Guiding rules

These are the project's standing constraints. All later sections must comply.

| # | Rule | Consequence |
|---|---|---|
| R1 | Build iteratively — branch, small reviewable commits, merge to `main` | One feature per branch; each commit builds |
| R2 | Cache whenever we pull data | Nothing is fetched twice if a valid cached copy exists |
| R3 | Stored data is optimised for limited storage | Field-filtered JSON, no pretty-printing, short keys |
| R4 | Clean, well-commented, optimised code | Comments explain *why*; hot paths measured, not guessed |
| R5 | Optimise for power | Deliberate phase once features are stable (§9) |
| R6 | Make full use of available hardware | LDR, RGB LED, speaker, SD, touch IRQ, DMA, dual core (§8) |
| R7 | Ration API calls carefully and show usage in the web UI | Hard budget enforcement (§6) |
| R8 | JSON documents, not databases | LittleFS files + NVS for secrets |

---

## 2. Hardware envelope

Full verified detail in [docs/HARDWARE.md](docs/HARDWARE.md). The numbers that
drive design:

* **ESP32-D0WD-V3**, dual core 240 MHz, Wi-Fi + BT.
* **4 MB flash. No PSRAM.** Measured at boot: 349,900 bytes free heap, but the
  **largest contiguous block is only 114,676 bytes** — and that is what caps any
  single allocation, before Wi-Fi and TLS take their share.
* **320×240 SPI display.** A full 16-bit framebuffer would be
  320×240×2 = **150 KB — larger than the biggest block the heap can offer.**
  Never allocate one. Measured full-screen fill is 31.2 ms at 40 MHz SPI, so
  rendering is not the bottleneck and DMA is unnecessary for our workload.
* **CH340 serial bridge, unreliable above 115200 baud.** Pinned in
  `platformio.ini`.
* Touch IRQ on an RTC-capable pin ⇒ wake-on-touch from deep sleep is possible.
* LDR on ADC1 ⇒ ambient light readable even with Wi-Fi active.

### The two constraints that shape everything

1. **No PSRAM.** We cannot buffer a whole API response in RAM, and we cannot
   hold a framebuffer. Both are solved by streaming (§5, §7).
2. **100 API requests per day.** Everything is cache-first and budgeted (§6).

---

## 3. Software stack

| Concern | Choice | Rationale |
|---|---|---|
| Build system | PlatformIO, Arduino framework | Already installed locally (Core 6.1.19, espressif32 6.13.0 ⇒ Arduino 2.0.17) with the Xtensa toolchain, so builds work offline |
| Display driver | **TFT_eSPI** with SPI DMA | The factory firmware uses exactly this on this board with DMA — known-good. Faster and far leaner than the alternatives |
| UI layer | **Custom lightweight screen manager** (no LVGL) | See decision below |
| Touch | XPT2046 on its own SPI pins, IRQ-driven | No CS contention with the panel |
| Filesystem | **LittleFS** (not SPIFFS) | Power-fail safe, wear-levelled, real directories, faster on small files. SPIFFS is deprecated and corrupts on brownout |
| Secrets | **NVS** via `Preferences` | Survives a filesystem format; keeps credentials out of the cache area and out of git |
| JSON | **ArduinoJson v7** with streaming + deserialisation filters | The only way to parse a 100 KB standings response in <200 KB RAM (§5) |
| Web server | Built-in synchronous `WebServer` | Config UI is low-traffic; async adds flash and a second task's stack for no benefit here. Revisit if OTA upload proves slow |
| Data providers | **Two free APIs behind a provider abstraction** | Neither free tier alone covers the brief; their gaps are complementary (§6) |
| Time | NTP + POSIX TZ | Needed for match countdowns and the midnight GMT quota reset |
| Discovery | mDNS | Reach the device at a name instead of hunting for its IP |

### Decision: no LVGL

The factory demo ships LVGL 8.3.3, and it is tempting to reuse. Rejected because:

* LVGL costs roughly **100–150 KB of flash** and a meaningful chunk of our very
  scarce RAM for its own draw buffers and object tree.
* Our UI is **seven mostly-static, text-and-table screens**. We need no
  animation engine, no layout solver, no widget hierarchy.
* Direct TFT_eSPI drawing with a small reusable sprite gives us precise control
  over exactly which pixels get pushed — which is also the *power* win (§9),
  since SPI traffic and CPU wakefulness are what cost us battery.

We accept writing our own scrolling for the league table (§4.5) as the price.
**Revisit if** we later want rich animation or many more screens.

---

## 4. Screens

Screens auto-cycle on a configurable dwell (default 12 s). Touch takes over:
swipe left/right to move between screens, tap to pin the current screen and
pause cycling, tap again to resume. A screen with no valid data is skipped in
the rotation rather than shown empty.

| # | Screen | Data source | Notes |
|---|---|---|---|
| 1 | **Live match** | api-sports `live=all` | Only in rotation while a match is in progress. Score, minute, scorers, cards |
| 2 | **Season record** | *derived from the table* | W/D/L for the season — costs no extra request |
| 3 | **Last result** | football-data `teams/{id}/matches` | Opponent, score, competition, date |
| 4 | **Next fixture** | football-data `teams/60/matches` | Opponent, kick-off, countdown, **both clubs' recent form** (§4.8) |
| 5 | **League table** | football-data `standings` | Scrollable — see below |
| 6 | **Top scorer** | football-data `scorers` | **Our team's scorers**, with the league leader as context (§4.6) |
| 7 | ~~Injuries~~ | — | **Deferred** — unavailable on either free tier (§6) |

### 4.5 League table rendering

The full table is 20 rows × 9 columns (Club, MP, W, D, L, GF, GA, GD, Pts) on a
320×240 screen — about 8 rows fit legibly at once.

* Drawn into a **single reusable 320×24 sprite** (15 KB) blitted row by row,
  rather than a full framebuffer. Costs 15 KB instead of 150 KB.
* Vertical drag to scroll; our team's row is highlighted and the view opens
  centred on it. Note the digitizer's axes are transposed relative to the
  display (see [docs/HARDWARE.md](docs/HARDWARE.md)) — the touch layer resolves
  this, so screens work in plain screen coordinates.
* Club names use the **official `tla` 3-letter code** supplied by
  football-data.org (`BOL`, `WHU`, `QPR`) — no abbreviation logic needed — with
  the full name shown for the highlighted row, so the numeric columns stay
  aligned and readable.
* The Championship has **24 teams**, so about 3 screens' worth of scrolling
  at 8 visible rows.

**Planned: proportional auto-scroll while cycling** (requested; not urgent).
The table opens centred on our team, which is right, but the rest of the table
is then never seen by someone who does not touch the device. The idea is to
pan automatically within the dwell period: from our team's row to the nearer
end of the table, then across to the far end, with the rate derived from the
row count and the configured dwell so the whole table is covered exactly once
however long the dwell is set to. It must pause the moment the screen is
touched, so a manual scroll is never fought by the animation.

One implementation note for when this is built: at 31 ms per full redraw a
smooth per-pixel pan is affordable, but redrawing the whole table for every
pixel of travel would keep the CPU and SPI bus busy continuously, which works
against the power plan (§9). Stepping a whole row at a time, on a timer derived
from the dwell, gets the same coverage for a fraction of the work.

---

### 4.5b Live match takes priority (requested)

When a match involving our team is in progress, the live screen stops being one
screen among several and becomes the default the device returns to.

**Required behaviour:**

1. **Live is shown by default** the moment a match involving our team goes
   in-play — not on the next rotation, immediately.
2. **Swiping away is temporary.** Any other screen may be viewed, but once the
   dwell period elapses the device returns to Live rather than continuing round
   the carousel. This is a deliberate exception to the normal rule that a
   deliberate swipe pins the rotation: with a match on, "go back to the match"
   is almost always what is wanted.
3. **An explicit tap-hold still wins.** Someone who taps to hold a screen has
   said so unambiguously, and that must not be overridden — otherwise the
   device fights the user.
4. **Next fixture skips the live match.** While a match is in progress, the
   Next screen shows the *following* scheduled fixture, not the one being
   played. Otherwise two screens show the same match and the genuinely useful
   information — what is coming up — is lost.
5. **The form guide gains a provisional chip** for the match in progress,
   reflecting the current state (winning/drawing/losing), marked as provisional
   so it is not mistaken for a settled result.
6. **Everything reverts once the match is confirmed over.** Not when the clock
   passes 90 — matches have stoppage time, and a result can change in it. The
   trigger is the provider reporting the match finished, at which point the
   provisional chip becomes a real result, Next advances, and normal rotation
   resumes.

Point 6 is the one with a trap in it: deciding a match is over from elapsed
minutes would settle a result while a goal could still change it.

### 4.5c Live match detail (requested)

The live screen shows the match narrative, not just the score:

| Shown | Source |
|---|---|
| Score and clock | `goals`, `fixture.status.elapsed` |
| **Goalscorers with minute** | `events[]` where type is `Goal` |
| Penalties and own goals distinguished | `events[].detail` |
| **Yellow and red cards with minute** | `events[]` where type is `Card` |
| Half-time / full-time state | `fixture.status.short` |

**Verified available.** `/fixtures?live=all` returns an `events` array per
fixture with `time.elapsed`, `type` (`Goal`, `Card`, `subst`), `detail`
(`Yellow Card`, `Red Card`, `Normal Goal`, `Penalty`, `Own Goal`), `player`,
`assist` and `team`.

**Layout:** two columns, home left and away right, each listing that team's
events in chronological order — so the shape of the match reads at a glance
without having to parse which side each line belongs to.

**Substitutions are deliberately excluded.** They are available, but on a
320×240 panel screen space is the scarce resource, and a substitution tells a
casual viewer far less than a goal or a card. Goals and cards only, capped at
what fits, oldest first so the most recent is nearest the score.

### 4.6 Top scorers — our team, not just the league

The league scoring chart is identical for every user of every device, and a
side in the bottom half never appears in it. Our own team's scorers are the
interesting view.

**Verified achievable on the free tier.** `/competitions/ELC/scorers?limit=100`
returns 100 entries — the list bottoms out at a single goal, so *everyone who
has scored* is included, and our players can be filtered out locally. For Bolton
that yields Sam Dalby (2), Thierry Gale (1) and Xavier Simons (1), none of whom
come close to the league top ten.

So **one request serves both views**: our team's list as the main content, and
the league leader as a single line of context. The screen labels which list it
is showing, because two goals reads as a plausible team-leading tally and as
nonsense for a league-leading one.

### 4.8 Form guide

Requested: our team's form over the last five games, and the opponent's too.

**The API's own `form` field is unusable on the free tier.** It is present in
the standings schema and `null` for every team — so form is *derived on-device*
from the finished-matches response instead.

That turns out to be the better route anyway:

* **Our team's form costs no extra request.** It comes from the same
  `/teams/60/matches?status=FINISHED` call that already supplies the last
  result. Two screens and the form guide from one request.
* **The opponent's costs one additional request**, and only when the next
  fixture changes — roughly once a matchday.
* Deriving it ourselves means we control the definition: last five *completed*
  matches, and we can later choose whether cup games count.

Correctness was checked against the league table, which is the useful test:
Bolton derived to `W D L L L L` across six games, matching their W1 D1 L4 row,
and Cardiff to `D D D L L D` matching W0 D4 L2.

**Presentation.** Five rounded chips, green/amber/red, reusing the same colour
language as the result screens so a glance means the same thing everywhere.
Chips read **left to right in chronological order**, and the most recent is
outlined — which resolves the ordering ambiguity without spending a line on a
caption. Shown for both clubs on the Next fixture screen, and for our own team
on the Season screen.

Form is stored on the fixture as `homeForm`/`awayForm`, five characters plus a
terminator, rather than per table row — because it is only derivable for clubs
we specifically fetch, not for all 24.

### 4.9 A finding about `limit` ordering

Worth recording, since it contradicts an earlier caution in this document.
`/teams/{id}/matches?status=FINISHED&limit=N` selects the **most recent N**
matches and returns them **oldest first**. That is why `limit=1` earlier
returned matchday 6 rather than matchday 1.

So the last result is the *final* element of the response, not the first — and
form is the tail of that same list, which is exactly the order the chips are
drawn in.

### 4.7 Team crests — as built

A crest is what makes the device feel like *your* team's rather than a generic
data display.

**Only the crests currently in use are kept**, which is the design that matters
once the team is user-chosen: caching a whole division would be pointless and
wrong, since the interesting set is tiny and moves every matchday.

| Kept | Why |
|---|---|
| Our team | Always relevant |
| Last opponent | The Last result screen |
| Next opponent | The Next fixture screen |
| Current opponent | The Live match screen, when one is on |

At most four, usually three. Everything else is pruned, so the cache is
**~14 KB regardless of which league the user follows** — measured at 13,824
bytes for three crests.

**Crests cost no API quota.** `crests.football-data.org` is a static host, not
the rate-limited API, and needs no key. It chains to ISRG Root X1, so the
existing certificate bundle already covers it.

#### Corrected assumption: crests are not one size

The original plan recorded crests as 70×70, measured from Bolton's. **They
vary considerably** — Bolton's is 70×70 at 7,736 bytes with alpha; West Ham's
and Cardiff's are 200×200 (6,859 and 16,928 bytes) without. A decode buffer
sized from the first crest inspected fails silently on every larger one, so the
line buffer is allocated from the width in the file, capped at 512 px.

#### Pipeline

1. Download to a heap buffer sized from `Content-Length` (this host does not
   chunk), verifying the PNG signature so an error page is distinguishable from
   a decoder fault.
2. Decode with PNGdec line-by-line, **box-filtering** down to 48×48. Area
   averaging rather than nearest-neighbour: a crest is fine detail and mostly
   lettering, and at a 200→48 reduction nearest-neighbour looks broken. The
   accumulators cost a few hundred bytes.
3. Composite alpha against the UI background at decode time, so the stored
   form is opaque and drawing needs no blending.
4. Write to a temporary and rename, as the JSON cache does — a reset mid-decode
   must not leave a half-image that would then be drawn as noise.
5. Draw by reading **one row at a time** into a 96-byte buffer, so the UI never
   holds the 4.6 KB image.

The decoder is **heap-allocated only while decoding**. PNGdec's `PNG` object
embeds its inflate window, and as a static member it cost ~46 KB of DRAM
permanently — measured as a jump from 16.6% to 30.9% — for something used once
per crest, while TLS wants ~42 KB of heap at the same time.

Crests appear on Last result, Next fixture (both clubs) and Season (ours). The
league table stays text-only: 24 icons at 19 px per row would be unreadable.

## 5. Fetching and caching

**Rule R2 in practice: every screen reads from cache. Only the refresh
scheduler talks to the network.** A screen never blocks on HTTP.

### The streaming problem

A `/standings` response for a 20-team league is roughly **100 KB of JSON** —
we cannot hold that, let alone parse it, in ~180 KB of free RAM alongside TLS
buffers (TLS alone wants ~40 KB).

Solution, applied to every endpoint:

1. Stream the HTTPS response **directly from the client into ArduinoJson**, so
   the raw body is never fully resident.
2. Apply an **ArduinoJson deserialisation filter** so only the handful of fields
   we actually display are materialised. For standings that reduces ~100 KB of
   JSON to well under 2 KB of objects.
3. **Re-serialise the filtered result to LittleFS** as compact JSON. That file is
   the cache — small by construction (R3).

This is the single most important implementation detail in the project. It is
what makes a no-PSRAM board viable against this API.

### Measured cache reduction

Filtering before storing is not a marginal saving. Observed on hardware:

| Document | Response | Stored |
|---|---|---|
| League table | 6,728 B | 4,100 B |
| Our matches | 6,269 B | 1,637 B |
| Next fixture | 3,087 B | 822 B |
| **Top scorers** | **62,044 B** | **11,165 B** |

The whole cache is about 18 KB of the 1408 KB filesystem. The scorers response
is the one that proves the approach: 62 KB streamed through a filter on a board
whose largest contiguous heap block is 110 KB, with heap steady at 174 KB
throughout and back to 224 KB afterwards.

**A reboot costs zero API calls.** The cache repopulates the screens before the
radio has even associated, and the schedule is then aligned to the cached
timestamps so nothing still inside its TTL is re-requested.

### Cache format — as built

One file per data type under `/cache/`, plus a single shared metadata file:

```
/cache/standings.json     — filtered payload, compact, short keys
/cache/team_matches.json  — serves last result AND both form guides
/cache/opp_matches.json
/cache/scorers.json
/cache/live.json
/cache/meta.bin           — fetch time and TTL per document, 8 bytes each
```

Metadata is **one small binary file, not a sidecar per document**: five
documents would otherwise mean five extra files and five extra directory
entries to hold what amounts to 40 bytes. It is mirrored in RAM at mount, so a
freshness check touches no filesystem at all — which matters because the
screen rotation asks about freshness constantly while data is fetched rarely.

* **Compact, never pretty-printed**; short keys (`p` not `points`).
* `t` is the fetch time as a Unix timestamp; freshness is `now - t < ttl`.
* Written **atomically** — write `foo.json.tmp`, flush, close, then rename —
  so a reset or brownout mid-write leaves either the previous document or
  none, never a truncated one. This matters more than it looks: cache is
  written immediately after a network fetch, which is exactly when current
  draw peaks and a marginal supply is most likely to sag.
* A **short write** removes the temporary file, leaving the previous good
  document in place — the entire reason for writing to a temporary first.
* A document **too large for the caller's buffer is refused, not truncated**.
  A truncated JSON document does not fail cleanly at the parser; it fails
  confusingly, and looks like corrupt data from the API rather than a buffer
  that was too small.
* **Freshness needs a clock.** Before NTP, `statusOf()` reports *stale* rather
  than guessing, so cached data is still served but a refresh is attempted when
  possible — the safe direction to err in. Timestamps of 0 mean "unknown"
  explicitly, so a pre-NTP stamp can never make a document look absurdly
  fresh once the clock jumps.
* Cache survives reboots, so a restart costs **zero** API calls.
* On a parse failure the file is deleted and the entry marked stale, so one bad
  write cannot wedge a screen permanently.

### Refresh policy

| Data | TTL | Reasoning |
|---|---|---|
| Live match | 90 s – 5 min, adaptive (§6) | Only while a match is in progress |
| Standings | 6 h, and only if a match has been played since last fetch | The table cannot change otherwise, so a timer alone would waste calls |
| Next fixture | 12 h | Fixtures move rarely, and we hold the whole list |
| Last result | After a known fixture ends | Event-driven, not polled |
| Season record | 24 h | |
| Top scorer | 24 h | |
| Injuries | 24 h | |

**Fixture-list-driven scheduling.** One `/fixtures?team=&season=` call gives us
the entire season's fixture list, cached long-term. From it the device knows —
at **zero** API cost — whether a match is on today, when kick-off is, and
therefore whether to enable live polling at all. This is what keeps idle days
down to a handful of calls.

---

## 6. API budget (Rule R7)

**Endpoint:** `https://v3.football.api-sports.io`
**Auth:** `x-apisports-key` header.
**Plan verified against the live account:** Free, active to 2027-03-10.

### Verified limits

Two independent limits, both confirmed from live response headers:

| Header | Meaning |
|---|---|
| `x-ratelimit-requests-limit: 100` | **100 requests per day**, resets midnight UTC |
| `x-ratelimit-requests-remaining: 99` | Daily allowance left |
| `x-ratelimit-limit: 10` | **10 requests per minute** |
| `x-ratelimit-remaining: 9` | Per-minute allowance left |

The **per-minute limit of 10 was not in the original brief** and must also be
respected; our polling is far below it, but a burst of user-triggered refreshes
could hit it, so the client rate-limits itself to a minimum spacing between
calls.

### The server is the source of truth

Because the API reports remaining quota on **every response**, the device does
not rely solely on its own tally. It reads the headers back and trusts them.
This self-heals the cases a local counter gets wrong: requests that failed after
being counted, clock drift across the midnight reset, calls made by another
client on the same key, or a counter lost to an unexpected reboot.

A local count in NVS remains as the **pre-flight** check — we must decide whether
we can afford a call *before* making it — but it is corrected to the server's
figure after every response.

### ⚠️ Verified coverage — the free tier is far more restricted than documented

**This supersedes an earlier, incorrect entry in this spec.** The `/leagues`
endpoint happily reports seasons 2010–2026 with full coverage flags for
`standings`, `top_scorers` and `injuries`. That metadata describes seasons that
**exist**, not seasons the plan may **query**. Probing the actual data endpoints
tells a very different story.

Probe results against the live account (2026-09-09):

| Request | Result |
|---|---|
| `/leagues?id=39` | ✅ lists seasons 2010–2026 — **misleading** |
| `/standings?league=40&season=2024` | ✅ works |
| `/players/topscorers?league=40&season=2024` | ✅ works, 20 rows |
| `/injuries?team=68&season=2024` | ✅ endpoint allowed, but **0 rows** for a Championship side |
| `/fixtures?live=all` | ✅ **works, returns present-day live matches** |
| `/standings?league=40&season=2026` | ❌ `Free plans do not have access to this season, try from 2022 to 2024` |
| `/leagues?team=68&season=2026` | ❌ same season block |
| `/fixtures?team=68&next=1` | ❌ `Free plans do not have access to the Next parameter` |
| `/fixtures?team=68&season=2024&last=3` | ❌ `Free plans do not have access to the Last parameter` |
| `/fixtures?team=68&date=2026-09-13` | ❌ `Free plans do not have access to this date, try from 2026-09-08 to 2026-09-10` |
| `/fixtures?team=68` | ❌ `The Season field is required` |

So the free tier's real shape is:

* **Season-parameter queries are locked to 2022–2024.** The current season
  (2026) is unreachable for standings, top scorers and team statistics.
* **The `next` and `last` parameters are blocked entirely**, so "next fixture"
  and "last result" cannot be asked for directly at any season.
* **Date queries are clamped to a ±1 day window around today.** We can ask about
  yesterday, today and tomorrow — nothing further out.
* **But `live=all` works and returns genuinely current in-play matches.**

### What this means for the screens

| Screen | Free tier, current season |
|---|---|
| Live match | ✅ available via `live=all` |
| Next fixture | ⚠️ only if it is today or tomorrow |
| Last result | ⚠️ only if it was yesterday or today |
| League table | ❌ current season blocked (2024 available) |
| Season record | ❌ current season blocked |
| Top scorer | ❌ current season blocked (2024 available) |
| Injuries | ❌ no Championship data even on allowed seasons |

The irony is neat and useful: **api-sports gives away the live data that most
providers charge for, and withholds the static tables that most providers give
away.** That points directly at a hybrid (see below).

### Decision: hybrid across two free providers

**Decided.** Neither free tier alone covers the brief, but their gaps are almost
perfectly complementary, so we use both behind a single abstraction.

| Provider | Supplies | Auth header | Limits |
|---|---|---|---|
| **api-sports v3** | Live match only — score, minute, goalscorers, cards | `x-apisports-key` | 100/day, 10/min |
| **football-data.org v4** | Current-season league table, fixture schedule, season record, top scorer | `X-Auth-Token` | 10/min, **no documented daily cap** |

Rationale: api-sports uniquely gives away current live data on its free tier,
which football-data.org charges for (its free scores are *delayed*).
football-data.org uniquely gives away current-season tables and full fixture
lists, which api-sports blocks below 2022–2024. Using each for its strength
costs one extra free registration and gets every screen but injuries onto
current-season data.

**A free registered key is mandatory for football-data.org.** Anonymous access
was tested and returns `403 restricted` for `/competitions/ELC`,
`/competitions/ELC/standings` *and* `/competitions/PL/standings` — the anonymous
tier's 50 requests/day are useless for our purposes. Register at
<https://www.football-data.org/client/register>.

### ✅ Hybrid verified end to end (2026-09-09)

All of the above is confirmed against the live account, not assumed:

| Check | Result |
|---|---|
| Key authenticates | ✅ `X-Authenticated-Client: Chris` |
| Championship on free tier | ✅ `ELC` → "Championship", current season 2026-08-14 → 2027-05-01, matchday 6 |
| `/competitions/ELC/standings` | ✅ **24 rows** in just **6.7 KB** |
| `/teams/60/matches?status=SCHEDULED&limit=1` | ✅ Bolton v Cardiff, 2026-09-12 11:30 UTC, matchday 7 — 1.2 KB |
| `/teams/60/matches?status=FINISHED&limit=1` | ✅ Bolton 2-3 West Ham, 2026-09-08, `winner: AWAY_TEAM` — 1.3 KB |
| `/competitions/ELC/scorers?limit=3` | ✅ works — 2.2 KB |
| Rate headers | `x-requests-available-minute`, `X-RequestCounter-Reset` (60 s) |

### 🚩 The two providers use different team id spaces

**Bolton Wanderers is `68` on api-sports and `60` on football-data.org.**

Worse, **football-data.org id `68` is Norwich City** — so transposing the two ids
does not fail loudly, it silently displays another club's data. Mitigations:

* Ids are stored under provider-qualified NVS keys (`team.apisports.id`,
  `team.footballdata.id`), never a single `team.id`.
* The provider interface takes its own id type, so the compiler helps.
* Each provider's client asserts the team name it gets back matches the
  configured team, and raises a config error rather than displaying wrong data.

### Free-tier data quirks worth coding around

* **`status=SCHEDULED` returns matches whose actual status is `TIMED`.** Do not
  filter on `status == "SCHEDULED"` client-side or the next fixture vanishes.
* **`assists` is `null`** on the scorers endpoint — that field is behind the paid
  "Deep Data" add-on. The top scorer screen shows goals and appearances only.
* **`limit=1` ordering is not documented.** The finished-matches probe returned
  matchday 6 (the most recent) rather than matchday 1, so it appears to favour
  the latest — but we do not rely on it: fetch a small window with
  `dateFrom`/`dateTo` and sort locally.
* Payloads are **1–7 KB**, an order of magnitude smaller than api-sports'
  ~100 KB standings. The streaming-and-filtering machinery (§5) is therefore
  critical only for api-sports' `live=all` response; football-data responses fit
  comfortably in RAM. We keep the streaming path for both anyway — it costs
  nothing to reuse and removes a class of failure.
* **The `tla` field gives an official 3-letter code per club** (`BOL`, `WHU`,
  `QPR`). This is exactly what the league table screen needs for its abbreviated
  club column, so we do **not** need to generate abbreviations ourselves (§4.5).

### The Championship has 24 teams, not 20

Noted because it changes the table screen: 24 rows to scroll rather than 20, and
Bolton currently sit 22nd, so **opening the table centred on our team matters
more than it would for a mid-table side** — a naive top-of-table render would
show a Bolton fan nothing they care about.

### Provider abstraction

Screens never know which provider served them. A thin interface sits behind the
cache so a provider can be swapped, or a paid key dropped in, without touching
any screen code:

```
Screens ─→ Cache (LittleFS) ─→ Provider interface
                                 ├── ApiSports        live match
                                 └── FootballDataOrg  table, fixtures, scorers
```

This is deliberate insurance. Both free tiers have already proven their
documentation unreliable, so we assume either could change under us.

### Endpoint map

| Screen | Provider | Request |
|---|---|---|
| Live match | api-sports | `/fixtures?live=all`, filtered to our team locally |
| League table | football-data | `/competitions/ELC/standings` |
| Season record | football-data | **free — reuses the standings response** (see below) |
| Next fixture | football-data | `/teams/60/matches?status=SCHEDULED` |
| Last result | football-data | `/teams/60/matches?status=FINISHED` |
| Top scorer | football-data | `/competitions/ELC/scorers` (current season only) |
| Injuries | — | **Not available on either free tier** (§ below) |

**Season record costs zero extra calls.** The standings response already carries
`playedGames`, `won`, `draw`, `lost`, `goalsFor`, `goalsAgainst`,
`goalDifference` and `points` per team — so screen 2 is derived from our team's
row in the table we already fetched for screen 5. One request serves two screens.

### Injuries: deferred

`/injuries?team=68&season=2024` on api-sports returns **0 rows** for a
Championship side, and football-data.org's free tier has no injuries endpoint at
all. The screen is **deferred rather than dropped**: the screen manager already
skips any screen with no valid data (§4), so the feature can appear later behind
a paid plan with no rework. Removed from the near-term roadmap.

### Revised budget

Splitting the load transforms the api-sports picture, because it now serves
*only* the live screen:

| Scenario | api-sports (of 100/day) | football-data (10/min, no daily cap) |
|---|---|---|
| Idle day, no match | **0** | ~6 — table 4, fixtures 2 |
| Match day | **~40** — adaptive polling per §6 | ~8 |

The 100/day limit stops being the binding constraint. All the rationing
machinery in §6 stays — it now protects a budget we comfortably fit inside,
which is the right place to be. The per-minute limits become the live concern,
so the client enforces minimum call spacing per provider.

### `/status` under-reports — trust the headers

`/status` reported `2 / 100` used after six successful billable calls, so it
appears to be cached or lagging server-side. The per-response
`x-ratelimit-requests-remaining` header tracked correctly throughout.

**Therefore: reconcile the local counter from response headers, and treat
`/status` as advisory only** — useful for plan detection on boot, not for
budget enforcement. This reverses the reconciliation source implied earlier.

### Daily budget

Target: **stay under 100/day with real headroom**, never exhaust the quota.

| Scenario | Calls | Breakdown |
|---|---|---|
| Idle day (no match) | **~5** | standings 1, fixtures 1, season record 1, top scorer 1, injuries 1 |
| Match day | **~60** | the 5 above + adaptive live polling |

Adaptive live polling, so a 2-hour match does not eat the day:

| Match phase | Interval | Why |
|---|---|---|
| Pre-kick-off (15 min before) | 5 min | Detect actual kick-off, confirm lineups |
| In play, quiet | 3 min | ~35 calls across 105 min |
| In play, within 3 min of a goal | 90 s | Catch the follow-up and any VAR reversal |
| Final 10 min + stoppage | 90 s | Where matches are decided |
| Half-time | 10 min | Nothing happens; do not waste calls |
| Full-time | stop, one final fetch | Settles the result, then live screen leaves rotation |

**Hard guards:**

* **Reserve floor of 10 calls.** Automatic refreshes stop when fewer than 10
  remain for the day, keeping the reserve for user-triggered refresh from the
  web UI. Below the floor, screens serve stale cache with a visible "stale"
  marker rather than going blank.
* **Minimum spacing** between any two calls, to respect the 10/min limit.
* Every call is **logged with endpoint, timestamp and outcome** to a rolling
  counter surfaced in the web UI (§7), so the budget is auditable.
* `/status` is used for reconciliation on boot — it reports plan and usage and
  **does not itself count** against the daily quota (verified).

---

## 6b. On-device settings menu

Opened by a **long press** (two seconds). A modal mode, not a screen in the
carousel: while it is open the rotation is suspended and touch means "press a
button" rather than "navigate".

**Scope is deliberately narrow.** Anything requiring typing belongs in the web
interface — a resistive panel is a poor keyboard — so this offers only what has
to work when the web UI *cannot* be reached:

| Page | Purpose |
|---|---|
| **Device info** | Read-only: network, **IP address**, web UI address, signal, team, clock, API calls today, free memory |
| **How to use** | On-device gesture help, for someone who has never used it |
| **Reset Wi-Fi** | Recovers a device joined to a network that no longer exists |
| **Factory reset** | Erases credentials, settings and cache |
| **Back to screens** | Returns to the carousel |

The address on the Device info page is the point of the whole menu: it is how
the user gets to the web interface to change anything substantial.

**Both resets are confirmed** on a dedicated page with "Yes, do it" / "Go back".
The safe option is placed *below* the destructive one, so a stray tap lands on
the harmless choice.

**Every page has more than one way out**, because being stranded in a menu on a
device with no buttons would be unrecoverable short of a power cycle:
a Back button, a right-swipe (matching the carousel's own navigation), and a
long press anywhere — the same gesture that opened it.

Destructive actions are returned to the caller as an enum rather than executed
inside the menu, so the menu stays a pure UI component with no power to reboot
the device by itself.

## 7. Web interface

Served from the device; reachable by mDNS. Purpose: configuration, and honest
visibility into the API budget.

### Pages

| Page | Contents |
|---|---|
| **Dashboard** | Today's API usage vs limit, remaining quota, next scheduled refresh, cache freshness per data type, uptime, free heap, Wi-Fi RSSI |
| **Team & league** | Team search (by name, via API) and league/season selection |
| **Screens** | Enable/disable and reorder screens; **set the cycle dwell time** (requested); brightness and auto-brightness |
| **Wi-Fi** | Network scan and credential entry |
| **Cache** | Inspect each cached document, its age and size; force refresh (spends quota, with a confirmation) or clear |
| **System** | Device status, guarded Wi-Fi reset, guarded factory reset. OTA to follow |

**API keys are never echoed back to the browser.** The form shows only whether
each key is set; a blank field means "leave unchanged". There is no legitimate
reason for the page to carry a secret the device already holds, and blank-means-
unchanged gives the same editing experience without it.

**Destructive actions are returned to the caller, not performed in the web
layer** — the same separation the on-device menu uses, so no page handler can
reboot or wipe the device by itself. Both also confirm in the browser and are
POST-only, so a crawler or prefetching browser cannot trigger one by following
a link.

**Settings apply without a restart** wherever possible: brightness, dwell,
screen selection and API keys all take effect immediately. Changing the team or
competition *clears* the cache rather than letting it expire, because that data
is then wrong rather than merely stale.

### Styling — Pure CSS, embedded gzipped (decision revised)

**Measured, then decided.** `pure-min.css` is **15,721 bytes raw and 3,583
bytes gzipped**. The individual modules we would need (base, grids, forms,
buttons, menus) come to about 4,500 bytes gzipped — *more* than the complete
file, which compresses better as one unit. So the whole of Pure is cheaper than
a subset of it.

The original plan was to download and cache it at runtime. **That is now
reversed: it is embedded in flash, gzipped, and served with
`Content-Encoding: gzip`.** Reasons, in order of weight:

1. **The AP setup portal has no internet.** The page whose entire job is to
   obtain Wi-Fi credentials cannot download its own stylesheet. A download
   strategy is structurally unable to work in the one situation where first
   impressions are formed — so an embedded copy is required *regardless*, at
   which point a second, downloaded copy earns nothing.
2. **3.5 KB is not a saving worth engineering for.** Against ~900 KB free in
   the app partition, the download path would cost more in code — fetch,
   validate, cache, invalidate, fall back — than the asset itself.
3. **No decompression on-device.** Served with `Content-Encoding: gzip`, the
   browser inflates it. The ESP32 streams 3.5 KB straight from flash with no
   RAM buffer and no CPU cost.
4. Pure is a pinned version. There is nothing to keep fresh.

This is consistent with the original preference, which allowed baking assets in
where there is "plenty of left-over space" — there is. Rule R2 (cache what we
pull) is unaffected: the best version of caching a static asset is not pulling
it at all.

Generated by `tools/gen_pure_css.py` into a header, so the embedded bytes are
reproducible rather than a mystery blob.

### First-boot provisioning

1. No credentials in NVS ⇒ start a **SoftAP** (`FootballTracker-XXXX`), blue LED on.
2. **Captive portal** redirects any request to the setup page.
3. The **display shows the AP name, password and portal URL** — the screen is
   right there, so the user never has to guess.
4. User picks a network from a scan, enters the password, chooses their team.
5. Credentials saved to NVS, device reboots into station mode.
6. Long-press BOOT (GPIO0) at any time to clear credentials and return here.

---

## 8. Hardware feature use (Rule R6)

Beyond the display and touch, the board's extras all earn a job:

| Feature | Use |
|---|---|
| **LDR** (GPIO34, ADC1) | Auto-brightness — backlight PWM tracks ambient light, with hysteresis and slew-limiting so it never visibly flickers |
| **RGB LED** (4/16/17) | Green pulse on a goal for us, red on a goal against, blue steady in AP/config mode, red blink on error |
| **Speaker** (GPIO26, DAC) | Goal chime and kick-off alert. Off by default; opt-in from the web UI, because a device that beeps unbidden is a device that gets unplugged |
| **SD card slot** | Optional: season-long match archive and team crest storage, keeping flash free. Absent card degrades gracefully |
| **Touch IRQ** (GPIO36, RTC) | Wake-on-touch from deep sleep (§9) |
| **SPI DMA** | Display pushes overlap computation instead of blocking, as the factory firmware demonstrates |
| **Dual core** | Networking and JSON parsing pinned to core 0, display and touch to core 1, so a slow HTTPS fetch never stutters the UI |
| **BOOT button** (GPIO0) | Long-press factory reset |
| **RTC memory** | Carry small state across deep sleep without touching flash |

---

## 9. Power optimisation (Rule R5)

### Measured on-device, because a meter would not fit

An inline USB meter is the obvious instrument, but this board's socket is
micro-USB and the available meter is USB-C, so absolute figures are
unavailable. That matters less than it sounds: what validates an optimisation
is the *relative* change, and the chip can measure the things that determine
its own consumption.

Three quantities dominate, in order: **backlight duty**, **radio time**, and
**CPU busy fraction**. All three are tracked, and combined into a relative cost
index with stated weights (`src/power.cpp`). **The index is not milliamps** and
the code says so — only comparisons between two runs of this firmware mean
anything, which is exactly what is needed to show whether a change helped.
The weights are named constants so anyone who does get a meter onto this board
can correct them from measurements.

### Results

| | Active | Dimmed | Change |
|---|---|---|---|
| CPU busy | 11% | **2%** | −82% |
| Backlight duty | 100% | **15%** | −85% |
| Redraws/min | 17 | **5** | −71% |
| Cost index | ~8,200 | **~950** at steady state | ~8× |

### What produced them

1. **Inactivity dimming** after two minutes, to 15% rather than off. The
   backlight is the largest single consumer, and this is the cheapest real
   saving available. Not off, because a display you glance at should stay
   readable — and a dark-but-visible panel draws a fraction of a bright one.
   The configured brightness acts as a ceiling, so dimming never brightens.
2. **A longer idle interval while dimmed** — 40 ms instead of 8 ms. A static,
   dimmed screen with nobody watching does not need polling 125 times a
   second. 40 ms is still well under the point where a tap feels delayed.
   This is what took CPU busy from 11% to 2%.
3. **Suppressed live redraws while dimmed.** This one was *found by the
   instrumentation*: the figures showed ~17 redraws a minute while idle, which
   is a countdown ticking once a second onto a panel at 15% brightness that
   nobody can read. Rotation continues, since a dimmed screen is still
   glanceable, but the per-second refreshes stop — 17/min down to 5/min, which
   is the rotation alone.

That third item is the argument for instrumenting before optimising. It was
invisible without measurement, cost nothing to fix, and no amount of reasoning
about the design would have surfaced it.

### Still available, not yet done

* **Wi-Fi modem sleep** is already enabled (`WiFi.setSleep(true)`), but the
  radio could be disconnected entirely between refreshes when the next one is
  hours away.
* **CPU frequency scaling** — 240 MHz is only needed while parsing JSON.
* **Light sleep between screen updates**, waking on the touch IRQ.
* **Deep sleep with the image retained.** The ILI9341 holds its own frame in
  GRAM, so the ESP32 can sleep while the panel keeps displaying the last
  screen with the backlight lit. A "next match" countdown could therefore sit
  on almost nothing overnight. This remains the biggest available win and is
  unique to the panel having its own memory.
* **Configurable quiet hours**, which would also save API calls.

### LDR auto-brightness: dropped

The ambient light sensor reads a flat zero on this unit (see
[docs/HARDWARE.md](docs/HARDWARE.md)), so automatic brightness is not
available. Inactivity dimming — the measure that actually matters — is
unaffected, since it depends on touch rather than light.

## 10. Data layout

```
NVS (20 KB)                    LittleFS (1472 KB)
├── wifi.ssid / wifi.pass      ├── /config.json      screens, dwell, brightness
├── apisports.key              ├── /cache/*.json     filtered API payloads
├── footballdata.key           ├── /cache/meta.json  fetch times and TTLs
├── team.id / league.code      │
├── api.count / api.day        ├── /www/pure-min.css downloaded once
└── screens.mask               └── /crests/*.raw     optional, else SD
```

Secrets in NVS, bulk in LittleFS: a cache wipe or filesystem reformat never
costs the user their Wi-Fi credentials. Settings and the quota tally live in
**separate NVS namespaces**, so clearing the tally can never endanger the
credentials.

Touch calibration is persisted here too, as a single blob rather than seven
keys — it is only ever read and written as a unit, and a partially-written
calibration is meaningless.

**A note on logs.** Arduino's `LittleFS.remove()` *and* `LittleFS.exists()`
both log at ERROR level for a file that is not there, so guarding a remove with
an exists check merely swaps one spurious error line for another. Since
deleting a not-yet-existing file is the normal case on every first write, that
noise appeared on every boot. The store uses POSIX `unlink()` and `stat()` on
the mounted path instead, which are silent. Worth the small ugliness: a log
full of harmless errors is a log nobody reads carefully.

### Partition table

We keep **both OTA slots**. On a sealed device with no debug header, the ability
to roll back a bad firmware is worth more than the ~1.28 MB reclaiming `app1`
would give us — and 1472 KB is already ample for our cache. Filesystem is
reformatted as LittleFS in the same offset the stock table used for SPIFFS.

---

### Flash budget — worth watching

Adding Wi-Fi, `WebServer`, mDNS and the DNS server took the firmware from
**40.5% to 72.4%** of the 1280 KB app partition in a single step, since they
pull in the whole IP stack and mbedTLS. That leaves about **330 KB of
headroom** for the HTTPS client, ArduinoJson, and the PNG decoder for crests.

It should fit, but it is no longer a non-issue, and it retrospectively
justifies two earlier decisions: declining LVGL (100–150 KB) and omitting
TFT_eSPI's smooth fonts. If headroom does run short, reclaiming the `app1` OTA
slot is the escape hatch — at the cost of firmware rollback, which is exactly
the trade we deliberately declined earlier, so it would be a real loss rather
than free space.

## 11. Roadmap

Each item is one branch, per R1.

* [x] Hardware discovery — chip, flash, pinout, partition table, API limits
* [x] PlatformIO scaffold + display bring-up — panel identified as an inverted
      ILI9341 variant; 31 ms full redraw measured
* [x] Touch driver + calibration — hand-rolled XPT2046, IRQ-based press
      detection, **three-point** calibration detecting the transposed axes,
      tap/swipe gestures verified by an in-firmware direction check
* [x] Screen manager and auto-cycling with placeholder data — six screens,
      swipe navigation, scrollable 24-row table, all verified on hardware
* [x] LittleFS, config and cache layer with atomic writes — settings in NVS,
      documents in LittleFS, freshness metadata, on-device self-test passing
* [x] Wi-Fi provisioning — SoftAP + captive portal, on-screen credentials,
      NTP with UK DST, mDNS at `football.local`
* [x] Web interface skeleton — embedded gzipped Pure CSS, setup portal,
      dashboard showing quota and cache state
* [x] On-device settings menu — long press, device info, how-to-use, guarded
      Wi-Fi and factory resets
* [x] Web interface settings pages — **both API keys**, team ids, screens,
      dwell, brightness, cache controls, guarded resets
* [x] Provider abstraction + football-data.org client (table, fixtures, scorers)
* [x] api-sports client — streaming parse, filters, budget enforcement
* [x] Cache persistence — a reboot costs zero API calls
* [x] Live match screen — event columns, priority lock-back, provisional form
* [ ] The six screens wired to real data
* [ ] Live match polling with adaptive scheduling
* [ ] Team crests — streaming PNG decode, permanent LittleFS cache (§4.7)
* [ ] Hardware extras — LDR, RGB LED, speaker
* [ ] Power optimisation phase, with measurements
* [x] OTA updates — web upload, and pull from a published release with
      SHA-256 verification
* [x] Release automation — SemVer from git tags, GitHub Actions build and
      release, manifest devices poll, **stable-only OTA** with pre-releases
      published but never distributed (see [RELEASING.md](RELEASING.md))

## 12. Ideas parked for later

Not committed, recorded so they are not lost:

* Multiple tracked teams, cycling between them
* Match-event history on SD, and a season-long form graph
* Home/away and form-based split records
* Head-to-head record on the "next fixture" screen
* Web push or webhook on goals
* Screensaver showing the team crest
* Tint the UI with the team's `clubColors`, which the API already returns
* BLE as an alternative provisioning path
* Multi-league support for teams in cup competitions

## 13. Open questions

* ~~Panel controller~~ — **resolved at bring-up: ILI9341-compatible but an
  inverted variant**, needing `-D TFT_INVERSION_ON=1`. Geometry and pin map were
  correct as specified. Panel ID read-back is unavailable on this unit (MISO is
  on strapping pin GPIO12), so it was identified visually.
* **Countdown shows `--` until NTP is wired up.** By design: the time
  formatter refuses to guess rather than silently showing a kick-off an hour
  out during BST. Resolves with the network layer.
* **🚩 Light sensor reads zero.** GPIO34 sits at ground with no variance, so the
  LDR is probably not populated on this board. **Auto-brightness (§9 item 1) may
  have to be dropped**; inactivity dimming is unaffected and remains the real
  power win. Needs a torch test to confirm.
* ~~Team choice~~ — **decided: Bolton Wanderers**, api-sports team `id=68`
  (founded 1874, Toughsheet Community Stadium). Note the API returns the name as
  plain `"Bolton"`, so the UI needs a display-name override to show
  "Bolton Wanderers". Overridable in the web UI.
* **Which competitions** — league first, with cups as a toggleable option
  (decided). Cups cost extra API calls, so the toggle defaults to off.
* ~~Data source strategy~~ — **decided: hybrid across two free providers** (§6).
* ~~Needs a football-data.org key~~ — **supplied and verified** (§6).
* ~~Unverified: `ELC`, free Championship coverage, Bolton's id~~ — **all
  confirmed**: `ELC` is correct, the Championship is on the free tier, and
  Bolton is football-data id **60** (api-sports **68**).
* **Touch quality** — resistive panels vary; calibration may need a UI flow.
