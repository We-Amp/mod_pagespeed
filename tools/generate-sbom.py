#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
"""Generate (or verify) the SPDX 2.3 SBOM for mod_pagespeed 1.1.

Mirrors pagespeed-optimizer/tools/generate-sbom.py so both products emit
byte-comparable SPDX documents and feed the same grype + OpenVEX gate.

Unlike 2.0 (which is Bzlmod and parses MODULE.bazel), 1.1 is a
WORKSPACE-era project: external C++ deps are declared in
`bazel/repositories.bzl` via http_archive/git_repository, with versions
held in `*_VERSION` / `*_COMMIT` constants. This script parses those
constants for versions and pairs them with a curated license/PURL map
(CPP_DEPS) — versions auto-track repositories.bzl, metadata is reviewed.

Scope: the directly-declared runtime/library deps. Intentionally excluded:
  - build/test-only: googletest, rules_foreign_cc, bazel_compdb
  - grpc-transitive (c-ares, protobuf, upb): pulled by grpc_deps(), not
    pinned directly by 1.1; tracked upstream via the gRPC bump.
The committed JS deps are the two bundled into admin_console.html
(svelte, uplot); the rest of pagespeed/system/console/ is build tooling.

Usage:
    tools/generate-sbom.py            # write sbom/pagespeed-1.1.spdx.json
    tools/generate-sbom.py --verify   # fail if the committed copy is stale
"""

import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

UTC = timezone.utc

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPOSITORIES_BZL = os.path.join(REPO_ROOT, "bazel", "repositories.bzl")
SBOM_DIR = os.path.join(REPO_ROOT, "sbom")
SBOM_PATH = os.path.join(SBOM_DIR, "pagespeed-1.1.spdx.json")
VERSION_FILE = os.path.join(REPO_ROOT, "net", "instaweb", "public", "VERSION")


def read_product_version():
    """Product version MAJOR.MINOR.BUILD from the canonical VERSION file.

    Sourced (not hardcoded) so a renumber auto-tracks here instead of
    drifting against a literal. PRERELEASE is intentionally excluded, matching
    the prior committed value (a bare base semver).
    """
    fields = {}
    with open(VERSION_FILE) as vf:
        for line in vf:
            line = line.strip()
            if "=" in line:
                key, _, val = line.partition("=")
                fields[key.strip()] = val.strip()
    return f"{fields['MAJOR']}.{fields['MINOR']}.{fields['BUILD']}"

# ---------------------------------------------------------------------------
# C++ dependencies (declared in bazel/repositories.bzl).
#
# Each entry:
#   name         SPDX package name
#   version      either {"key": "<CONST>"} resolved from repositories.bzl,
#                or {"literal": "<value>"} for deps pinned inline
#   license      SPDX license expression
#   holder       copyright holder (free text)
#   repo         github "owner/repo" for the PURL, or None (no github PURL)
#   purl_version explicit PURL/tag version; None → use the resolved version
# downloadLocation is derived from repo (github) or `url` (non-github).
# ---------------------------------------------------------------------------
CPP_DEPS = [
    {"name": "abseil-cpp", "version": {"key": "_ABSEIL_VERSION"}, "license": "Apache-2.0",
     "holder": "Google Inc.", "repo": "abseil/abseil-cpp", "purl_version": None},
    {"name": "zlib-ng", "version": {"key": "ZLIB_NG_VERSION"}, "license": "Zlib",
     "holder": "zlib-ng contributors", "repo": "zlib-ng/zlib-ng", "purl_version": None},
    {"name": "boringssl", "version": {"key": "BORINGSSL_VERSION"}, "license": "OpenSSL",
     "holder": "Google Inc.", "repo": "google/boringssl", "purl_version": None},
    {"name": "libevent", "version": {"key": "LIBEVENT_VERSION"}, "license": "BSD-3-Clause",
     "holder": "Niels Provos and Nick Mathewson", "repo": "libevent/libevent", "purl_version": None},
    {"name": "grpc", "version": {"key": "GRPC_VERSION"}, "license": "Apache-2.0",
     "holder": "The gRPC Authors", "repo": "grpc/grpc", "purl_version": "v1.78.1"},
    {"name": "re2", "version": {"key": "_RE2_VERSION"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "google/re2", "purl_version": None},
    {"name": "fmt", "version": {"key": "FMT_VERSION"}, "license": "MIT",
     "holder": "Victor Zverovich", "repo": "fmtlib/fmt", "purl_version": None},
    {"name": "spdlog", "version": {"key": "SPDLOG_VERSION"}, "license": "MIT",
     "holder": "Gabi Melman", "repo": "gabime/spdlog", "purl_version": "v1.17.0"},
    {"name": "envoy", "version": {"key": "ENVOY_COMMIT"}, "license": "Apache-2.0",
     "holder": "The Envoy Project Authors", "repo": "envoyproxy/envoy", "purl_version": "v1.37.5"},
    {"name": "hiredis", "version": {"key": "HIREDIS_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Salvatore Sanfilippo and contributors", "repo": "redis/hiredis", "purl_version": "v1.3.0"},
    {"name": "jsoncpp", "version": {"key": "JSONCPP_COMMIT"}, "license": "MIT",
     "holder": "Baptiste Lepilleur and contributors", "repo": "open-source-parsers/jsoncpp", "purl_version": None},
    {"name": "libpng", "version": {"key": "LIBPNG_COMMIT"}, "license": "Libpng-2.0",
     "holder": "Glenn Randers-Pehrson et al.", "repo": "glennrp/libpng", "purl_version": "v1.6.58"},
    {"name": "libwebp", "version": {"key": "LIBWEBP_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "webmproject/libwebp", "purl_version": "v1.5.0"},
    {"name": "sparsehash", "version": {"key": "GOOGLE_SPARSEHASH_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "sparsehash/sparsehash", "purl_version": None},
    {"name": "gflags", "version": {"key": "GFLAGS_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "gflags/gflags", "purl_version": "v2.3.0"},
    {"name": "libpsl", "version": {"key": "LIBPSL_VERSION"}, "license": "MIT",
     "holder": "Tim Ruehsen and contributors", "repo": "rockdaboot/libpsl", "purl_version": "0.21.5"},
    {"name": "giflib", "version": {"key": "GIFLIB_COMMIT"}, "license": "MIT",
     "holder": "Eric S. Raymond", "repo": None,
     "download": "https://sourceforge.net/projects/giflib/", "purl_version": None},
    {"name": "optipng", "version": {"key": "OPTIPNG_COMMIT"}, "license": "Zlib",
     "holder": "Cosmin Truta", "repo": None,
     "download": "https://optipng.sourceforge.net/", "purl_version": None},
    {"name": "libjpeg-turbo", "version": {"key": "LIBJPEG_TURBO_VERSION"}, "license": "BSD-3-Clause AND IJG",
     "holder": "D. R. Commander", "repo": "libjpeg-turbo/libjpeg-turbo", "purl_version": None},
    {"name": "apr", "version": {"key": "APR_COMMIT"}, "license": "Apache-2.0",
     "holder": "The Apache Software Foundation", "repo": "apache/apr", "purl_version": None},
    {"name": "apr-util", "version": {"key": "APRUTIL_COMMIT"}, "license": "Apache-2.0",
     "holder": "The Apache Software Foundation", "repo": "apache/apr-util", "purl_version": None},
    {"name": "curl", "version": {"key": "LIBCURL_VERSION"}, "license": "curl",
     "holder": "Daniel Stenberg and contributors", "repo": "curl/curl", "purl_version": "curl-8_20_0"},
    {"name": "libmemcached", "version": {"key": "LIBMEMCACHED_VERSION"}, "license": "BSD-3-Clause",
     "holder": "Brian Aker / awesomized", "repo": "awesomized/libmemcached", "purl_version": None},
    {"name": "ed25519", "version": {"literal": "b1f19fab4aebe607805620d25a5e42566ce46a0e"}, "license": "Zlib",
     "holder": "Orson Peters", "repo": "orlp/ed25519", "purl_version": None},
    {"name": "cyclone", "version": {"literal": "main"}, "license": "BUSL-1.1",
     "holder": "We-Amp B.V.", "repo": "We-Amp/cyclone-cache", "purl_version": None},
    # Bundled matched-pair nginx for the design record ASP.NET Core sidecar (1.30 stable
    # branch). Version is NOT a repositories.bzl constant (nginx is built from upstream
    # source, not a Bazel http_archive), so it is pinned inline as a literal. Keep this
    # lockstep with DEF_NGINX_VER (focal path) in install/nginx/build_module_in_container.sh
    # — the load-bearing pin the sidecar binary is actually built from.
    {"name": "nginx", "version": {"literal": "1.30.3"}, "license": "BSD-2-Clause",
     "holder": "Nginx, Inc. / F5", "repo": None,
     "download": "https://nginx.org/", "purl_version": None},
]

# ---------------------------------------------------------------------------
# JavaScript runtime dependencies bundled into admin_console.html.
# (pagespeed/system/console/ — only svelte + uplot ship; rest is tooling.)
# ---------------------------------------------------------------------------
JS_DEPS = [
    ("svelte", "5.55.9", "MIT", "Rich Harris", "https://www.npmjs.com/package/svelte"),
    ("uplot", "1.6.32", "MIT", "Leon Sorokin", "https://www.npmjs.com/package/uplot"),
]


def parse_bzl_constants():
    """Extract every `NAME = "value"` string constant from repositories.bzl.

    Captures module-level and indented, with or without a leading underscore
    (e.g. ZLIB_NG_VERSION, _ABSEIL_VERSION, _RE2_VERSION).
    """
    with open(REPOSITORIES_BZL) as f:
        content = f.read()
    consts = {}
    for m in re.finditer(r'^\s*(_?[A-Z][A-Z0-9_]*)\s*=\s*"([^"]+)"', content, re.MULTILINE):
        consts.setdefault(m.group(1), m.group(2))
    return consts


def resolve_version(dep, consts):
    spec = dep["version"]
    if "literal" in spec:
        return spec["literal"]
    key = spec["key"]
    if key not in consts:
        print(f"ERROR: {REPOSITORIES_BZL} has no constant {key!r} for "
              f"dependency {dep['name']!r}", file=sys.stderr)
        sys.exit(1)
    return consts[key]


def sanitize_spdx_id(name):
    """Create a valid SPDX identifier from a package name."""
    return re.sub(r"[^A-Za-z0-9._-]", "-", name)


def make_package(name, version, license_expr, copyright_text, supplier,
                 download_location, purl="", sha256=""):
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
        pkg["externalRefs"].append({
            "referenceCategory": "PACKAGE-MANAGER",
            "referenceType": "purl",
            "referenceLocator": purl,
        })
    if sha256:
        pkg["checksums"] = [{"algorithm": "SHA256", "checksumValue": sha256}]
    return pkg


def generate_sbom():
    """Generate the full SPDX 2.3 JSON document."""
    now = datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    consts = parse_bzl_constants()

    root_pkg = {
        "SPDXID": "SPDXRef-Package-pagespeed-1.1",
        "name": "pagespeed-1.1",
        "versionInfo": read_product_version(),
        "supplier": "Organization: We-Amp B.V.",
        "downloadLocation": "https://github.com/We-Amp/mod_pagespeed",
        "filesAnalyzed": False,
        "licenseConcluded": "BUSL-1.1",
        "licenseDeclared": "BUSL-1.1",
        "copyrightText": "Copyright (c) 2024-2026 We-Amp B.V.",
        "externalRefs": [],
    }

    packages = [root_pkg]
    relationships = []

    def add(pkg):
        packages.append(pkg)
        relationships.append({
            "spdxElementId": "SPDXRef-Package-pagespeed-1.1",
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": pkg["SPDXID"],
        })

    # --- C++ dependencies from bazel/repositories.bzl ---
    for dep in CPP_DEPS:
        version = resolve_version(dep, consts)
        repo = dep.get("repo")
        purl = ""
        download = dep.get("download", "")
        if repo:
            purl_ver = dep.get("purl_version") or version
            purl = f"pkg:github/{repo}@{purl_ver}"
            download = download or f"https://github.com/{repo}"
        add(make_package(dep["name"], version, dep["license"], dep["holder"],
                         dep["holder"], download or "NOASSERTION", purl=purl))

    # --- JavaScript dependencies (bundled admin console) ---
    for name, version, lic_expr, holder, _url in JS_DEPS:
        add(make_package(name, version, lic_expr, holder, holder,
                         f"https://www.npmjs.com/package/{name}/v/{version}",
                         purl=f"pkg:npm/{name}@{version}"))

    relationships.insert(0, {
        "spdxElementId": "SPDXRef-DOCUMENT",
        "relationshipType": "DESCRIBES",
        "relatedSpdxElement": "SPDXRef-Package-pagespeed-1.1",
    })

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "pagespeed-1.1",
        "documentNamespace": f"https://spdx.org/spdxdocs/pagespeed-1.1-{now}",
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


def main():
    verify = "--verify" in sys.argv
    doc = generate_sbom()
    output = json.dumps(doc, indent=2) + "\n"

    if verify:
        if not os.path.exists(SBOM_PATH):
            print(f"ERROR: {SBOM_PATH} does not exist. Run without --verify first.",
                  file=sys.stderr)
            sys.exit(1)
        with open(SBOM_PATH) as f:
            existing = f.read()

        def normalize(text):
            d = json.loads(text)
            d["creationInfo"]["created"] = "NORMALIZED"
            d["documentNamespace"] = "NORMALIZED"
            return json.dumps(d, indent=2, sort_keys=True)

        if normalize(existing) == normalize(output):
            print("SBOM is up to date.")
            sys.exit(0)
        with tempfile.NamedTemporaryFile(mode="w", suffix=".spdx.json", delete=False) as tmp:
            tmp.write(output)
            tmp_path = tmp.name
        try:
            subprocess.run(["diff", "-u", SBOM_PATH, tmp_path], check=False)
        except FileNotFoundError:
            print("SBOM differs from checked-in version.", file=sys.stderr)
        finally:
            os.unlink(tmp_path)
        print(f"\nERROR: {SBOM_PATH} is stale. Regenerate with: tools/generate-sbom.py",
              file=sys.stderr)
        sys.exit(1)

    os.makedirs(SBOM_DIR, exist_ok=True)
    with open(SBOM_PATH, "w") as f:
        f.write(output)
    print(f"Generated {SBOM_PATH}")
    print(f"  {len(doc['packages']) - 1} dependencies "
          f"({len(CPP_DEPS)} C++, {len(JS_DEPS)} JavaScript)")


if __name__ == "__main__":
    main()
