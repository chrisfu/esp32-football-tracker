# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries below the Unreleased heading are added automatically when a release is
tagged, from the commit subjects since the previous tag — so keeping commit
subjects clear is what keeps this file useful.

## [Unreleased]

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
