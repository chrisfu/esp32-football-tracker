# Releasing

Cutting a release is one action: **push a SemVer tag.** Everything else is
automated, so the tag, the binary and the update manifest cannot disagree.

```bash
git tag -a v0.2.0 -m "Add power saving and OTA"
git push origin v0.2.0
```

## What happens then

`.github/workflows/release.yml` runs and:

1. Builds the firmware with the version taken from the tag.
2. **Verifies the binary actually contains that version string.** A firmware
   that misreports its own version would make OTA comparisons meaningless —
   devices would refuse a real update, or reinstall the same one forever — so
   the release fails rather than shipping it.
3. Computes the SHA-256 and writes `manifest.json`.
4. Publishes a GitHub Release with the binary, its `.sha` sidecar and — for a
   stable tag — the manifest attached.

It commits nothing. The release is the distribution: devices poll
`releases/latest/download/manifest.json`, which GitHub resolves to the newest
release not marked as a pre-release.

## Stable releases and pre-releases

**Only plain numbered releases are distributed over the air.**

| Tag | Published as | Manifest attached? | In the changelog? | Over the air? |
|---|---|---|---|---|
| `v0.2.0` | ✅ Stable, marked **Latest** | Yes | Yes | Yes |
| `v0.2.0-rc1` | ⚠️ **Pre-release** | No | No | No |
| `v0.2.0-beta.2` | ⚠️ **Pre-release** | No | No | No |
| `v0.2.0-alpha1` | ⚠️ **Pre-release** | No | No | No |

A pre-release still gets a GitHub Release with a binary attached — it is just
kept out of the manifest devices poll, and GitHub marks it so nobody installs
it by accident.

This is enforced in three independent places, deliberately:

1. **The workflow** attaches a manifest only to a stable release, and
   `releases/latest` skips pre-releases regardless.
2. **`checkForUpdate()`** refuses a manifest advertising a pre-release, so a
   pipeline mistake or a hand-edited manifest cannot push a release candidate
   onto a device sitting on a shelf.
3. **`applyUpdate()`** checks again at the moment of installing.

The firmware and the workflow use the same rule — `^\d+\.\d+\.\d+$` — and
both are tested against the same table of tag names.

Installing a pre-release is therefore a deliberate act: upload it from the
System page, or flash over USB.

## Versioning

The version comes from `git describe`, so there is nothing to edit:

| State | `FIRMWARE_VERSION` | `FIRMWARE_BUILD` |
|---|---|---|
| On tag `v0.2.0` | `0.2.0` | `0.2.0` |
| 4 commits past it | `0.2.0` | `0.2.0+4.gab12cd` |
| With local edits | `0.2.0` | `0.2.0+4.gab12cd.dirty` |
| No git metadata | from `VERSION` | `0.2.0-nogit` |
| On tag `v0.3.0-rc1` | `0.3.0-rc1` | `0.3.0-rc1` |
| 4 commits past an rc | `0.3.0-rc1` | `0.3.0-rc1+4.gab12cd` |

A pre-release tag keeps its suffix in `FIRMWARE_VERSION` rather than being
reduced to the bare triple. A build from `v0.3.0-rc1` must not announce itself
as `0.3.0`: the device's refusal to install pre-releases rests entirely on the
version string being truthful about what it is.

Note also that `-dirty` is build metadata, not a pre-release. It is stripped
before the version is decided — otherwise a stable build with uncommitted
edits would report itself as the pre-release `0.2.0-dirty`.

Note that an untagged build keeps the *released* version in the SemVer field
and puts the extra detail after `+`. SemVer says build metadata is ignored for
precedence, which is exactly what we want: a local development build must
never look newer than the release it follows, or a device would refuse a
genuine update.

Follow SemVer properly, because devices update themselves against it:

* **Patch** — fixes, no behaviour change users would notice.
* **Minor** — new screens, new settings, anything additive.
* **Major** — anything that invalidates stored settings or the cache format,
  or changes hardware expectations.

## How a device updates itself

The device polls the manifest daily and reports what it finds, but **never
installs on its own.** Replacing firmware someone is relying on is not a
decision to make for them, so installing always takes a button press on the
System page.

The download is hashed as it is written and checked against the manifest
before the update is committed. ESP-IDF only switches the boot partition when
the commit succeeds, so a failed hash leaves the running firmware untouched —
which is also why a 1.2 MB image needs no staging space on a 1.4 MB
filesystem.

Both URLs now traverse the same hosts, so the manifest move needed no new
certificate:

| URL | Hosts |
|---|---|
| Manifest | `github.com` → `release-assets.githubusercontent.com` |
| Binary | `github.com` → `release-assets.githubusercontent.com` |

Both were observed on hardware, in the serial log of a real update check — the
asset host is `release-assets.githubusercontent.com`, not the
`objects.githubusercontent.com` this table named until it was checked.

The roots are embedded (`tools/gen_root_certs.py`). `raw.githubusercontent.com`
is no longer used.

Redirects matter here. The binary download redirects once. The manifest URL
redirects **twice** — `releases/latest/download/<asset>` resolves first to the
concrete tag, then to storage — so `kMaxRedirects` in `ota.cpp` is 3, which is
two hops plus headroom rather than a number picked to look safe.

## Published binaries carry no API keys

CI has no `include/secrets.h`, and the header guards on `__has_include`, so a
release build compiles with empty keys. Users enter their own on the Settings
page. A local build can still be provisioned at compile time by copying
`include/secrets_example.h` to `include/secrets.h` — that file is gitignored.

## Manual install

Either upload `firmware.bin` from the device's System page, or flash over USB:

```bash
pio run --target upload
```

An upload has no manifest, so there is no hash to check it against. The image
header is still validated, which catches a truncated or corrupt file — but
nothing confirms it is the firmware you intended. The pull path is the safer
one; upload exists because it works with no internet at all.

## What each release ships

| File | Purpose |
|---|---|
| `firmware-X.Y.Z.bin` | The firmware |
| `firmware-X.Y.Z.bin.sha` | SHA-256 in `shasum -c` format |
| `manifest.json` | What devices poll — **stable releases only** |

Release notes always carry a bullet list of the commit subjects since the
previous *stable* tag. Pre-releases are excluded from that range, so a release
lists everything since the last one people actually received rather than since
the last release candidate.

## The changelog

`CHANGELOG.md` is updated **in the pull request, before tagging** — not by CI.
The workflow deliberately commits nothing, so the changelog is written the same
way as everything else and goes through review with the change it describes:

```bash
git log --no-merges --pretty=tformat:'- %s' v0.2.0..HEAD > /tmp/changes.md
python3 tools/update_changelog.py 0.2.1 /tmp/changes.md --dry-run   # inspect
python3 tools/update_changelog.py 0.2.1 /tmp/changes.md             # apply
```

Use `tformat`, not `format`: the latter omits the trailing newline after the
last entry, which is exactly the sort of thing that reads fine and breaks a
heredoc.

The script declines to record a pre-release, and declines to add a version
twice, so running it again is safe.

## Making a failing build block a merge

CI builds every pull request as a job named **`firmware`**. By default a
failure only shows a red cross — the merge button still works. To make it
actually block, add a rule. GitHub offers two ways; either is fine.

> **Do this after at least one build has run.** A status check only appears in
> the picker once GitHub has seen it, so on a repository where CI has never
> run the box will be empty and the name cannot be typed in. This repository
> has already built, so `firmware` is selectable now.

### Option A — Rulesets (the current mechanism)

1. Repository **Settings**
2. Left sidebar, under *Code and automation* → **Rules** → **Rulesets**
3. **New ruleset** → **New branch ruleset**
4. **Ruleset name**: anything, e.g. `main protection`
5. **Enforcement status**: switch from *Disabled* to **Active** — a new
   ruleset is created disabled, and leaving it that way is the most common
   reason a rule appears to do nothing
6. Under **Target branches** → **Add target** → *Include default branch*
7. Tick **Require status checks to pass**
8. Under it, **Add checks** → search `firmware` → select it
9. Optionally tick **Require branches to be up to date before merging**
10. **Create**

**Note on bypass.** The *Bypass list* is empty by default, so the rule applies
to you as owner too. If you would rather keep pushing straight to `main`
yourself, add **Repository admin** to the bypass list — the rule then still
governs pull requests from anyone else.

**The release workflow needs no bypass.** It publishes a GitHub release and
nothing else — it never commits to `main` — so a ruleset requiring the
`firmware` check does not obstruct it.

That is a deliberate change. The workflow used to push
`firmware/manifest.json` to `main`, which is what devices polled. The ruleset
refused that push: `github-actions[bot]` is not a repository admin, so a
**Repository admin** bypass does not cover it, and its commit carries no status
check of its own. GitHub also offers no **GitHub Actions** entry in the bypass
list on this repository, so there was nothing to add. The result was v0.2.0 —
a release that published cleanly, verified correctly, and reached no device at
all, because the one file they read was never updated.

Devices now poll an asset of the release itself, so publishing the release *is*
distributing it. See **How a device updates itself** above.

### Option B — Classic branch protection

1. Repository **Settings**
2. Left sidebar, under *Code, planning, and automation* → **Branches**
3. Under *Branch protection rules* → **Add classic branch protection rule**
4. **Branch name pattern**: `main`
5. Tick **Require status checks to pass before merging**
6. In the search box that appears, find and select **`firmware`**
7. Optionally tick **Require branches to be up to date before merging**
8. To hold yourself to it as well, tick **Do not allow bypassing the above
   settings** — otherwise admins are exempt
9. **Create**

### What this does and does not do

Requiring a status check blocks *merging a pull request* whose build failed.
It does not stop a direct push to `main` unless you also require pull
requests — worth knowing, since this project has been developed by pushing
directly to `main`.

The build also compiles the development shims
(`SIMULATE_LIVE_MATCH`, `MARQUEE_SQUEEZE`), because nothing else would: they
are compiled out of normal builds, and a macro once defined in a `.cpp` rather
than a header left the default build broken while the flagged build passed.

## Checklist before tagging

- [ ] `pio run` succeeds
- [ ] Flashed and sanity-checked on hardware
- [ ] Flash usage leaves room — the app partition is 1280 KB, and OTA needs
      the *new* image to fit the other slot
- [ ] `SPEC.md` roadmap updated
- [ ] `README.md` still accurate
- [ ] `CHANGELOG.md` updated for this version (`tools/update_changelog.py`),
      committed in the pull request — CI does not write it
- [ ] `VERSION` bumped in the same pull request. It is only the fallback for a
      build with no git metadata, but CI no longer writes it, so nothing else
      will
- [ ] `git push --tags` is up to date — the previous tag must exist **on the
      remote**, not just locally. The release notes are built from
      `previous-tag..new-tag`, and the workflow only sees tags the runner can
      fetch. A previous tag left unpushed makes that range resolve to the whole
      history, and the notes then list every commit since the repository was
      created. Harmless, but it looks careless and cannot be fixed by re-running
