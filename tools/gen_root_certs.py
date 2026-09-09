#!/usr/bin/env python3
"""Generate include/assets_root_certs.h from the Let's Encrypt root CAs.

Both providers serve Let's Encrypt certificates chaining to ISRG Root X1, so a
single trust anchor validates both. Both X1 and X2 are embedded because Let's
Encrypt issues from either, and a chain that terminates at the one we lack
would fail for no good reason.

Certificate validation is worth doing here rather than calling setInsecure():
the API keys travel in request headers, so an active man-in-the-middle could
capture them. It costs ~2.5 KB of flash.

    python3 tools/gen_root_certs.py
"""

import pathlib
import urllib.request

ROOTS = [
    ("ISRG Root X1", "https://letsencrypt.org/certs/isrgrootx1.pem"),
    ("ISRG Root X2", "https://letsencrypt.org/certs/isrg-root-x2.pem"),
]
OUT = pathlib.Path(__file__).parent.parent / "include" / "assets_root_certs.h"


def fetch(url: str) -> str:
    pem = urllib.request.urlopen(url, timeout=30).read().decode("ascii")
    return pem.strip()


def as_c_string(pem: str) -> list[str]:
    return [f'    "{line}\\n"' for line in pem.splitlines()]


def main() -> None:
    lines = [
        "/**",
        " * @file assets_root_certs.h",
        " * @brief Trust anchors for the API providers. GENERATED - DO NOT EDIT.",
        " *",
        " * Both api.football-data.org and v3.football.api-sports.io serve Let's",
        " * Encrypt certificates, so these two roots validate both.",
        " *",
        " * Regenerate with: python3 tools/gen_root_certs.py",
        " *",
        " * NOTE: certificate validation requires a correct clock, because it checks",
        " * validity dates. Fetches must therefore wait for NTP - see refresh.cpp.",
        " *",
        " * If Let's Encrypt ever retires both of these roots, TLS will start failing",
        " * and the fix is a firmware update. That is a deliberate choice: failing",
        " * closed and saying so beats silently accepting any certificate, since the",
        " * API keys travel in request headers.",
        " */",
        "",
        "#pragma once",
        "",
        "namespace assets {",
        "",
    ]

    names = []
    for i, (name, url) in enumerate(ROOTS):
        pem = fetch(url)
        var = f"kRootCa{i}"
        names.append((name, var))
        lines.append(f"/// {name}")
        lines.append(f"const char {var}[] =")
        lines += as_c_string(pem)
        lines[-1] = lines[-1] + ";"
        lines.append("")
        print(f"{name}: {len(pem)} bytes PEM")

    # A single bundle string is what WiFiClientSecure::setCACert wants; it
    # accepts multiple concatenated PEM blocks.
    lines.append("/// Both roots concatenated, for setCACert().")
    lines.append("const char kRootCaBundle[] =")
    for i, (_, url) in enumerate(ROOTS):
        pem = fetch(url)
        lines += as_c_string(pem)
    lines[-1] = lines[-1] + ";"
    lines += ["", "}  // namespace assets", ""]

    OUT.write_text("\n".join(lines))
    print(f"{OUT}: {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
