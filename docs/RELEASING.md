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
3. Computes the SHA-256 and writes `firmware/manifest.json`.
4. Publishes a GitHub Release with `firmware.bin` and the manifest attached.
5. Commits the manifest to `main`, which is the stable URL devices poll.

## Stable releases and pre-releases

**Only plain numbered releases are distributed over the air.**

| Tag | Published as | In the manifest? | In the changelog? | Over the air? |
|---|---|---|---|---|
| `v0.2.0` | ✅ Stable, marked **Latest** | Yes | Yes | Yes |
| `v0.2.0-rc1` | ⚠️ **Pre-release** | No | No | No |
| `v0.2.0-beta.2` | ⚠️ **Pre-release** | No | No | No |
| `v0.2.0-alpha1` | ⚠️ **Pre-release** | No | No | No |

A pre-release still gets a GitHub Release with a binary attached — it is just
kept out of the manifest devices poll, and GitHub marks it so nobody installs
it by accident.

This is enforced in three independent places, deliberately:

1. **The workflow** only updates the manifest for a stable tag.
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

Two URLs are involved, and both were chosen for their certificates:

| URL | Host | Root CA |
|---|---|---|
| Manifest | `raw.githubusercontent.com` | ISRG Root X1 |
| Binary | `github.com` → `objects.githubusercontent.com` | USERTrust ECC → ISRG Root X1 |

All three roots are embedded (`tools/gen_root_certs.py`). The binary download
redirects once, which the updater follows.

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
| `firmware/manifest.json` | What devices poll — **stable releases only** |

Release notes always carry a bullet list of the commit subjects since the
previous *stable* tag. Pre-releases are excluded from that range, so a release
lists everything since the last one people actually received rather than since
the last release candidate.

## The changelog

`CHANGELOG.md` gains a dated section automatically on every stable release,
built from the same commit subjects. `tools/update_changelog.py` does it, and
is a script rather than an inline snippet so it can be run and checked outside
CI:

```bash
git log --no-merges --pretty=format:'- %s' v0.1.0..HEAD > /tmp/changes.md
python3 tools/update_changelog.py 0.2.0 /tmp/changes.md --dry-run
```

It declines to record a pre-release, and declines to add a version twice, so
re-running a release is safe.

## Branch protection

CI builds every pull request as a job named **`firmware`**. To make a failing
build actually block a merge rather than just show a red cross, add a branch
protection rule on `main`:

1. **Settings → Branches → Add branch protection rule**
2. Branch name pattern: `main`
3. Tick **Require status checks to pass before merging**
4. Search for and select **`firmware`**
5. Tick **Require branches to be up to date before merging**

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
