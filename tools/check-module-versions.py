#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Drift and supply-chain guard for the C++ libraries taken from the BCR.

For every tracked module (abseil-cpp, zlib-ng, boringssl, re2, googletest,
protobuf, fmt, spdlog) it asserts, without running Bazel:

  1. the module is a root `bazel_dep` and MODULE.bazel.lock selects a registry
     version of it;
  2. the selected version equals the root's declared version (or its
     single_version_override): a version that another module lifted through
     minimal version selection would build something the SBOM does not
     describe, so it fails here and has to be bumped on purpose;
  3. the mirror constant in bazel/repositories.bzl (read by the notices, CVE and
     vendoring tools) equals the selected version, `.bcr.N` stripped;
  4. the committed SBOM (sbom/pagespeed-1.1.spdx.json) carries the selected
     version (protobuf: its purl tag `v<version>`; its versionInfo is the tag
     commit).

It also asserts that every single_version_override version is the selected
version, and that boringssl has one (its version must never float with the
graph).

With `--graph FILE` (the output of `bazel mod graph --output=json`, run in the
lock-check job) it additionally checks that every registry module of the graph
Bazel resolved has exactly one version and that it is the version the lock
selects, and that the lock selects no module outside the graph (except
LOCK_ONLY).

Usage:
  tools/check-module-versions.py
  tools/check-module-versions.py --graph graph.json
  tools/check-module-versions.py --self-test
"""

import json
import os
import re
import sys

import bzlmod_versions

REPO_ROOT = bzlmod_versions.REPO_ROOT
REPOSITORIES_BZL = os.path.join(REPO_ROOT, "bazel", "repositories.bzl")
SBOM_PATH = os.path.join(REPO_ROOT, "sbom", "pagespeed-1.1.spdx.json")

# module, repositories.bzl mirror constant (or None), SBOM package (or None)
TRACKED = [
    ("abseil-cpp", "_ABSEIL_VERSION", "abseil-cpp"),
    ("zlib-ng", "ZLIB_NG_VERSION", "zlib-ng"),
    ("boringssl", "BORINGSSL_VERSION", "boringssl"),
    ("re2", "_RE2_VERSION", "re2"),
    ("googletest", "GOOGLETEST_VERSION", None),
    ("protobuf", None, "protobuf"),
    ("fmt", "FMT_VERSION", "fmt"),
    ("spdlog", "SPDLOG_VERSION", "spdlog"),
]
# Modules whose version must be pinned by a single_version_override.
MUST_OVERRIDE = ["boringssl"]
# SBOM packages whose versionInfo is not the module version; the purl tag is.
PURL_TAG_ONLY = {"protobuf"}
# Modules the lock selects that `bazel mod graph` does not show: Bazel's
# built-in bazel_tools module depends on buildozer, and the graph output
# leaves bazel_tools' subtree out.
LOCK_ONLY = {"buildozer"}


def parse_constants(text):
    consts = {}
    for m in re.finditer(r'^\s*(_?[A-Z][A-Z0-9_]*)\s*=\s*"([^"]+)"', text, re.MULTILINE):
        consts.setdefault(m.group(1), m.group(2))
    return consts


def sbom_versions(doc):
    out = {}
    for pkg in doc.get("packages", []):
        purls = [r["referenceLocator"] for r in pkg.get("externalRefs", [])
                 if r.get("referenceType") == "purl"]
        out[pkg["name"]] = (pkg.get("versionInfo"), purls[0] if purls else "")
    return out


def graph_versions(graph):
    """{module: {versions}} from `bazel mod graph --output=json`."""
    found = {}

    def walk(node):
        key = node.get("key", "")
        name, _, version = key.partition("@")
        if name and version and version != "_" and not key.startswith("<root>"):
            found.setdefault(name, set()).add(version)
        for child in node.get("dependencies", []) or []:
            walk(child)

    walk(graph)
    return found


def check(declared, overrides, resolved, consts, sbom, graph=None):
    errors = []
    up = bzlmod_versions.upstream_version
    for module, const, sbom_name in TRACKED:
        if module not in declared:
            errors.append(f"{module}: not a root bazel_dep in MODULE.bazel")
            continue
        if module not in resolved:
            errors.append(f"{module}: MODULE.bazel.lock selects no registry version")
            continue
        got = resolved[module]
        want = overrides.get(module, declared[module])
        if got != want:
            errors.append(f"{module}: resolved {got} but MODULE.bazel pins {want} "
                          "(lifted by another module? bump the root bazel_dep, its "
                          "mirror constant and the SBOM together)")
        if const is not None:
            if const not in consts:
                errors.append(f"{module}: bazel/repositories.bzl has no {const}")
            elif consts[const] != up(got):
                errors.append(f"{module}: bazel/repositories.bzl {const} = "
                              f"{consts[const]} but the resolved version is {up(got)}")
        if sbom_name is not None:
            if sbom_name not in sbom:
                errors.append(f"{module}: no SBOM package {sbom_name!r}")
            else:
                version, purl = sbom[sbom_name]
                if sbom_name in PURL_TAG_ONLY:
                    if not purl.endswith("@v" + up(got)):
                        errors.append(f"{module}: SBOM purl {purl} does not carry "
                                      f"v{up(got)}; regenerate with tools/generate-sbom.py")
                elif version != up(got):
                    errors.append(f"{module}: SBOM version {version} but the resolved "
                                  f"version is {up(got)}; regenerate with "
                                  "tools/generate-sbom.py")
    for module, version in sorted(overrides.items()):
        if resolved.get(module) != version:
            errors.append(f"{module}: single_version_override pins {version} but the "
                          f"resolved version is {resolved.get(module)}")
    for module in MUST_OVERRIDE:
        if module not in overrides:
            errors.append(f"{module}: needs a single_version_override with a version "
                          "(its version must not float with the module graph)")
    if graph is not None:
        gv = graph_versions(graph)
        for module, versions in sorted(gv.items()):
            if len(versions) != 1:
                errors.append(f"{module}: the module graph holds versions {sorted(versions)}")
            elif module not in resolved:
                errors.append(f"{module}: in the module graph but MODULE.bazel.lock "
                              "selects no registry version of it")
            elif resolved[module] not in versions:
                errors.append(f"{module}: the lock selects {resolved[module]} but the "
                              f"module graph resolved {next(iter(versions))}")
        # The other direction: every module the lock selects is in the graph,
        # except the known lock-only ones. This is exact because lock-check
        # runs it right after asserting that `bazel mod deps` left the
        # committed lock unchanged.
        for module in sorted(resolved):
            if module not in gv and module not in LOCK_ONLY:
                errors.append(f"{module}: MODULE.bazel.lock selects it but the module "
                              "graph does not contain it")
        for module, _, _ in TRACKED:
            if module not in gv:
                errors.append(f"{module}: not in the resolved module graph")
    return errors


def self_test():
    declared = {"abseil-cpp": "1.0", "zlib-ng": "2.3.3.bcr.2", "boringssl": "0.1",
                "re2": "r1", "googletest": "1.17.0.bcr.2", "protobuf": "35.1.bcr.envoy",
                "fmt": "12.2.0", "spdlog": "1.17.0"}
    overrides = {"abseil-cpp": "1.0", "boringssl": "0.1"}
    resolved = dict(declared)
    consts = {"_ABSEIL_VERSION": "1.0", "ZLIB_NG_VERSION": "2.3.3",
              "BORINGSSL_VERSION": "0.1", "_RE2_VERSION": "r1",
              "GOOGLETEST_VERSION": "1.17.0", "FMT_VERSION": "12.2.0",
              "SPDLOG_VERSION": "1.17.0"}
    sbom = {"abseil-cpp": ("1.0", "pkg:github/a/a@1.0"),
            "zlib-ng": ("2.3.3", "pkg:github/z/z@2.3.3"),
            "boringssl": ("0.1", "pkg:github/b/b@0.1"),
            "re2": ("r1", "pkg:github/r/r@r1"),
            "protobuf": ("abc123", "pkg:github/p/p@v35.1"),
            "fmt": ("12.2.0", "pkg:github/f/f@12.2.0"),
            "spdlog": ("1.17.0", "pkg:github/s/s@v1.17.0")}
    graph = {"key": "<root>", "dependencies": [
        {"key": f"{m}@{v}", "dependencies": []} for m, v in resolved.items()]}
    cases = [
        ("pass", {}, 0),
        ("lifted by MVS", {"resolved": dict(resolved, re2="r2")}, 1),
        ("override not honoured", {"resolved": dict(resolved, boringssl="0.2")}, 1),
        ("mirror constant drift", {"consts": dict(consts, ZLIB_NG_VERSION="2.3.2")}, 1),
        ("SBOM drift", {"sbom": dict(sbom, boringssl=("0.0", ""))}, 1),
        ("protobuf purl drift", {"sbom": dict(sbom, protobuf=("abc123", "pkg:github/p/p@v30.0"))}, 1),
        ("protobuf purl keeps the .bcr.envoy suffix",
         {"sbom": dict(sbom, protobuf=("abc123", "pkg:github/p/p@v35.1.bcr.envoy"))}, 1),
        ("fmt mirror constant drift", {"consts": dict(consts, FMT_VERSION="12.1.0")}, 1),
        ("boringssl override missing", {"overrides": {"abseil-cpp": "1.0"}}, 1),
        ("graph disagrees with lock", {"graph": {"key": "<root>", "dependencies": [
            {"key": "re2@r9", "dependencies": []}]}}, 1),
        ("lock-only buildozer is allowed",
         {"resolved": dict(resolved, buildozer="7.1.2")}, 0),
        ("other lock-only module fails",
         {"resolved": dict(resolved, rules_foo="1.0")}, 1),
    ]
    fails = []
    for name, change, want_fail in cases:
        args = dict(declared=declared, overrides=overrides, resolved=resolved,
                    consts=consts, sbom=sbom, graph=graph)
        args.update(change)
        errors = check(**args)
        if bool(errors) != bool(want_fail):
            fails.append(f"{name}: expected {'failure' if want_fail else 'pass'}, got {errors}")
    lock = {"registryFileHashes": {
        "https://bcr.bazel.build/modules/boringssl/0.1/MODULE.bazel": "x",
        "https://bcr.bazel.build/modules/boringssl/0.1/source.json": "x",
        "https://bcr.bazel.build/modules/re2/r0/MODULE.bazel": "x"}}
    if bzlmod_versions.resolved_from_lock(lock) != {"boringssl": "0.1"}:
        fails.append("lock parsing: only source.json entries are selections")
    lock["registryFileHashes"]["https://bcr.bazel.build/modules/boringssl/0.2/source.json"] = "x"
    try:
        bzlmod_versions.resolved_from_lock(lock)
        fails.append("lock parsing: two selected versions must be an error")
    except bzlmod_versions.VersionError:
        pass
    deps, ovr = bzlmod_versions.parse_module_bazel(
        'bazel_dep(name = "a", version = "1")\n# bazel_dep(name = "b", version = "2")\n'
        'single_version_override(\n    module_name = "a",\n    patches = ["x(y).patch"],\n'
        '    version = "1",\n)\n')
    if deps != {"a": "1"} or ovr != {"a": "1"}:
        fails.append(f"MODULE.bazel parsing: got {deps} {ovr}")
    if fails:
        print("\n".join("FAIL: " + f for f in fails), file=sys.stderr)
        return 1
    print(f"check-module-versions self-test OK ({len(cases)} cases)")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    graph = None
    if "--graph" in argv:
        with open(argv[argv.index("--graph") + 1]) as f:
            graph = json.load(f)
    try:
        declared, overrides = bzlmod_versions.load_declared()
        resolved = bzlmod_versions.load_resolved()
    except bzlmod_versions.VersionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    with open(REPOSITORIES_BZL) as f:
        consts = parse_constants(f.read())
    with open(SBOM_PATH) as f:
        sbom = sbom_versions(json.load(f))
    errors = check(declared, overrides, resolved, consts, sbom, graph)
    if errors:
        print("\n".join("FAIL: " + e for e in errors), file=sys.stderr)
        return 1
    for module, _, _ in TRACKED:
        print(f"OK: {module} {resolved[module]}")
    print("module versions agree with MODULE.bazel, repositories.bzl and the SBOM"
          + (" and the resolved module graph" if graph is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
