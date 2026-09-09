#!/usr/bin/env python3
"""Generate include/assets_pure_css.h from the pinned Pure CSS release.

The stylesheet is embedded gzipped and served with Content-Encoding: gzip, so
the browser inflates it and the device never has to. See SPEC.md for why it is
embedded rather than downloaded: the AP setup portal has no internet, so a
download strategy cannot work when it matters most.

Run this only to change the pinned version. The generated header is committed,
so a normal build needs no network access.

    python3 tools/gen_pure_css.py
"""

import gzip
import pathlib
import urllib.request

VERSION = "3.0.0"
URL = f"https://cdn.jsdelivr.net/npm/purecss@{VERSION}/build/pure-min.css"
OUT = pathlib.Path(__file__).parent.parent / "include" / "assets_pure_css.h"


def main() -> None:
    raw = urllib.request.urlopen(URL, timeout=30).read()
    # mtime=0 so repeated runs produce byte-identical output and the header
    # does not show spurious diffs.
    packed = gzip.compress(raw, compresslevel=9, mtime=0)

    lines = [
        "/**",
        " * @file assets_pure_css.h",
        " * @brief Pure CSS, embedded gzipped. GENERATED FILE - DO NOT EDIT.",
        " *",
        f" * Source: {URL}",
        f" * Pure CSS version {VERSION}: {len(raw)} bytes raw, {len(packed)} gzipped.",
        " *",
        " * Regenerate with: python3 tools/gen_pure_css.py",
        " *",
        " * Served with Content-Encoding: gzip, so the browser inflates it and the",
        " * device streams it straight from flash - no decompression, no RAM buffer.",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "namespace assets {",
        "",
        f"/// Uncompressed size, for reference only ({len(raw)} bytes).",
        f"constexpr size_t kPureCssRawSize = {len(raw)};",
        "",
        "/// gzip-compressed Pure CSS. Send verbatim with Content-Encoding: gzip.",
        "const uint8_t kPureCssGz[] = {",
    ]

    for i in range(0, len(packed), 16):
        chunk = packed[i : i + 16]
        lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk))

    lines += [
        "};",
        "",
        f"constexpr size_t kPureCssGzSize = {len(packed)};",
        "",
        "}  // namespace assets",
        "",
    ]

    OUT.write_text("\n".join(lines))
    print(f"{OUT}: {len(raw)} raw -> {len(packed)} gzipped bytes")


if __name__ == "__main__":
    main()
