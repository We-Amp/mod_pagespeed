#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Generate an SPDX 2.3 JSON SBOM for mod_pagespeed.

Parses MODULE.bazel to extract C++ dependencies, includes curated Rust,
JavaScript, and .NET dependency tables, and optionally reads Cyclone's git
commit hash.

Usage:
    tools/generate-sbom.py            # Generate sbom/mod_pagespeed-2.1.spdx.json
    tools/generate-sbom.py --verify   # Compare generated SBOM with checked-in copy
    tools/generate-sbom.py --name pagespeed-optimizer-2.5.3 --version 2.5.3 \\
        --output dist/pagespeed-optimizer-2.5.3.spdx.json   # a release copy
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from datetime import UTC, datetime
from pathlib import Path

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_BAZEL = os.path.join(REPO_ROOT, "MODULE.bazel")
SBOM_DIR = os.path.join(REPO_ROOT, "sbom")
DEFAULT_NAME = "mod_pagespeed-2.1"
SBOM_PATH = os.path.join(SBOM_DIR, DEFAULT_NAME + ".spdx.json")

# ---------------------------------------------------------------------------
# License mapping: name -> (spdx_license, copyright_holder, homepage_url)
# ---------------------------------------------------------------------------
LICENSE_MAP = {
    "abseil-cpp": ("Apache-2.0", "Google Inc.", "https://github.com/abseil/abseil-cpp"),
    "re2": ("BSD-3-Clause", "Google Inc.", "https://github.com/google/re2"),
    "highway": ("Apache-2.0", "Google LLC", "https://github.com/google/highway"),
    "libjpeg_turbo": (
        "BSD-3-Clause AND IJG",
        "D. R. Commander",
        "https://github.com/libjpeg-turbo/libjpeg-turbo",
    ),
    "jpegli": ("BSD-3-Clause", "Google LLC", "https://github.com/google/jpegli"),
    "libpng": (
        "Libpng-2.0",
        "Glenn Randers-Pehrson et al.",
        "https://github.com/glennrp/libpng",
    ),
    "libwebp": (
        "BSD-3-Clause",
        "Google Inc.",
        "https://github.com/webmproject/libwebp",
    ),
    "zlib": (
        "Zlib",
        "Jean-loup Gailly and Mark Adler",
        "https://github.com/madler/zlib",
    ),
    "brotli": ("MIT", "Google Inc.", "https://github.com/google/brotli"),
    "giflib": ("MIT", "Eric S. Raymond", "https://sourceforge.net/projects/giflib/"),
    "optipng": ("Zlib", "Cosmin Truta", "https://optipng.sourceforge.net/"),
    "libuv": ("MIT", "libuv project contributors", "https://github.com/libuv/libuv"),
    "llhttp": ("MIT", "Node.js contributors", "https://github.com/nodejs/llhttp"),
    "libaom": (
        "BSD-2-Clause",
        "Alliance for Open Media",
        "https://aomedia.googlesource.com/aom/",
    ),
    "libyuv": (
        "BSD-3-Clause",
        "The Chromium Authors",
        "https://chromium.googlesource.com/libyuv/libyuv/",
    ),
    "libavif": ("BSD-2-Clause", "Joe Drago", "https://github.com/AOMediaCodec/libavif"),
    "ed25519": ("Zlib", "Orson Peters", "https://github.com/orlp/ed25519"),
    "skcms": ("BSD-3-Clause", "Google LLC", "https://skia.googlesource.com/skcms"),
    "nlohmann_json": ("MIT", "Niels Lohmann", "https://github.com/nlohmann/json"),
    "nanosvg": ("Zlib", "Mikko Mononen", "https://github.com/memononen/nanosvg"),
    "cyclone": ("Apache-2.0", "We-Amp B.V.", ""),
}

# Build-only deps that should be excluded from the SBOM.
BUILD_ONLY_DEPS = {
    "rules_cc",
    "googletest",
    "rules_foreign_cc",
    "rules_rust",
    "bazel_skylib",
    "platforms",
    "hedron_compile_commands",
}

# GitHub owner/repo for PURL generation (http_archive deps).
GITHUB_OWNERS = {
    "highway": "google/highway",
    "libjpeg_turbo": "libjpeg-turbo/libjpeg-turbo",
    "jpegli": "google/jpegli",
    "libpng": "glennrp/libpng",
    "libwebp": "webmproject/libwebp",
    "zlib": "madler/zlib",
    "brotli": "google/brotli",
    "libuv": "libuv/libuv",
    "llhttp": "nodejs/llhttp",
    "libavif": "AOMediaCodec/libavif",
    "nlohmann_json": "nlohmann/json",
    "nanosvg": "memononen/nanosvg",
    "ed25519": "orlp/ed25519",
}

# BCR deps get GitHub PURLs too.
BCR_GITHUB_OWNERS = {
    "abseil-cpp": "abseil/abseil-cpp",
    "re2": "google/re2",
}

# ---------------------------------------------------------------------------
# Rust transitive dependencies (from compliance plan audit). Versions follow
# lib/image/vtracer_ffi/Cargo.lock, which the Bazel build resolves from;
# THIRD-PARTY-NOTICES is checked against the generated SBOM in CI.
# ---------------------------------------------------------------------------
RUST_DEPS = [
    (
        "vtracer",
        "0.6.5",
        "MIT OR Apache-2.0",
        "visioncortex",
        "https://crates.io/crates/vtracer",
    ),
    (
        "visioncortex",
        "0.8.10",
        "MIT OR Apache-2.0",
        "visioncortex",
        "https://crates.io/crates/visioncortex",
    ),
    (
        "image",
        "0.23.14",
        "MIT",
        "The image-rs Developers",
        "https://crates.io/crates/image",
    ),
    (
        "rayon",
        "1.12.0",
        "MIT OR Apache-2.0",
        "Josh Stone, Niko Matsakis",
        "https://crates.io/crates/rayon",
    ),
    (
        "rayon-core",
        "1.13.0",
        "MIT OR Apache-2.0",
        "Josh Stone, Niko Matsakis",
        "https://crates.io/crates/rayon-core",
    ),
    (
        "crossbeam-deque",
        "0.8.6",
        "MIT OR Apache-2.0",
        "The Crossbeam Project Developers",
        "https://crates.io/crates/crossbeam-deque",
    ),
    (
        "crossbeam-epoch",
        "0.9.20",
        "MIT OR Apache-2.0",
        "The Crossbeam Project Developers",
        "https://crates.io/crates/crossbeam-epoch",
    ),
    (
        "crossbeam-utils",
        "0.8.21",
        "MIT OR Apache-2.0",
        "The Crossbeam Project Developers",
        "https://crates.io/crates/crossbeam-utils",
    ),
    (
        "num-traits",
        "0.2.19",
        "MIT OR Apache-2.0",
        "The Rust Project Developers",
        "https://crates.io/crates/num-traits",
    ),
    (
        "num-integer",
        "0.1.46",
        "MIT OR Apache-2.0",
        "The Rust Project Developers",
        "https://crates.io/crates/num-integer",
    ),
    (
        "num-iter",
        "0.1.45",
        "MIT OR Apache-2.0",
        "The Rust Project Developers",
        "https://crates.io/crates/num-iter",
    ),
    (
        "num-rational",
        "0.3.2",
        "MIT OR Apache-2.0",
        "The Rust Project Developers",
        "https://crates.io/crates/num-rational",
    ),
    (
        "itertools",
        "0.8.2",
        "MIT OR Apache-2.0",
        "bluss",
        "https://crates.io/crates/itertools",
    ),
    (
        "bytemuck",
        "1.25.0",
        "Zlib OR Apache-2.0 OR MIT",
        "Lokathor",
        "https://crates.io/crates/bytemuck",
    ),
    (
        "byteorder",
        "1.5.0",
        "MIT OR Unlicense",
        "Andrew Gallant",
        "https://crates.io/crates/byteorder",
    ),
    (
        "bit-vec",
        "0.6.3",
        "MIT OR Apache-2.0",
        "Alexis Beingessner",
        "https://crates.io/crates/bit-vec",
    ),
    (
        "gif",
        "0.11.4",
        "MIT OR Apache-2.0",
        "The image-rs Developers",
        "https://crates.io/crates/gif",
    ),
    (
        "png",
        "0.16.8",
        "MIT OR Apache-2.0",
        "The image-rs Developers",
        "https://crates.io/crates/png",
    ),
    (
        "jpeg-decoder",
        "0.1.22",
        "MIT OR Apache-2.0",
        "The image-rs Developers",
        "https://crates.io/crates/jpeg-decoder",
    ),
    (
        "tiff",
        "0.6.1",
        "MIT",
        "The image-rs Developers",
        "https://crates.io/crates/tiff",
    ),
    (
        "roots",
        "0.0.6",
        "BSD-2-Clause",
        "Mikhail Vorotilov",
        "https://crates.io/crates/roots",
    ),
    (
        "cfg-if",
        "1.0.4",
        "MIT OR Apache-2.0",
        "Alex Crichton",
        "https://crates.io/crates/cfg-if",
    ),
    (
        "libc",
        "0.2.186",
        "MIT OR Apache-2.0",
        "The Rust Project Developers",
        "https://crates.io/crates/libc",
    ),
    (
        "smallvec",
        "1.15.1",
        "MIT OR Apache-2.0",
        "The Servo Project Developers",
        "https://crates.io/crates/smallvec",
    ),
    (
        "either",
        "1.16.0",
        "MIT OR Apache-2.0",
        "bluss",
        "https://crates.io/crates/either",
    ),
    (
        "fastrand",
        "2.4.1",
        "MIT OR Apache-2.0",
        "Stjepan Glavina",
        "https://crates.io/crates/fastrand",
    ),
    (
        "deflate",
        "0.8.6",
        "MIT OR Apache-2.0",
        "oyvindln",
        "https://crates.io/crates/deflate",
    ),
    (
        "weezl",
        "0.1.12",
        "MIT OR Apache-2.0",
        "HeroicKatora",
        "https://crates.io/crates/weezl",
    ),
    (
        "adler",
        "1.0.2",
        "0BSD OR MIT OR Apache-2.0",
        "Jonas Schievink",
        "https://crates.io/crates/adler",
    ),
    ("adler32", "1.2.0", "Zlib", "Remi Rampin", "https://crates.io/crates/adler32"),
    (
        "crc32fast",
        "1.5.0",
        "MIT OR Apache-2.0",
        "Sam Rijs, Alex Crichton",
        "https://crates.io/crates/crc32fast",
    ),
    (
        "bitflags",
        "1.3.2",
        "MIT OR Apache-2.0",
        "The Rust Project Developers",
        "https://crates.io/crates/bitflags",
    ),
    (
        "autocfg",
        "1.5.1",
        "MIT OR Apache-2.0",
        "Josh Stone",
        "https://crates.io/crates/autocfg",
    ),
    # Linked through vtracer -> visioncortex (curve fitting) and image/png/
    # gif/tiff (quantization, decoder thread pool, inflate); verified as
    # symbols / codegen units in the shipped daemon binary.
    (
        "flo_curves",
        "0.3.1",
        "Apache-2.0",
        "Andrew Hunter",
        "https://crates.io/crates/flo_curves",
    ),
    (
        "color_quant",
        "1.1.0",
        "MIT",
        "PistonDevelopers",
        "https://crates.io/crates/color_quant",
    ),
    (
        "scoped_threadpool",
        "0.1.9",
        "MIT",
        "Marvin Löbel",
        "https://crates.io/crates/scoped_threadpool",
    ),
    # Two releases are linked at once: png pulls 0.3.7, tiff pulls 0.4.4.
    (
        "miniz_oxide",
        "0.3.7",
        "MIT",
        "Frommi",
        "https://crates.io/crates/miniz_oxide",
    ),
    (
        "miniz_oxide",
        "0.4.4",
        "MIT OR Zlib OR Apache-2.0",
        "Frommi",
        "https://crates.io/crates/miniz_oxide",
    ),
]

# ---------------------------------------------------------------------------
# JavaScript runtime dependencies (Console SPA).
# ---------------------------------------------------------------------------
JS_DEPS = [
    ("svelte", "5.55.9", "MIT", "Rich Harris", "https://www.npmjs.com/package/svelte"),
    ("uplot", "1.6.32", "MIT", "Leon Sorokin", "https://www.npmjs.com/package/uplot"),
    (
        "pixelmatch",
        "7.2.0",
        "ISC",
        "Volodymyr Agafonkin",
        "https://www.npmjs.com/package/pixelmatch",
    ),
    ("pngjs", "7.0.0", "MIT", "Luke Page", "https://www.npmjs.com/package/pngjs"),
    (
        "d3-axis",
        "3.0.0",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-axis",
    ),
    (
        "d3-format",
        "3.1.2",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-format",
    ),
    (
        "d3-scale",
        "4.0.2",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-scale",
    ),
    (
        "d3-selection",
        "3.0.0",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-selection",
    ),
    (
        "d3-array",
        "3.2.4",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-array",
    ),
    (
        "d3-interpolate",
        "3.0.1",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-interpolate",
    ),
    (
        "d3-time",
        "3.1.0",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-time",
    ),
    (
        "d3-time-format",
        "4.1.0",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-time-format",
    ),
    (
        "d3-color",
        "3.1.0",
        "ISC",
        "Mike Bostock",
        "https://www.npmjs.com/package/d3-color",
    ),
]

# ---------------------------------------------------------------------------
# .NET/NuGet runtime dependencies of the shipped WeAmp.PageSpeed.AspNetCore
# package. Declared in samples/aspnetcore/src/WeAmp.PageSpeed.AspNetCore/
# WeAmp.PageSpeed.AspNetCore.csproj; the version field is the range declared
# there verbatim (the package floats on the latest 3.x). Test-only packages
# (xunit, NSubstitute, Microsoft.NET.Test.Sdk, Microsoft.AspNetCore.Mvc.Testing)
# never ship and are excluded, as is the Microsoft.AspNetCore.App framework
# reference (part of the .NET runtime, not a NuGet dependency).
#
# The SBOM records the RESOLVED version, not the floating declaration: dotnet
# pack pins the resolved version into the shipped nupkg's nuspec, so the SBOM
# must name the same concrete version for scanners to match it. Resolution
# happens at generation time against nuget.org's flat-container API (see
# resolve_nuget_version below) -- the same answer a fresh `dotnet restore`
# produces. That keeps --verify reproducible in the SBOM-gate CI job, which is
# a checkout-only runner with network but no .NET SDK (so neither
# obj/project.assets.json nor `dotnet restore` is available there), and makes
# the drift check fire when upstream publishes a new matching release: the
# resolved version then differs from the committed SBOM and regeneration is
# forced.
# ---------------------------------------------------------------------------
DOTNET_DEPS = [
    (
        "Microsoft.IO.RecyclableMemoryStream",
        "3.*",
        "MIT",
        "Microsoft",
        "https://www.nuget.org/packages/Microsoft.IO.RecyclableMemoryStream",
    ),
]

NUGET_FLAT_CONTAINER = "https://api.nuget.org/v3-flatcontainer"


def resolve_nuget_version(name, range_spec):
    """Resolve a NuGet version range to the concrete latest matching release.

    Queries the flat-container API (a small static JSON document, the same
    index `dotnet restore` consults) and picks the highest stable version
    matching the range. Supports an exact version ("3.0.1") and a single
    trailing wildcard ("3.*"); prereleases only match a wildcard that asks
    for them ("3.*-alpha" style ranges are not used here). Fails closed: an
    unreachable index or an unmatched range aborts generation rather than
    writing a version the shipped artifact does not carry.
    """
    url = f"{NUGET_FLAT_CONTAINER}/{name.lower()}/index.json"
    try:
        with urllib.request.urlopen(url, timeout=30) as resp:
            versions = json.load(resp)["versions"]
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        sys.exit(f"ERROR: cannot resolve {name}@{range_spec} from {url}: {exc}")
    if range_spec.endswith(".*"):
        prefix = range_spec[: -len(".*")] + "."
        candidates = [v for v in versions if v.startswith(prefix) and "-" not in v]
        if not candidates:
            sys.exit(
                f"ERROR: no stable {name} release on nuget.org matches '{range_spec}'"
            )
        # Numeric compare, not lexicographic: "3.0.10" > "3.0.9".
        return max(candidates, key=lambda v: [int(p) for p in v.split(".")])
    if range_spec not in versions:
        sys.exit(f"ERROR: {name} {range_spec} is not published on nuget.org")
    return range_spec


def _extract_balanced_block(text, start):
    """Extract a parenthesized block starting from the first '(' at or after start."""
    idx = text.index("(", start)
    depth = 0
    i = idx
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[idx : i + 1]
        i += 1
    return text[idx:]


def parse_module_bazel():
    """Parse MODULE.bazel and return a list of C++ dependency dicts."""
    with open(MODULE_BAZEL) as f:
        content = f.read()

    deps = []

    # --- bazel_dep entries ---
    for m in re.finditer(
        r'bazel_dep\(\s*name\s*=\s*"([^"]+)"',
        content,
    ):
        name = m.group(1)
        block = _extract_balanced_block(content, m.start())
        # Skip dev and build-only deps.
        if name in BUILD_ONLY_DEPS:
            continue
        if "dev_dependency" in block and "True" in block:
            continue
        ver_m = re.search(r'version\s*=\s*"([^"]+)"', block)
        if not ver_m:
            continue
        version = ver_m.group(1)
        if name not in LICENSE_MAP:
            continue
        lic, holder, url = LICENSE_MAP[name]
        purl = ""
        if name in BCR_GITHUB_OWNERS:
            purl = f"pkg:github/{BCR_GITHUB_OWNERS[name]}@{version}"
        deps.append(
            {
                "name": name,
                "version": version,
                "license": lic,
                "copyright": holder,
                "homepage": url,
                "download": url,
                "purl": purl,
            }
        )

    # --- http_archive entries ---
    for m in re.finditer(
        r'http_archive\(\s*name\s*=\s*"([^"]+)"',
        content,
    ):
        name = m.group(1)
        # Extract the full block by matching balanced parentheses.
        block = _extract_balanced_block(content, m.start())
        if name not in LICENSE_MAP:
            continue

        # Extract version from strip_prefix by stripping the project name prefix.
        # Patterns: "libpng-1.6.40", "libjpeg-turbo-2.1.5.1",
        #           "jpegli-bc19ca2...", "llhttp-release-v9.2.1"
        version = ""
        sp = re.search(r'strip_prefix\s*=\s*"([^"]+)"', block)
        if sp:
            prefix = sp.group(1)
            # Try to extract a semver-like version (digits.digits...).
            ver_match = re.search(r"-v?(\d+\.\d+[\w.]*)", prefix)
            if ver_match:
                version = ver_match.group(1)
            else:
                # Commit hash or other non-semver suffix: take last component.
                parts = prefix.rsplit("-", 1)
                if len(parts) > 1:
                    version = parts[1]

        sha256 = ""
        sh = re.search(r'sha256\s*=\s*"([^"]+)"', block)
        if sh:
            sha256 = sh.group(1)

        download = ""
        ul = re.search(r'urls\s*=\s*\["([^"]+)"', block)
        if ul:
            download = ul.group(1)

        lic, holder, url = LICENSE_MAP[name]

        purl = ""
        if name in GITHUB_OWNERS:
            purl = f"pkg:github/{GITHUB_OWNERS[name]}@{version}"

        deps.append(
            {
                "name": name,
                "version": version,
                "license": lic,
                "copyright": holder,
                "homepage": url,
                "download": download or url,
                "purl": purl,
                "sha256": sha256,
            }
        )

    # --- new_git_repository entries ---
    for m in re.finditer(
        r'new_git_repository\(\s*name\s*=\s*"([^"]+)"',
        content,
    ):
        name = m.group(1)
        block = _extract_balanced_block(content, m.start())
        if name not in LICENSE_MAP:
            continue
        # Cyclone is emitted once, by generate_sbom(), with its public https
        # location; the git remote here would add a second row with the same
        # SPDXID, and element ids must be unique within a document.
        if name == "cyclone":
            continue

        commit = ""
        cm = re.search(r'commit\s*=\s*"([^"]+)"', block)
        if cm:
            commit = cm.group(1)

        remote = ""
        rm = re.search(r'remote\s*=\s*"([^"]+)"', block)
        if rm:
            remote = rm.group(1)

        lic, holder, url = LICENSE_MAP[name]

        purl = ""
        if name in GITHUB_OWNERS:
            purl = f"pkg:github/{GITHUB_OWNERS[name]}@{commit}"

        deps.append(
            {
                "name": name,
                "version": commit,
                "license": lic,
                "copyright": holder,
                "homepage": url,
                "download": remote or url,
                "purl": purl,
            }
        )

    return deps


def get_cyclone_commit():
    """Get Cyclone's pinned commit from MODULE.bazel, or 'unknown' if not pinned.

    This is the commit Bazel actually builds (the new_git_repository pin), so it
    is deterministic across a dev checkout and the vendored CI tree. It
    previously read reference/cyclone/.git, which resolves to a local dev
    checkout's HEAD but is absent in CI's vendored source — so the cyclone
    versionInfo flipped between a real SHA and 'unknown', making the SBOM
    non-reproducible and failing `generate-sbom.py --verify` at release time.
    """
    try:
        with open(MODULE_BAZEL, encoding="utf-8") as f:
            content = f.read()
    except OSError:
        return "unknown"
    m = re.search(
        r'new_git_repository\(\s*name\s*=\s*"cyclone".*?'
        r'commit\s*=\s*"([0-9a-fA-F]{7,40})"',
        content,
        re.DOTALL,
    )
    return m.group(1) if m else "unknown"


def sanitize_spdx_id(name):
    """Create a valid SPDX identifier from a package name.

    SPDX 2.3 restricts an idstring to letters, digits, "." and "-"; an
    underscore (libjpeg_turbo, nlohmann_json, miniz_oxide) is not allowed.
    """
    return re.sub(r"[^A-Za-z0-9.-]", "-", name)


def make_package(
    name,
    version,
    license_expr,
    copyright_text,
    supplier,
    download_location,
    purl="",
    sha256="",
):
    """Create an SPDX package dict."""
    spdx_id = f"SPDXRef-Package-{sanitize_spdx_id(name)}"
    pkg = {
        "SPDXID": spdx_id,
        "name": name,
        "versionInfo": version,
        "supplier": f"Organization: {supplier}",
        "downloadLocation": download_location or "NOASSERTION",
        "filesAnalyzed": False,
        "licenseConcluded": license_expr,
        "licenseDeclared": license_expr,
        "copyrightText": f"Copyright {copyright_text}",
        "externalRefs": [],
    }
    if purl:
        pkg["externalRefs"].append(
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": purl,
            }
        )
    if sha256:
        pkg["checksums"] = [
            {
                "algorithm": "SHA256",
                "checksumValue": sha256,
            }
        ]
    return pkg


def generate_sbom(doc_name=DEFAULT_NAME, version=None):
    """Generate the full SPDX 2.3 JSON document.

    With the defaults the document is the committed sbom/ copy. A release
    passes its own name and version (--name, --version) so the shipped
    document names the release.
    """
    now = datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

    # Root package version: the caller's, else VERSION.txt at repo root.
    version_file = Path(__file__).parent.parent / "VERSION.txt"
    root_version = version or (
        version_file.read_text().strip() if version_file.exists() else "0.0.0"
    )
    # SPDX ids allow letters, digits, "." and "-" only.
    root_id = "SPDXRef-Package-" + re.sub(r"[^A-Za-z0-9.-]", "-", doc_name)

    # Root package.
    root_pkg = {
        "SPDXID": root_id,
        "name": doc_name,
        "versionInfo": root_version,
        "supplier": "Organization: We-Amp B.V.",
        "downloadLocation": "https://github.com/We-Amp/pagespeed-optimizer",
        "filesAnalyzed": False,
        "licenseConcluded": "Apache-2.0",
        "licenseDeclared": "Apache-2.0",
        "copyrightText": "Copyright (c) 2024-2026 We-Amp B.V.",
        "externalRefs": [],
    }

    packages = [root_pkg]
    relationships = []

    # --- C++ dependencies from MODULE.bazel ---
    cpp_deps = parse_module_bazel()
    for dep in cpp_deps:
        pkg = make_package(
            dep["name"],
            dep["version"],
            dep["license"],
            dep["copyright"],
            dep["copyright"],
            dep.get("download", "NOASSERTION"),
            purl=dep.get("purl", ""),
            sha256=dep.get("sha256", ""),
        )
        packages.append(pkg)
        relationships.append(
            {
                "spdxElementId": root_id,
                "relationshipType": "DEPENDS_ON",
                "relatedSpdxElement": pkg["SPDXID"],
            }
        )

    # --- Cyclone Cache ---
    cyclone_commit = get_cyclone_commit()
    lic, holder, _ = LICENSE_MAP["cyclone"]
    cyclone_pkg = make_package(
        "cyclone",
        cyclone_commit,
        lic,
        holder,
        holder,
        "https://github.com/We-Amp/cyclone-cache",
    )
    packages.append(cyclone_pkg)
    relationships.append(
        {
            "spdxElementId": root_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": cyclone_pkg["SPDXID"],
        }
    )

    # --- Rust dependencies ---
    for name, version, lic_expr, holder, _url in RUST_DEPS:
        purl = f"pkg:cargo/{name}@{version}"
        pkg = make_package(
            name,
            version,
            lic_expr,
            holder,
            holder,
            f"https://crates.io/crates/{name}/{version}",
            purl=purl,
        )
        # Two releases of one crate can be linked at once (miniz_oxide via
        # png and tiff); SPDXIDs must stay unique within the document, so a
        # repeated crate name carries its version in the id.
        if any(p["SPDXID"] == pkg["SPDXID"] for p in packages):
            pkg["SPDXID"] += "-" + sanitize_spdx_id(version)
        packages.append(pkg)
        relationships.append(
            {
                "spdxElementId": root_id,
                "relationshipType": "DEPENDS_ON",
                "relatedSpdxElement": pkg["SPDXID"],
            }
        )

    # --- JavaScript dependencies ---
    for name, version, lic_expr, holder, _url in JS_DEPS:
        purl = f"pkg:npm/{name}@{version}"
        pkg = make_package(
            name,
            version,
            lic_expr,
            holder,
            holder,
            f"https://www.npmjs.com/package/{name}/v/{version}",
            purl=purl,
        )
        packages.append(pkg)
        relationships.append(
            {
                "spdxElementId": root_id,
                "relationshipType": "DEPENDS_ON",
                "relatedSpdxElement": pkg["SPDXID"],
            }
        )

    # --- .NET/NuGet dependencies (ASP.NET Core middleware package) ---
    for name, range_spec, lic_expr, holder, _url in DOTNET_DEPS:
        version = resolve_nuget_version(name, range_spec)
        purl = f"pkg:nuget/{name}@{version}"
        pkg = make_package(
            name,
            version,
            lic_expr,
            holder,
            holder,
            f"https://www.nuget.org/packages/{name}/{version}",
            purl=purl,
        )
        packages.append(pkg)
        relationships.append(
            {
                "spdxElementId": root_id,
                "relationshipType": "DEPENDS_ON",
                "relatedSpdxElement": pkg["SPDXID"],
            }
        )

    # --- DESCRIBES relationship ---
    relationships.insert(
        0,
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": root_id,
        },
    )

    doc = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": doc_name,
        "documentNamespace": f"https://spdx.org/spdxdocs/{doc_name}-{now}",
        "creationInfo": {
            "created": now,
            "creators": [
                "Tool: generate-sbom.py-1.0",
                "Organization: We-Amp B.V.",
            ],
            "licenseListVersion": "3.22",
        },
        "packages": packages,
        "relationships": relationships,
    }

    return doc


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--name", default=DEFAULT_NAME)
    parser.add_argument("--version", default=None)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()
    verify = args.verify
    if verify and (args.output or args.name != DEFAULT_NAME or args.version):
        parser.error("--verify checks the committed copy; it takes no other option")

    doc = generate_sbom(args.name, args.version)
    output = json.dumps(doc, indent=2) + "\n"

    if verify:
        if not os.path.exists(SBOM_PATH):
            print(
                f"ERROR: {SBOM_PATH} does not exist. Run without --verify first.",
                file=sys.stderr,
            )
            sys.exit(1)

        with open(SBOM_PATH) as f:
            existing = f.read()

        # Normalize: strip timestamp-dependent fields for comparison.
        # Re-parse both, zero out created and documentNamespace, then compare.
        def normalize(text):
            d = json.loads(text)
            d["creationInfo"]["created"] = "NORMALIZED"
            d["documentNamespace"] = "NORMALIZED"
            return json.dumps(d, indent=2, sort_keys=True)

        norm_existing = normalize(existing)
        norm_generated = normalize(output)

        if norm_existing == norm_generated:
            print("SBOM is up to date.")
            sys.exit(0)
        else:
            # Write to temp file and diff.
            with tempfile.NamedTemporaryFile(
                mode="w", suffix=".spdx.json", delete=False
            ) as tmp:
                tmp.write(output)
                tmp_path = tmp.name
            try:
                subprocess.run(["diff", "-u", SBOM_PATH, tmp_path], check=False)
            except FileNotFoundError:
                # diff not available; print a simple message.
                print("SBOM differs from checked-in version.", file=sys.stderr)
            finally:
                os.unlink(tmp_path)
            print(
                f"\nERROR: {SBOM_PATH} is stale. Regenerate with: tools/generate-sbom.py",
                file=sys.stderr,
            )
            sys.exit(1)
    else:
        out_path = args.output or SBOM_PATH
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w") as f:
            f.write(output)
        print(f"Generated {out_path}")
        # Count packages (excluding root).
        pkg_count = len(doc["packages"]) - 1
        print(
            f"  {pkg_count} dependencies ({len(parse_module_bazel())} C++, "
            f"1 Cyclone, {len(RUST_DEPS)} Rust, {len(JS_DEPS)} JavaScript, "
            f"{len(DOTNET_DEPS)} .NET)"
        )


if __name__ == "__main__":
    main()
