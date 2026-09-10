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

## Versioning

The version comes from `git describe`, so there is nothing to edit:

| State | `FIRMWARE_VERSION` | `FIRMWARE_BUILD` |
|---|---|---|
| On tag `v0.2.0` | `0.2.0` | `0.2.0` |
| 4 commits past it | `0.2.0` | `0.2.0+4.gab12cd` |
| With local edits | `0.2.0` | `0.2.0+4.gab12cd.dirty` |
| No git metadata | from `VERSION` | `0.2.0-nogit` |

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

## Checklist before tagging

- [ ] `pio run` succeeds
- [ ] Flashed and sanity-checked on hardware
- [ ] Flash usage leaves room — the app partition is 1280 KB, and OTA needs
      the *new* image to fit the other slot
- [ ] `SPEC.md` roadmap updated
- [ ] `README.md` still accurate
