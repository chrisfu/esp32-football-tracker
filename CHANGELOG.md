# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries below the Unreleased heading are added automatically when a release is
tagged, from the commit subjects since the previous tag — so keeping commit
subjects clear is what keeps this file useful.

## [Unreleased]

## [0.2.1] - 2026-09-12

- fix: stop the over-the-air update check overflowing the fetch task's stack, which crashed the device and left it in a reset loop that survived power-cycling
- fix: skip the automatic update check after one that did not finish, so a crash inside it can never boot-loop the device again
- fix: show the device's real mDNS address on the Device info and How to use screens, instead of a fixed football.local that stopped resolving once hostnames gained a per-device suffix
- fix: stop the release workflow dirtying the working tree before it builds, which stamped v0.2.0's binary as "0.2.0+dirty"
- feat: default the OTA manifest URL, so a freshly flashed device checks for updates without being configured first
- feat: distribute the OTA manifest as a release asset rather than a file on main, so publishing a release is what distributes it
- fix: make the release workflow's binary checks reliable regardless of how grep handles binary files
- fix: correct the blank-line spacing that the changelog generator left between sections

## [0.2.0] - 2026-09-12

- fix: fail loudly if the release manifest cannot be pushed to main
- fix: terminate change-list entries so the release notes heredoc closes
- fix: select the previous stable tag by positive match rather than excluding a dash
- docs: correct the branch protection instructions for the current GitHub UI
- fix: drop the path filter that suppressed CI on a branch-creation push
- chore: add MIT licence, rewrite the README, and automate changelog and releases
- fix: scope the live fetch to our team, rework match polling and schedule post-match refreshes
- fix: identify the tracked club by provider id, widen competition names and version the cache schema
- feat: link the provider docs from settings and list every club id from the cached table
- fix: set football-data crest ids on the live fixture and clip event labels to their column
- feat: half-size crests, newest-first scrollable events and missed-penalty marker on Live
- fix: skip the live fetch when simulating, which was wiping the simulated match
- fix: centre Live panel names on shared columns and stop sprite overspill
- fix: align form chips with the club name above them
- fix: size name columns from measurement, and scroll names that overflow
- fix: centre each club's crest and name on a shared column
- perf: idle power savings, and three optimisations measured then rejected
- feat: on-device power instrumentation, and the savings it found
- feat: only distribute stable releases over the air

## [0.1.0] - 2026-09-10

First tagged release.

- Hardware bring-up for the ESP32-2432S028R, including panel and touch
  identification
- Six screens: live match, season record, last result, next fixture, league
  table and top scorers
- Two-provider data layer with on-flash caching, so a restart costs no API
  requests
- Wi-Fi provisioning over a captive portal, with on-screen instructions
- Web interface for configuration, cache inspection and firmware updates
- Team crests, fetched and cached only for the clubs currently in play
- Over-the-air updates with SHA-256 verification
- Release automation from git tags
