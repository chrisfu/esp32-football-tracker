#!/usr/bin/env python3
"""Generate include/assets_root_certs.h — the device's trust anchors.

Certificates are extracted by subject from curl's maintained CA bundle rather
than fetched individually. Per-root download URLs proved unreliable (one
returned an entirely different certificate), and one well-known bundle is both
easier to verify and easier to re-run.

Why validate at all, rather than calling setInsecure(): the API keys travel in
request headers, so an active man-in-the-middle could capture them, and OTA
downloads firmware the device then executes. Both deserve a real trust chain.

    python3 tools/gen_root_certs.py
"""

import pathlib
import re
import subprocess
import urllib.request

BUNDLE_URL = "https://curl.se/ca/cacert.pem"

# Subject common names of the roots we need, and what needs each.
WANTED = [
    ("ISRG Root X1",
     "football-data.org, api-sports.io, the crest host, "
     "objects.githubusercontent.com"),
    ("ISRG Root X2",
     "Let's Encrypt issues from either root, so both are required"),
    ("USERTrust ECC Certification Authority",
     "github.com, where release downloads start before redirecting"),
]

OUT = pathlib.Path(__file__).parent.parent / "include" / "assets_root_certs.h"


def subject_of(pem: str) -> str:
    result = subprocess.run(
        ["openssl", "x509", "-noout", "-subject"],
        input=pem, capture_output=True, text=True, check=False,
    )
    return result.stdout.strip()


def not_after(pem: str) -> str:
    result = subprocess.run(
        ["openssl", "x509", "-noout", "-enddate"],
        input=pem, capture_output=True, text=True, check=False,
    )
    return result.stdout.strip().replace("notAfter=", "")


def main() -> None:
    bundle = urllib.request.urlopen(BUNDLE_URL, timeout=60).read().decode()
    blocks = re.findall(
        r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
        bundle, re.S,
    )
    print(f"{len(blocks)} certificates in {BUNDLE_URL}")

    found = {}
    for block in blocks:
        subject = subject_of(block)
        for cn, _ in WANTED:
            if f"CN={cn}" in subject or subject.endswith(f"CN = {cn}"):
                found[cn] = block

    missing = [cn for cn, _ in WANTED if cn not in found]
    if missing:
        raise SystemExit(f"roots not found in bundle: {missing}")

    lines = [
        "/**",
        " * @file assets_root_certs.h",
        " * @brief Trust anchors. GENERATED FILE - DO NOT EDIT.",
        " *",
        f" * Extracted by subject from {BUNDLE_URL}",
        " * Regenerate with: python3 tools/gen_root_certs.py",
        " *",
        " * NOTE: certificate validation checks validity dates, so it cannot",
        " * succeed before NTP. Fetches wait for the clock; see refresh.cpp.",
        " *",
        " * If every root here is retired, TLS starts failing and the fix is a",
        " * firmware update. That is deliberate: failing closed and saying so",
        " * beats accepting any certificate, since the API keys travel in",
        " * request headers and OTA downloads code the device then runs.",
        " */",
        "",
        "#pragma once",
        "",
        "namespace assets {",
        "",
    ]

    for cn, why in WANTED:
        pem = found[cn].strip()
        lines.append(f"// {cn}")
        lines.append(f"//   needed for: {why}")
        lines.append(f"//   expires: {not_after(pem)}")
        print(f"  {cn}: expires {not_after(pem)}")

    lines.append("")
    lines.append("/// All roots concatenated, as setCACert() expects.")
    lines.append("const char kRootCaBundle[] =")
    for cn, _ in WANTED:
        for line in found[cn].strip().splitlines():
            lines.append(f'    "{line}\\n"')
    lines[-1] += ";"
    lines += ["", "}  // namespace assets", ""]

    OUT.write_text("\n".join(lines))
    print(f"{OUT}: {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
