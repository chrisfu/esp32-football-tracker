"""PlatformIO pre-build script: inject the firmware version.

Single source of truth is the git tag, so a release is cut by tagging and
nothing has to be edited in two places. The VERSION file is the fallback for
builds from a source archive with no git metadata — which is what a GitHub
source tarball is.

Produces, as build flags:
  FIRMWARE_VERSION   "1.2.3"      SemVer, for comparison and display
  FIRMWARE_BUILD     "1.2.3+4.ab12cd" or "1.2.3-dev" for untagged work
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

    # An exact tag looks like "v1.2.3"; anything further along looks like
    # "v1.2.3-4-gab12cd" and may carry "-dirty".
    exact = re.fullmatch(r"v?(\d+\.\d+\.\d+)", described)
    if exact:
        return exact.group(1), exact.group(1)

    ahead = re.match(r"v?(\d+\.\d+\.\d+)-(\d+)-g([0-9a-f]+)(-dirty)?",
                     described)
    if ahead:
        base, count, sha, dirty = ahead.groups()
        # The SemVer part stays the *released* version. Build metadata after
        # "+" is explicitly not part of precedence in SemVer, which is right:
        # an untagged build must not look newer than the release it follows,
        # or OTA would refuse a genuine update.
        build = f"{base}+{count}.g{sha}{'.dirty' if dirty else ''}"
        return base, build

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
