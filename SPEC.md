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
* **4 MB flash. No PSRAM.** ~320 KB internal DRAM total; realistically
  **160–200 KB free** once Wi-Fi + TLS are running.
* **320×240 SPI display.** A full 16-bit framebuffer would be
  320×240×2 = **150 KB — more RAM than we can spare.** Never allocate one.
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
| 1 | **Live match** | `/fixtures?id=` | Only in rotation while a match is actually in progress. Score, minute, scorers, cards |
| 2 | **Season record** | `/teams/statistics` | W/D/L rate for the season, plus form string |
| 3 | **Last result** | `/fixtures?team=&last=1` | Opponent, score, competition, date |
| 4 | **Next fixture** | `/fixtures?team=&next=1` | Opponent, competition, kick-off in local time, live countdown |
| 5 | **League table** | `/standings` | Scrollable — see below |
| 6 | **Top scorer** | `/players/topscorers` | Goals, appearances; league-wide and our team's best |
| 7 | **Injuries** | `/injuries` | Player, reason, expected return |

### 4.5 League table rendering

The full table is 20 rows × 9 columns (Club, MP, W, D, L, GF, GA, GD, Pts) on a
320×240 screen — about 8 rows fit legibly at once.

* Drawn into a **single reusable 320×24 sprite** (15 KB) blitted row by row,
  rather than a full framebuffer. Costs 15 KB instead of 150 KB.
* Vertical drag to scroll; our team's row is highlighted and the view opens
  centred on it.
* Club names are abbreviated to a 3-letter code with the full name shown for
  the highlighted row, so the numeric columns stay aligned and readable.

---

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

### Cache format

One file per data type under `/cache/`, plus a sidecar of metadata:

```
/cache/standings.json     — filtered payload, compact, short keys
/cache/meta.json          — { "standings": { "t": 1757433600, "ttl": 21600 }, ... }
```

* **Compact, never pretty-printed**; short keys (`p` not `points`).
* `t` is the fetch time as a Unix timestamp; freshness is `now - t < ttl`.
* Written **atomically** — write `foo.json.tmp`, then rename — so a reset or
  brownout mid-write can never leave a truncated cache that fails to parse.
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

### Verified coverage

Checked against the live API for the Premier League: seasons **2010–2026** are
all accessible on the free plan (the current season is 2026, running
2026-08-21 → 2027-05-30), with coverage flags confirming `standings`,
`top_scorers`, `injuries`, and fixture `events` and `lineups`. **The free tier
imposes no season restriction** — every screen in §4 is supported.

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

## 7. Web interface

Served from the device; reachable by mDNS. Purpose: configuration, and honest
visibility into the API budget.

### Pages

| Page | Contents |
|---|---|
| **Dashboard** | Today's API usage vs limit, remaining quota, next scheduled refresh, cache freshness per data type, uptime, free heap, Wi-Fi RSSI |
| **Team & league** | Team search (by name, via API) and league/season selection |
| **Screens** | Enable/disable and reorder screens; set dwell time; brightness and auto-brightness |
| **Wi-Fi** | Network scan and credential entry |
| **Cache** | Inspect each cached document, its age and size; force refresh (spends quota, with a confirmation) or clear |
| **System** | OTA firmware upload, factory reset, timezone, log |

### Styling — Pure CSS, downloaded and cached

Per preference, [Pure CSS](https://pure-css.github.io/) is **fetched at runtime
and cached to LittleFS**, not baked into flash:

* On first successful internet connection, download `pure-min.css` (~17 KB) to
  `/www/pure-min.css` and serve it locally from then on with a far-future
  `Cache-Control`, so browsers fetch it once.
* **A ~1 KB critical CSS is embedded in flash as a fallback, and this is not
  optional.** During first-boot AP configuration **there is no internet
  connection yet** — the very page whose job is to obtain Wi-Fi credentials
  cannot download its own stylesheet. Without an embedded fallback the setup
  portal would render unstyled at exactly the moment first impressions are
  formed. The fallback also covers a failed download or a wiped filesystem.

We have flash to spare (1472 KB filesystem, 17 KB stylesheet), so this is a
clean win rather than a compromise.

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

Scheduled as a **deliberate phase after the feature set is stable**, so we
measure rather than guess. Planned measures, cheapest-first:

1. **Backlight PWM** — the backlight dominates power draw. Dim on inactivity,
   and cap brightness via the LDR.
2. **Wi-Fi modem sleep** between fetches; disconnect entirely when the next
   refresh is far away. The radio is the second-largest consumer.
3. **CPU frequency scaling** — drop to 80 MHz when idle; 240 MHz is only needed
   while parsing JSON.
4. **Light sleep between screen updates.** A static screen needs no CPU at all.
5. **Deep sleep with the image retained.** The ILI9341 holds its own frame in
   GRAM, so the ESP32 can deep-sleep while the panel keeps displaying the last
   rendered screen with the backlight still lit. A "next match" countdown
   screen can therefore sit on ~nothing overnight, waking on the touch IRQ or a
   timer. This is the biggest available win and is unique to having the panel's
   own memory.
6. **Configurable quiet hours** — long deep sleep overnight, which also saves
   API calls.

Measurement: log `esp_timer` wake durations and instrument with an inline USB
power meter before and after each change, recorded in this document.

---

## 10. Data layout

```
NVS (20 KB)                    LittleFS (1472 KB)
├── wifi.ssid / wifi.pass      ├── /config.json      screens, dwell, brightness
├── api.key                    ├── /cache/*.json     filtered API payloads
├── team.id / league.id        ├── /cache/meta.json  fetch times and TTLs
├── api.count / api.day        ├── /www/pure-min.css downloaded once
└── screens.mask               └── /crests/*.raw     optional, else SD
```

Secrets in NVS, bulk in LittleFS: a cache wipe or filesystem reformat never
costs the user their Wi-Fi credentials.

### Partition table

We keep **both OTA slots**. On a sealed device with no debug header, the ability
to roll back a bad firmware is worth more than the ~1.28 MB reclaiming `app1`
would give us — and 1472 KB is already ample for our cache. Filesystem is
reformatted as LittleFS in the same offset the stock table used for SPIFFS.

---

## 11. Roadmap

Each item is one branch, per R1.

* [x] Hardware discovery — chip, flash, pinout, partition table, API limits
* [ ] PlatformIO scaffold + display bring-up (**confirms ILI9341 vs ST7789**)
* [ ] Touch driver + calibration
* [ ] Screen manager and auto-cycling with placeholder data
* [ ] LittleFS, config and cache layer with atomic writes
* [ ] Wi-Fi provisioning — SoftAP + captive portal
* [ ] Web interface with Pure CSS caching
* [ ] API client — streaming parse, filters, budget enforcement
* [ ] The seven real screens
* [ ] Live match polling with adaptive scheduling
* [ ] Hardware extras — LDR, RGB LED, speaker
* [ ] Power optimisation phase, with measurements
* [ ] OTA updates

## 12. Ideas parked for later

Not committed, recorded so they are not lost:

* Multiple tracked teams, cycling between them
* Match-event history on SD, and a season-long form graph
* Home/away and form-based split records
* Head-to-head record on the "next fixture" screen
* Web push or webhook on goals
* Screensaver showing the team crest
* BLE as an alternative provisioning path
* Multi-league support for teams in cup competitions

## 13. Open questions

* **Panel controller** — ILI9341 assumed; confirmed on first display bring-up.
  See the warning in [docs/HARDWARE.md](docs/HARDWARE.md).
* **Team choice** — a default team needs picking; overridable in the web UI.
* **Which competitions** — league only at first, or cups too? Cups mean extra
  API calls, so this is a budget decision.
* **Touch quality** — resistive panels vary; calibration may need a UI flow.
