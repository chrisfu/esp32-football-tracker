"""PlatformIO pre-build script: inject the firmware version.

Single source of truth is the git tag, so a release is cut by tagging and
nothing has to be edited in two places. The VERSION file is the fallback for
builds from a source archive with no git metadata — which is what a GitHub
source tarball is.

Produces, as build flags:
  FIRMWARE_VERSION   "1.2.3"      SemVer, for comparison and display
  FIRMWARE_BUILD     "1.2.3+4.ab12cd" or "1.2.3-dev" for untagged work

A pre-release tag is reported with its suffix intact ("1.2.3-rc1"), never
reduced to the bare triple. The device refuses to install pre-releases over
the air, and that protection rests entirely on the version string being
truthful about what it is.
"""

import pathlib
import re
import subprocess

Import("env")  # noqa: F821  (injected by SCons)

ROOT = pathlib.Path(env.subst("$PROJECT_DIR"))  # noqa: F821


def git(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args], cwd=ROOT, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return ""


def file_version() -> str:
    path = ROOT / "VERSION"
    if path.is_file():
        text = path.read_text().strip()
        if re.fullmatch(r"\d+\.\d+\.\d+", text):
            return text
    return "0.0.0"


def resolve() -> tuple[str, str]:
    """Return (semver, build description)."""
    described = git("describe", "--tags", "--dirty", "--always")
    if not described:
        v = file_version()
        return v, f"{v}-nogit"

    # Strip the dirty marker first and remember it separately.
    #
    # This has to happen before anything else looks at the string. Left in
    # place, "v0.1.0-dirty" matches a pre-release pattern and a stable build
    # with uncommitted edits reports itself as the pre-release "0.1.0-dirty" —
    # which is both untrue and exactly the sort of thing the device's
    # stable-only rule depends on not happening. Dirtiness is build metadata,
    # not part of the version.
    dirty = described.endswith("-dirty")
    base = described[: -len("-dirty")] if dirty else described
    suffix = ".dirty" if dirty else ""

    # An exact stable tag: "v1.2.3".
    exact = re.fullmatch(r"v?(\d+\.\d+\.\d+)", base)
    if exact:
        version = exact.group(1)
        return version, f"{version}+{suffix.lstrip('.')}" if dirty else version

    # Commits past a tag: "v1.2.3-4-gab12cd".
    #
    # Checked before the pre-release pattern, because "-4-gab12cd" would
    # otherwise look like a pre-release suffix.
    ahead = re.fullmatch(r"v?(\d+\.\d+\.\d+)-(\d+)-g([0-9a-f]+)", base)
    if ahead:
        released, count, sha = ahead.groups()
        # The SemVer part stays the *released* version, with the detail after
        # "+". SemVer excludes build metadata from precedence, which is what
        # stops a local build appearing newer than the release it follows.
        return released, f"{released}+{count}.g{sha}{suffix}"

    # Pre-release tag with commits past it: "v1.2.3-rc1-4-gab12cd".
    #
    # Tried BEFORE the exact pre-release pattern below. That pattern's
    # character class includes digits, dots and hyphens, so it happily
    # swallows "-4-gab12cd" and reports the whole thing as the version — which
    # is what it did until this ordering was fixed.
    preAhead = re.fullmatch(
        r"v?(\d+\.\d+\.\d+-[0-9A-Za-z.-]+?)-(\d+)-g([0-9a-f]+)", base
    )
    if preAhead:
        released, count, sha = preAhead.groups()
        return released, f"{released}+{count}.g{sha}{suffix}"

    # An exact pre-release tag: "v1.2.3-rc1", "v1.2.3-beta.2".
    #
    # Reported with its suffix intact rather than reduced to the bare triple.
    # A build from v1.2.3-rc1 must not announce itself as "1.2.3" — the device
    # refuses to install pre-releases, and that protection rests entirely on
    # the version string being truthful about what it is.
    pre = re.fullmatch(r"v?(\d+\.\d+\.\d+-[0-9A-Za-z.-]+)", base)
    if pre:
        version = pre.group(1)
        return version, f"{version}{suffix}"

    # No tags at all yet: fall back to the file, and say so.
    v = file_version()
    return v, f"{v}-dev.{described}"


semver, build = resolve()
print(f"Firmware version: {semver}  (build {build})")

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FIRMWARE_VERSION", env.StringifyMacro(semver)),  # noqa: F821
        ("FIRMWARE_BUILD", env.StringifyMacro(build)),  # noqa: F821
    ]
)
