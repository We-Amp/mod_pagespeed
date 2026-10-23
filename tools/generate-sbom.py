#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Generate (or verify) the SPDX 2.3 SBOM for mod_pagespeed 1.1.

Mirrors pagespeed-optimizer/tools/generate-sbom.py so both products emit
byte-comparable SPDX documents and feed the same grype + OpenVEX gate.

Dependencies come from two places. The C++ libraries taken from the Bazel
Central Registry (abseil-cpp, zlib-ng, boringssl, protobuf, re2) are
`bazel_dep`s in MODULE.bazel; their versions are read from MODULE.bazel.lock,
i.e. the versions the module resolution actually selected (a `.bcr.N` patch
release maps to its upstream version). Everything else is declared in
`bazel/repositories.bzl` (the non_bcr_deps module extension) via
http_archive/git_repository, with versions held in `*_VERSION` / `*_COMMIT`
constants. This script pairs those versions with a curated license/PURL map
(CPP_DEPS): versions auto-track the build, metadata is reviewed.
tools/check-module-versions.py keeps the repositories.bzl mirror constants of
the BCR libraries equal to the resolved versions.

Scope: the directly-declared runtime/library deps. Intentionally excluded:
  - build/test-only: googletest, rules_cc, rules_proto, rules_foreign_cc,
    bazel_compdb
  - rulesets and modules that other modules pull in (rules_java,
    rules_python, etc.): not pinned directly by 1.1.
The committed JS deps are the two bundled into admin_console.html
(svelte, uplot); the rest of pagespeed/system/console/ is build tooling.

Usage:
    tools/generate-sbom.py            # write sbom/pagespeed-1.1.spdx.json
    tools/generate-sbom.py --verify   # fail if the committed copy is stale
    tools/generate-sbom.py --self-test  # assert source-only scoping (Envoy)
"""

import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

import bzlmod_versions

UTC = timezone.utc

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPOSITORIES_BZL = os.path.join(REPO_ROOT, "bazel", "repositories.bzl")
SBOM_DIR = os.path.join(REPO_ROOT, "sbom")
SBOM_PATH = os.path.join(SBOM_DIR, "pagespeed-1.1.spdx.json")
VERSION_FILE = os.path.join(REPO_ROOT, "net", "instaweb", "public", "VERSION")


def read_product_version():
    """Product version MAJOR.MINOR.BUILD from the canonical VERSION file.

    Sourced (not hardcoded) so a version renumber auto-tracks here instead of
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
#   version      {"module": "<bcr module>"} resolved from MODULE.bazel.lock,
#                {"key": "<CONST>"} resolved from repositories.bzl,
#                or {"literal": "<value>"} for deps pinned inline
#   purl_module  optional; derive purl_version as "v<resolved version>" of
#                this BCR module (protobuf: versionInfo stays the tag commit)
#   license      SPDX license expression
#   holder       copyright holder (free text)
#   repo         github "owner/repo" for the PURL, or None (no github PURL)
#   purl_version explicit PURL/tag version; None → use the resolved version
#   source_only  optional; True for a dep that only the SOURCE tree needs
#                (Envoy: no shipped build contains pagespeed/envoy/). It
#                stays a package (same name and purl, so the notices,
#                CPE/NVD matcher and VEX statements keep matching) but
#                is NOT a dependency of the product: it hangs off the
#                source-only filter package, see ENVOY_FILTER_SOURCE.
# downloadLocation is derived from repo (github) or `url` (non-github).
# ---------------------------------------------------------------------------
CPP_DEPS = [
    {"name": "abseil-cpp", "version": {"module": "abseil-cpp"}, "license": "Apache-2.0",
     "holder": "Google Inc.", "repo": "abseil/abseil-cpp", "purl_version": None},
    {"name": "zlib-ng", "version": {"module": "zlib-ng"}, "license": "Zlib",
     "holder": "zlib-ng contributors", "repo": "zlib-ng/zlib-ng", "purl_version": None},
    {"name": "boringssl", "version": {"module": "boringssl"}, "license": "OpenSSL",
     "holder": "Google Inc.", "repo": "google/boringssl", "purl_version": None},
    {"name": "libevent", "version": {"key": "LIBEVENT_VERSION"}, "license": "BSD-3-Clause",
     "holder": "Niels Provos and Nick Mathewson", "repo": "libevent/libevent", "purl_version": None},
    {"name": "protobuf", "version": {"key": "PROTOBUF_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "The Protocol Buffers Authors", "repo": "protocolbuffers/protobuf", "purl_version": None,
     "purl_module": "protobuf"},
    {"name": "re2", "version": {"module": "re2"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "google/re2", "purl_version": None},
    {"name": "fmt", "version": {"key": "FMT_VERSION"}, "license": "MIT",
     "holder": "Victor Zverovich", "repo": "fmtlib/fmt", "purl_version": None},
    {"name": "spdlog", "version": {"key": "SPDLOG_VERSION"}, "license": "MIT",
     "holder": "Gabi Melman", "repo": "gabime/spdlog", "purl_version": "v1.17.0"},
    {"name": "envoy", "version": {"key": "ENVOY_COMMIT"}, "license": "Apache-2.0",
     "holder": "The Envoy Project Authors", "repo": "envoyproxy/envoy", "purl_version": "v1.40.0",
     "source_only": True},
    {"name": "hiredis", "version": {"key": "HIREDIS_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Salvatore Sanfilippo and contributors", "repo": "redis/hiredis", "purl_version": "v1.3.0"},
    {"name": "jsoncpp", "version": {"key": "JSONCPP_COMMIT"}, "license": "MIT",
     "holder": "Baptiste Lepilleur and contributors", "repo": "open-source-parsers/jsoncpp", "purl_version": None},
    {"name": "libpng", "version": {"key": "LIBPNG_COMMIT"}, "license": "Libpng-2.0",
     "holder": "Glenn Randers-Pehrson et al.", "repo": "glennrp/libpng", "purl_version": "v1.6.58"},
    {"name": "libwebp", "version": {"key": "LIBWEBP_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "webmproject/libwebp", "purl_version": "v1.5.0"},
    {"name": "libavif", "version": {"key": "LIBAVIF_VERSION"}, "license": "BSD-2-Clause",
     "holder": "Joe Drago and libavif contributors", "repo": "AOMediaCodec/libavif", "purl_version": "v1.4.2"},
    # aom is commit-pinned (googlesource tarballs are not byte-stable — see
    # bazel/repositories.bzl); the commit equals tag v3.12.0. googlesource
    # has no purl type, so the locator uses the generic form with the
    # dereferenced tag: it exists so the THIRD-PARTY-NOTICES gate has a
    # release tag to check the entry's parenthetical against.
    {"name": "aom", "version": {"key": "AOM_COMMIT"}, "license": "BSD-2-Clause",
     "holder": "Alliance for Open Media", "repo": None,
     "download": "https://aomedia.googlesource.com/aom/", "purl_version": None,
     "purl": "pkg:generic/aom@v3.12.0"},
    {"name": "dav1d", "version": {"key": "DAV1D_VERSION"}, "license": "BSD-2-Clause",
     "holder": "VideoLAN and dav1d authors", "repo": "videolan/dav1d", "purl_version": None},
    {"name": "sparsehash", "version": {"key": "GOOGLE_SPARSEHASH_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "Google Inc.", "repo": "sparsehash/sparsehash", "purl_version": None},
    # gurl (Chromium's URL parser) ships statically linked into core code
    # (pagespeed/kernel/http). It is declared in bazel/repositories.bzl like
    # every other C++ dep precisely so this chain sees it.
    {"name": "gurl", "version": {"key": "GOOGLEURL_COMMIT"}, "license": "BSD-3-Clause",
     "holder": "The Chromium Authors", "repo": "google/gurl", "purl_version": None},
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
     "holder": "Daniel Stenberg and contributors", "repo": "curl/curl", "purl_version": "curl-8_22_0"},
    {"name": "libmemcached", "version": {"key": "LIBMEMCACHED_VERSION"}, "license": "BSD-3-Clause",
     "holder": "Brian Aker / awesomized", "repo": "awesomized/libmemcached", "purl_version": None},
    {"name": "ed25519", "version": {"literal": "b1f19fab4aebe607805620d25a5e42566ce46a0e"}, "license": "Zlib",
     "holder": "Orson Peters", "repo": "orlp/ed25519", "purl_version": None},
    # Cyclone records the PINNED COMMIT rather than the branch it is cut from.
    # The licence of this dependency is a property of the commit, not of the
    # branch: the library was relicensed part-way along `main`, so a bill of
    # materials that says "main" cannot be checked against the bits we ship.
    # The commit can. Tracks CYCLONE_COMMIT the same way every other C++ dep
    # tracks its repositories.bzl constant.
    {"name": "cyclone", "version": {"key": "CYCLONE_COMMIT"}, "license": "Apache-2.0",
     "holder": "We-Amp B.V.", "repo": "We-Amp/cyclone-cache", "purl_version": None},
    # Bundled matched-pair nginx for the ASP.NET Core sidecar (1.30 stable
    # branch). Version is NOT a repositories.bzl constant (nginx is built from upstream
    # source, not a Bazel http_archive), so it is pinned inline as a literal. Keep this
    # lockstep with DEF_NGINX_VER (focal path) in install/nginx/build_module_in_container.sh
    # — the load-bearing pin the sidecar binary is actually built from.
    {"name": "nginx", "version": {"literal": "1.30.6"}, "license": "BSD-2-Clause",
     "holder": "Nginx, Inc. / F5", "repo": None,
     "download": "https://nginx.org/", "purl_version": None},
]

# Source-only package standing for the Envoy filter port (pagespeed/envoy/).
# It ships in the signed source tarball, not in any binary artefact (Apache
# module, nginx module, IIS installer), so the product CONTAINS it as source
# and it DEPENDS_ON the source_only deps; the product itself does not.
ENVOY_FILTER_SOURCE = "pagespeed-envoy-filter-source"
ENVOY_FILTER_COMMENT = (
    "Source-only: the Envoy filter port (pagespeed/envoy/) is carried by the "
    "signed source tarball and is not part of any shipped binary build "
    "(users build it from source). Its dependencies (Envoy) are not "
    "contained in any "
    "binary artefact of pagespeed-1.1."
)
SOURCE_ONLY_DEP_COMMENT = (
    "Required only by the source-only Envoy filter; not contained in any "
    "binary artefact."
)
ROOT_COMMENT = (
    "Covers the source release plus the binary artefacts; the binaries "
    "exclude pagespeed/envoy/ (the source-only Envoy filter)."
)

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


def resolved_module_version(module, resolved):
    if module not in resolved:
        print(f"ERROR: {bzlmod_versions.MODULE_LOCK} selects no registry version "
              f"of module {module!r}", file=sys.stderr)
        sys.exit(1)
    return bzlmod_versions.upstream_version(resolved[module])


def load_resolved():
    try:
        return bzlmod_versions.load_resolved()
    except bzlmod_versions.VersionError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)


def resolve_version(dep, consts, resolved=None):
    """Version of one CPP_DEPS entry.

    `resolved` is the {module: version} map from MODULE.bazel.lock; callers
    that resolve a single dep (tools/dependency/cve_scan.py) may omit it.
    """
    spec = dep["version"]
    if "literal" in spec:
        return spec["literal"]
    if "module" in spec:
        if resolved is None:
            resolved = load_resolved()
        return resolved_module_version(spec["module"], resolved)
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
    resolved = load_resolved()

    root_pkg = {
        "SPDXID": "SPDXRef-Package-pagespeed-1.1",
        "name": "pagespeed-1.1",
        "versionInfo": read_product_version(),
        "supplier": "Organization: We-Amp B.V.",
        "downloadLocation": "https://github.com/We-Amp/mod_pagespeed",
        "filesAnalyzed": False,
        "licenseConcluded": "Apache-2.0",
        "licenseDeclared": "Apache-2.0",
        "copyrightText": "Copyright (c) 2024-2026 We-Amp B.V.",
        "externalRefs": [],
        "comment": ROOT_COMMENT,
    }

    packages = [root_pkg]
    relationships = []

    root_id = "SPDXRef-Package-pagespeed-1.1"
    filter_pkg = make_package(
        ENVOY_FILTER_SOURCE, root_pkg["versionInfo"], "Apache-2.0",
        "(c) 2024-2026 We-Amp B.V.", "We-Amp B.V.", "NOASSERTION")
    filter_pkg["comment"] = ENVOY_FILTER_COMMENT
    filter_id = filter_pkg["SPDXID"]

    def add(pkg, source_only=False):
        packages.append(pkg)
        # A source-only dep is a dependency of the filter source, not of the
        # shipped product.
        relationships.append({
            "spdxElementId": filter_id if source_only else root_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": pkg["SPDXID"],
        })

    # --- C++ dependencies (MODULE.bazel.lock and bazel/repositories.bzl) ---
    for dep in CPP_DEPS:
        version = resolve_version(dep, consts, resolved)
        repo = dep.get("repo")
        # An explicit "purl" (deps without a github locator, e.g. aom on
        # googlesource) wins over the derived github form.
        purl = dep.get("purl", "")
        download = dep.get("download", "")
        if repo:
            purl_ver = dep.get("purl_version") or version
            if dep.get("purl_module"):
                purl_ver = "v" + resolved_module_version(dep["purl_module"], resolved)
            purl = f"pkg:github/{repo}@{purl_ver}"
            download = download or f"https://github.com/{repo}"
        pkg = make_package(dep["name"], version, dep["license"], dep["holder"],
                           dep["holder"], download or "NOASSERTION", purl=purl)
        if dep.get("source_only"):
            pkg["comment"] = SOURCE_ONLY_DEP_COMMENT
        add(pkg, source_only=bool(dep.get("source_only")))

    # --- JavaScript dependencies (bundled admin console) ---
    for name, version, lic_expr, holder, _url in JS_DEPS:
        add(make_package(name, version, lic_expr, holder, holder,
                         f"https://www.npmjs.com/package/{name}/v/{version}",
                         purl=f"pkg:npm/{name}@{version}"))

    if any(d.get("source_only") for d in CPP_DEPS):
        packages.insert(1, filter_pkg)
        relationships.insert(0, {
            "spdxElementId": root_id,
            "relationshipType": "CONTAINS",
            "relatedSpdxElement": filter_id,
        })

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


def self_test():
    """Assert the source-only scoping: Envoy is never a product dependency."""
    doc = generate_sbom()
    root = "SPDXRef-Package-pagespeed-1.1"
    ids = {p["name"]: p["SPDXID"] for p in doc["packages"]}
    rels = {(r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"])
            for r in doc["relationships"]}
    fails = []
    src_only = [d["name"] for d in CPP_DEPS if d.get("source_only")]
    if not src_only:
        print("FAIL: no dependency carries source_only", file=sys.stderr)
        sys.exit(1)
    if "envoy" not in src_only:
        fails.append("envoy must be flagged source_only")
    for name in src_only:
        if (root, "DEPENDS_ON", ids[name]) in rels:
            fails.append(f"{name}: product must not DEPENDS_ON a source-only dep")
        if (ids[ENVOY_FILTER_SOURCE], "DEPENDS_ON", ids[name]) not in rels:
            fails.append(f"{name}: filter source must DEPENDS_ON it")
    if (root, "CONTAINS", ids[ENVOY_FILTER_SOURCE]) not in rels:
        fails.append("product must CONTAIN the filter source package")
    for d in CPP_DEPS:
        if not d.get("source_only") and (root, "DEPENDS_ON", ids[d["name"]]) not in rels:
            fails.append(f"{d['name']}: shipped dep lost its DEPENDS_ON")
    if sum(p["name"] == "envoy" for p in doc["packages"]) != 1:
        fails.append("envoy package must stay exactly once (VEX/notices match it)")
    if fails:
        print("\n".join("FAIL: " + f for f in fails), file=sys.stderr)
        sys.exit(1)
    print("generate-sbom self-test OK")
    sys.exit(0)


def main():
    if "--self-test" in sys.argv:
        self_test()
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
