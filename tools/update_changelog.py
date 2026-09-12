#!/usr/bin/env python3
"""Insert a released version's changes into CHANGELOG.md.

Kept as a script rather than inlined in the release workflow so it can be run
and tested outside CI — a shell heredoc inside a YAML block scalar is two
layers of quoting deep and fails in ways that are hard to see.

    python3 tools/update_changelog.py 0.2.0 /tmp/changes.md
    python3 tools/update_changelog.py 0.2.0 /tmp/changes.md --dry-run
"""

import argparse
import datetime
import pathlib
import re
import sys

UNRELEASED = "## [Unreleased]"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("version", help="SemVer being released, e.g. 0.2.0")
    parser.add_argument("changes", help="File of markdown bullets")
    parser.add_argument("--changelog", default="CHANGELOG.md")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        # Pre-releases are published but deliberately not recorded here: the
        # changelog tracks what people actually received.
        print(f"{args.version} is not a plain release; changelog untouched")
        return 0

    path = pathlib.Path(args.changelog)
    text = path.read_text()
    if UNRELEASED not in text:
        print(f"error: {path} has no '{UNRELEASED}' heading", file=sys.stderr)
        return 1

    if f"## [{args.version}]" in text:
        # Re-running a release must not append a second section for it.
        print(f"{args.version} is already in the changelog; nothing to do")
        return 0

    changes = pathlib.Path(args.changes).read_text().strip()
    if not changes:
        changes = "- No changes recorded"

    today = datetime.date.today().isoformat()
    entry = f"{UNRELEASED}\n\n## [{args.version}] - {today}\n\n{changes}\n"
    updated = text.replace(UNRELEASED, entry, 1)

    if args.dry_run:
        print(updated[: updated.index(UNRELEASED) + 600])
        return 0

    path.write_text(updated)
    print(f"Added {args.version} to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
