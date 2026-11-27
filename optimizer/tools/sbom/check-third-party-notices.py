#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cross-check THIRD-PARTY-NOTICES against the SBOM (and, with --links, the web).

THIRD-PARTY-NOTICES ships in every package, so each entry must name the
component version that is actually built. The SBOM
(sbom/mod_pagespeed-2.1.spdx.json) is generated from MODULE.bazel and the
curated Rust/JavaScript tables and is itself gated for drift, so it is the
reference the notices are checked against, in both directions:

  * an entry whose component has an SBOM package must carry the same version
    (a commit-hash prefix of at least 7 hex digits matches the full hash; a
    Bazel Central Registry ".bcr.N" suffix on the SBOM side is ignored);
  * every SBOM package, other than the product itself and first-party
    components, must have an entry.

Entries for components outside the SBOM (the C++ standard library, crates the
Rust toolchain bundles, the Docker base image) are listed for information and
never fail the check: there is nothing to compare them against. An entry whose
text says it is bundled through the Rust standard library counts as outside the
SBOM even when a crate of the same name is in it, because the SBOM lists the
release the product's own dependencies pull in, not the toolchain's.

With --links every URL in the file is requested (HEAD, falling back to GET
where HEAD is refused); anything but a 2xx/3xx answer fails. This needs the
network, so it belongs on the release checklist rather than in CI.

Usage:
    tools/sbom/check-third-party-notices.py            # version cross-check
    tools/sbom/check-third-party-notices.py --links    # also check every URL

Exit status is 1 on any version mismatch, missing entry, or dead link.
Python standard library only.
"""

import argparse
import concurrent.futures
import json
import os
import re
import sys
import urllib.error
import urllib.request

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NOTICES_PATH = os.path.join(REPO_ROOT, "THIRD-PARTY-NOTICES")
SBOM_PATH = os.path.join(REPO_ROOT, "sbom", "mod_pagespeed-2.1.spdx.json")

# SBOM packages that are the product or first-party code: no third-party notice.
FIRST_PARTY = {"mod_pagespeed-2.1", "cyclone"}

# A token that reads as a version: 1.2.3, v1.2.3, 20260107.1, 2025-11-05,
# a 7..40 digit commit hash, or a bare major version such as "20".
VERSION_TOKEN = re.compile(r"^(v?\d[\w.\-]*|[0-9a-f]{7,40})$")
HEX = re.compile(r"^[0-9a-f]{7,40}$")
URL = re.compile(r"https?://[^\s)\]]+")
# sourceforge.net answers 403 to an agent string it does not recognise.
USER_AGENT = "Mozilla/5.0 (compatible; mod_pagespeed-third-party-notices-check/1.0)"


def normalize_name(name):
    """Fold the spelling differences between the two files (nlohmann/json vs
    nlohmann_json, libjpeg-turbo vs libjpeg_turbo)."""
    return re.sub(r"[-/]", "_", name.strip().lower())


def parse_notices(text):
    """Return [(name, [versions], line_number, bundled)] for every entry.

    `bundled` is True when the entry's text says the component comes in through
    the Rust standard library, so it is not the release the SBOM lists.

    An entry is a non-indented line followed (after optional continuation
    lines) by an indented block that carries a "License:" line. The name is
    everything before the first version-looking token or parenthesis; the
    versions are the version-looking tokens before any parenthesis, so
    "miniz_oxide 0.3.7 and 0.4.4" yields two and "llvm-project 20 (libc++ ...)"
    yields one.
    """
    lines = text.splitlines()
    entries = []
    for i, line in enumerate(lines):
        if not line.strip() or line[0].isspace() or line.startswith("="):
            continue
        # The indented block that follows must contain a License: line.
        j = i + 1
        has_license = False
        while j < len(lines) and (lines[j].startswith(" ") or not lines[j].strip()):
            if not lines[j].strip():
                # A blank line ends the block unless more indented text follows.
                k = j + 1
                while k < len(lines) and not lines[k].strip():
                    k += 1
                if k >= len(lines) or not lines[k].startswith(" "):
                    break
            if re.match(r"^\s+License:", lines[j]):
                has_license = True
            j += 1
        if not has_license:
            continue
        bundled = "standard library" in " ".join(lines[i:j]).lower()
        head = line.split("(", 1)[0].split()
        name_tokens = []
        versions = []
        for tok in head:
            if versions or (name_tokens and VERSION_TOKEN.match(tok)):
                if tok.lower() not in ("and", "or", "&"):
                    versions.append(tok.rstrip(",").lstrip("v"))
            else:
                name_tokens.append(tok)
        entries.append((" ".join(name_tokens), versions, i + 1, bundled))
    return entries


def load_sbom(path):
    """Return {normalized name: (display name, {versions})}; a package that
    appears more than once (two linked releases of one crate) contributes
    every version it appears with."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    packages = {}
    for pkg in doc.get("packages", []):
        name = pkg.get("name", "")
        if name in FIRST_PARTY:
            continue
        display, versions = packages.setdefault(normalize_name(name), (name, set()))
        versions.add(str(pkg.get("versionInfo", "")))
    return packages


def versions_match(notice_version, sbom_version):
    a = notice_version.lstrip("v")
    b = re.sub(r"\.bcr\.\d+$", "", sbom_version.lstrip("v"))
    if a == b:
        return True
    if HEX.match(a) and HEX.match(b):
        return b.startswith(a) or a.startswith(b)
    return False


def check_versions(notices_text, sbom):
    """Print the comparison; return the number of problems."""
    problems = 0
    matched_sbom = set()
    outside = []
    entries = parse_notices(notices_text)
    for name, versions, lineno, bundled in entries:
        key = normalize_name(name)
        if key not in sbom or bundled:
            outside.append((name, versions, lineno))
            continue
        matched_sbom.add(key)
        display, sbom_versions = sbom[key]
        unmatched_notice = [
            v for v in versions if not any(versions_match(v, s) for s in sbom_versions)
        ]
        unmatched_sbom = [
            s for s in sbom_versions if not any(versions_match(v, s) for v in versions)
        ]
        if not versions or unmatched_notice or unmatched_sbom:
            problems += 1
            print(
                f"MISMATCH line {lineno}: {name} {' '.join(versions) or '(no version)'} "
                f"-- SBOM has {display} {' '.join(sorted(sbom_versions))}"
            )
    for key, (display, sbom_versions) in sorted(sbom.items()):
        if key not in matched_sbom:
            problems += 1
            print(
                f"MISSING: SBOM package {display} {' '.join(sorted(sbom_versions))} has no THIRD-PARTY-NOTICES entry"
            )
    print(
        f"{len(entries)} entries: {len(matched_sbom)} matched against {len(sbom)} SBOM packages, "
        f"{len(outside)} outside the SBOM (informational):"
    )
    for name, versions, lineno in outside:
        print(f"  line {lineno}: {name} {' '.join(versions)}".rstrip())
    return problems


def probe(url):
    """Return (url, status or error string). 2xx/3xx after redirects is OK."""
    for method in ("HEAD", "GET"):
        req = urllib.request.Request(
            url, method=method, headers={"User-Agent": USER_AGENT}
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return url, resp.status
        except urllib.error.HTTPError as e:
            if method == "HEAD" and e.code in (403, 405):
                continue
            return url, e.code
        except Exception as e:
            return url, f"error: {e}"
    return url, "unreachable"


def check_links(notices_text):
    urls = sorted({m.group(0).rstrip(".,;") for m in URL.finditer(notices_text)})
    problems = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for url, status in pool.map(probe, urls):
            ok = isinstance(status, int) and 200 <= status < 400
            if not ok:
                problems += 1
            print(
                f"{'OK  ' if ok else 'DEAD'} {status:<12} {url}"
                if isinstance(status, int)
                else f"DEAD {status} {url}"
            )
    print(f"{len(urls)} URLs checked, {problems} failed")
    return problems


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--links", action="store_true", help="also HTTP-check every URL in the file"
    )
    ap.add_argument("--notices", default=NOTICES_PATH, help=argparse.SUPPRESS)
    ap.add_argument("--sbom", default=SBOM_PATH, help=argparse.SUPPRESS)
    args = ap.parse_args()

    with open(args.notices, encoding="utf-8") as f:
        notices_text = f.read()
    problems = check_versions(notices_text, load_sbom(args.sbom))
    if args.links:
        problems += check_links(notices_text)
    if problems:
        print(f"FAIL: {problems} problem(s)", file=sys.stderr)
        return 1
    print(
        "OK: THIRD-PARTY-NOTICES agrees with the SBOM"
        + (" and every link resolves" if args.links else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
