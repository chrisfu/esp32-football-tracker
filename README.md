# ESP32 Football Tracker

A football (soccer) statistics display for the ESP32 **"Cheap Yellow Display"**
(2.8" ESP32-2432S028R). It follows one club and cycles through live scores,
results, fixtures, the league table, top scorers and form — configured entirely
from a built-in web interface, with no hard-coded team.

<!-- Add a photo of the device here once you have one. -->

## Contents

- [What it shows](#what-it-shows)
- [What you need](#what-you-need)
- [Getting started](#getting-started)
  - [1. Get API keys](#1-get-api-keys)
  - [2. Flash the firmware](#2-flash-the-firmware)
  - [3. Connect it to Wi-Fi](#3-connect-it-to-wi-fi)
  - [4. Choose your club](#4-choose-your-club)
- [Finding your team's IDs](#finding-your-teams-ids)
- [Supported competitions](#supported-competitions)
- [Using the device](#using-the-device)
- [The web interface](#the-web-interface)
- [Updating](#updating)
- [API usage and limits](#api-usage-and-limits)
- [Building from source](#building-from-source)
- [Tested hardware](#tested-hardware)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [Reporting problems](#reporting-problems)
- [Licence](#licence)

## What it shows

Screens cycle automatically, and any of them can be turned off.

| Screen | Shows |
|---|---|
| **Live** | Score, match clock, goalscorers, cards — updates during the match |
| **Season** | Played, won, drawn, lost, win rate, position, goal difference, form |
| **Last** | Most recent result with both crests, the verdict and the competition |
| **Next** | Next fixture, countdown, both clubs' recent form, both crests |
| **Table** | Full league table (MP, W, D, L, GF, GA, GD, Pts), opening on your club |
| **Scorers** | Your club's top scorers, with the league leader for context |

A match in progress takes over: the device shows it by default and returns to
it after the dwell period, so you are never more than a few seconds from the
score.

## What you need

- An **ESP32-2432S028R** "Cheap Yellow Display" (2.8", resistive touch)
- A USB cable (micro-USB on most boards)
- A 2.4 GHz Wi-Fi network — the ESP32 has no 5 GHz radio
- Two free API keys (below)

## Getting started

### 1. Get API keys

Both are free and take a minute.

| Provider | Used for | Register |
|---|---|---|
| **football-data.org** | Table, fixtures, scorers, form | <https://www.football-data.org/client/register> |
| **api-sports.io** | Live match scores and events | <https://dashboard.api-football.com/register> |

Only football-data.org is strictly required. Without api-sports the Live
screen simply never appears; everything else works.

### 2. Flash the firmware

Download `firmware-<version>.bin` from the
[latest release](../../releases/latest), then either:

**With esptool** (no toolchain needed):

```bash
pip install esptool
esptool --port /dev/ttyUSB0 --baud 115200 write-flash 0x10000 firmware-0.1.0.bin
```

**Or from source** — see [Building from source](#building-from-source).

> **Keep the baud rate at 115200.** The CH340 bridge fitted to these boards is
> not reliable above that, and a faster upload can corrupt the flash silently.

Each release also ships `firmware-<version>.bin.sha` containing the SHA-256 of
the binary, so you can check what you downloaded:

```bash
shasum -a 256 -c firmware-0.1.0.bin.sha
```

### 3. Connect it to Wi-Fi

On first boot the device has no credentials, so it starts its own access
point and shows you everything you need **on the screen**:

```
        SETUP REQUIRED
   Join this Wi-Fi network:
   NETWORK    FootballTracker-A1B2
   PASSWORD   football
   THEN OPEN  http://4.3.2.1
```

Join that network from a phone or laptop — a setup page should open by itself.
Pick your network, enter its password (there is a *Show password* box), and the
device restarts and connects.

### 4. Choose your club

Browse to the device — its address is on the **Device info** page of the
on-screen settings menu (long-press the screen), or try
`http://football-XXXX.local`. Then open **Settings** and fill in:

- Both **API keys**
- Your **football-data team id** and **api-sports team id** (see below)
- The **competition code** (for example `PL` or `ELC`)

Settings take effect immediately — no restart.

## Finding your team's IDs

The two providers number teams differently, so you need one id from each. This
matters more than it sounds: **football-data id 68 is Norwich City, while
api-sports id 68 is Bolton Wanderers.** Mixing them up shows another club's
data rather than failing, so it is worth getting right.

### football-data.org

If the club plays in the competition you are already tracking, the device can
tell you: **Settings → Team → "football-data ids for …"** lists every club in
the table with its id.

Otherwise, with your key:

```bash
curl -H "X-Auth-Token: YOUR_KEY" \
  https://api.football-data.org/v4/competitions/PL/teams \
  | grep -E '"id"|"name"'
```

Docs: <https://www.football-data.org/documentation/quickstart>

### api-sports.io

Search by name from their dashboard, or:

```bash
curl -H "x-apisports-key: YOUR_KEY" \
  "https://v3.football.api-sports.io/teams?search=arsenal"
```

Dashboard: <https://dashboard.api-football.com/> ·
Docs: <https://www.api-football.com/documentation-v3>

## Supported competitions

Whatever football-data.org's free tier covers — currently 13 competitions.
Use the **Code** in the competition setting:

| Code | Competition | Country |
|---|---|---|
| `PL` | Premier League | England |
| `ELC` | Championship | England |
| `BL1` | Bundesliga | Germany |
| `SA` | Serie A | Italy |
| `PD` | Primera Division | Spain |
| `FL1` | Ligue 1 | France |
| `DED` | Eredivisie | Netherlands |
| `PPL` | Primeira Liga | Portugal |
| `BSA` | Campeonato Brasileiro Série A | Brazil |
| `CL` | UEFA Champions League | Europe |
| `EC` | European Championship | Europe |
| `CLI` | Copa Libertadores | South America |
| `WC` | FIFA World Cup | World |

The table, scorers and form come from the competition you set. The **Live**
screen is not restricted to it — a cup tie or a European night is picked up
too, because it asks about your club rather than about a league.

## Using the device

| Gesture | Does |
|---|---|
| **Swipe left / right** | Change screen |
| **Double tap** | Hold the current screen, or resume cycling |
| **Swipe up / down** | Scroll the league table, or the match events |
| **Long press** (2s) | Open the settings menu |

A single tap does nothing deliberately — on a resistive panel it is far too
easy to trigger by brushing the screen.

The on-screen menu shows the device's address, a how-to-use page, and guarded
options to reset Wi-Fi or factory reset. Anything requiring typing lives in the
web interface instead.

## The web interface

| Page | Contents |
|---|---|
| **Status** | Connection, API quota, cache freshness, uptime |
| **Settings** | API keys, team ids, competition, refresh rates, screens, brightness |
| **Cache** | What is stored, how fresh, and controls to refresh or clear it |
| **System** | Version, firmware update, guarded resets |

API keys are never sent back to the browser — the page shows only whether each
is set, and a blank field leaves it unchanged.

## Updating

**Over the air.** The device checks daily and offers anything newer on
**System**. Installing always takes a button press; it never replaces firmware
on its own. The download is hashed and verified before it is committed, so a
failed update leaves the running firmware untouched.

Only plain numbered releases (`1.2.3`) are offered. Pre-releases
(`1.2.3-rc1`) are published but never distributed automatically.

**By hand.** Upload a `.bin` from the System page, or flash over USB.

## API usage and limits

| Provider | Limit | What we do |
|---|---|---|
| football-data.org | 10/minute, no daily cap | Refresh hourly by default |
| api-sports.io | 100/day, 10/minute | Live match only, ~12/hour while playing |

On a day with no match, api-sports is not called at all — the cached fixture
list tells the device there is nothing on, which costs nothing. Everything is
cached to flash, so **restarting the device spends no requests**.

Both rates are configurable under **Settings → How often to update**.

> If you run more than one tracker, give each its own api-sports key. They
> share the 100/day allowance otherwise, and two live matches at once will
> exhaust it.

## Building from source

```bash
pip install platformio
git clone <your-fork-url>
cd ESP32-Football-Tracker
pio run --target upload
```

Optionally, to bake API keys into your own build rather than typing them in:

```bash
cp include/secrets_example.h include/secrets.h
# edit include/secrets.h — it is gitignored
```

Useful build flags for development:

```bash
# Simulate a live match, and narrow the name columns so scrolling engages
PLATFORMIO_BUILD_FLAGS="-DSIMULATE_LIVE_MATCH=1 -DMARQUEE_SQUEEZE=50" pio run -t upload
```

## Tested hardware

| Board | Status |
|---|---|
| ESP32-2432S028R, 2.8", resistive touch, CH340 | ✅ Verified |

Full details, including the GPIO map and the discovery commands, are in
[docs/HARDWARE.md](docs/HARDWARE.md).

Two quirks worth knowing if you are porting to another board:

- Some panels of this model power up **colour-inverted** and need
  `TFT_INVERSION_ON`; ours does.
- The touch digitizer's axes are **transposed** relative to the display.
  Calibration detects this — hold **BOOT** during a reset to recalibrate.

## Troubleshooting

**Colours look inverted** — a panel variant. Flip `TFT_INVERSION_ON` in
`platformio.ini` and reflash.

**Touch is offset or the axes feel wrong** — hold **BOOT** while resetting to
recalibrate. It asks for three taps and then checks itself.

**The Live screen never appears** — check the api-sports key and team id on the
Settings page. The Status page shows how much daily quota is left.

**A screen is missing from the rotation** — screens with no data are skipped.
If Season or Scorers is absent, the football-data team id probably does not
match the competition you set.

**"Port doesn't exist" when flashing** — the USB device name changes when you
replug into a different port. `platformio.ini` matches by pattern for this
reason; with esptool, check the current name.

## Contributing

Contributions are welcome, including bug reports, which are just as useful.

1. **Open an issue first** for anything substantial, so the approach can be
   agreed before you spend time on it.
2. **Fork and branch** — one change per branch, named like
   `feat/live-match-events` or `fix/table-alignment`.
3. **Keep commits small and readable.** Subject lines are one line, in the
   imperative: `fix: clip event labels to their column`.
4. **Comment the *why*, not the *what*.** The codebase explains reasoning,
   trade-offs and rejected alternatives; please match that.
5. **Test on hardware.** This project talks to real APIs and a real panel, and
   several bugs here were invisible until the firmware ran on a board.
6. **Open a pull request.** CI builds every push and pull request as a job
   named `firmware`; a failing build blocks the merge. Setting that up is
   described in [docs/RELEASING.md](docs/RELEASING.md).

There is no formal style guide beyond matching what is there: Google-ish C++,
two-space indent, 80 columns.

Areas that would be genuinely useful:

- Other CYD variants, and other display sizes
- More competitions, or providers with a freer tier
- A deep-sleep quiet-hours mode (the groundwork is in [docs/SPEC.md](docs/SPEC.md))
- Translations

## Reporting problems

Please open an [issue](../../issues/new/choose). The template asks for the
things that usually matter — firmware version, board, competition, and the
serial log if you can capture one:

```bash
pio device monitor
```

For anything involving data, the **Status** and **Cache** pages of the web
interface say a lot in one screenshot.

## Licence

[MIT](LICENSE) — do what you like with it.

Football data from [football-data.org](https://www.football-data.org) and
[API-Sports](https://www.api-football.com). Club crests are served by
football-data.org and remain the property of their respective clubs.
