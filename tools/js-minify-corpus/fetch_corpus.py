#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Fetch the real-world JS corpus from pinned npm registry tarballs.

Two modes:

  resolve   Query the registry for the candidate packages below, download each
            tarball, verify the requested member files exist, compute the
            tarball sha256, and write a fully-pinned manifest. Use this (and
            commit the result) when adding or bumping a package.

  fetch     Download tarballs per the committed manifest, VERIFY the pinned
            sha256 (hard fail on mismatch), and extract the listed member
            files into the corpus directory.

Nothing downloaded or extracted is committed: everything lands under
.corpus/ (gitignored). Third-party sources stay out of the repo.

Stdlib only. Network access is needed for both modes.
"""

import argparse
import hashlib
import json
import os
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MANIFEST = os.path.join(HERE, "manifest.json")
DEFAULT_DEST = os.path.join(HERE, ".corpus", "real")

USER_AGENT = "js-minify-corpus-fetch/1.0 (mod_pagespeed minifier stress spike)"

# Candidates for `resolve`: (package, version, [member files to extract]).
# Version numbers are pinned; resolve verifies they exist and records the
# tarball sha256. Member paths are checked against the tarball listing;
# missing ones are reported and dropped (adjust the list and re-resolve).
CANDIDATES = [
    # UMD, unminified + already-minified pairs (re-minification stress).
    ("jquery", "3.7.1", [
        "dist/jquery.js",
        "dist/jquery.min.js",
        "dist/jquery.slim.js",
        "dist/jquery.slim.min.js",
    ]),
    ("lodash", "4.17.21", [
        "lodash.js",
        "lodash.min.js",
        "core.js",
        "core.min.js",
    ]),
    ("underscore", "1.13.7", [
        "underscore-umd.js",
        "underscore-umd-min.js",
        "underscore-esm.js",
        "underscore-esm-min.js",  # ESM pair, unminified + minified
    ]),
    # Rollup-bundled transpiled output (UMD + ESM + CJS flavours).
    ("axios", "1.10.0", [
        "dist/axios.js",
        "dist/axios.min.js",
        "dist/esm/axios.js",
        "dist/node/axios.cjs",
    ]),
    # Concatenated dist builds.
    ("moment", "2.30.1", [
        "moment.js",
        "min/moment.min.js",
        "min/moment-with-locales.js",
        "min/moment-with-locales.min.js",
    ]),
    ("d3", "7.9.0", [
        "dist/d3.js",       # UMD bundle
        "dist/d3.min.js",
        "src/index.js",     # ESM source
    ]),
    # Modern ESM-first package; also ships a CJS build.
    ("three", "0.160.1", [
        "build/three.module.js",      # unminified ESM, ~1.2MB
        "build/three.module.min.js",  # minified ESM
        "build/three.cjs",            # CJS
    ]),
    # UMD dev builds = large unminified framework output.
    ("react", "18.3.1", [
        "umd/react.development.js",
        "umd/react.production.min.js",
    ]),
    ("react-dom", "18.3.1", [
        "umd/react-dom.development.js",
        "umd/react-dom.production.min.js",
    ]),
    # Framework dist builds: IIFE (script), ESM bundler flavour, CJS.
    ("vue", "3.5.13", [
        "dist/vue.global.js",
        "dist/vue.global.prod.js",
        "dist/vue.esm-bundler.js",
        "dist/vue.cjs.js",
    ]),
    # Vue 2 backport line: ES5-ish dist, good legacy-syntax contrast.
    ("vue", "2.7.16", [
        "dist/vue.js",
        "dist/vue.min.js",
        "dist/vue.esm.js",
    ]),
    # Pre-bundled transpiled polyfill output (minified bundle), plus a few
    # unminified transpiled modules from the core-js package itself.
    ("core-js-bundle", "3.40.0", [
        "minified.js",
    ]),
    ("core-js", "3.40.0", [
        "modules/es.array.iterator.js",
        "modules/es.promise.js",
        "modules/esnext.iterator.from.js",
    ]),
    # TypeScript runtime helpers: ES5 + ESM flavours.
    ("tslib", "2.8.1", [
        "tslib.js",
        "tslib.es6.js",
        "modules/index.js",
    ]),
]


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def tarball_url(name, version):
    # Unscoped packages only: https://registry.npmjs.org/<name>-/<name>-<v>.tgz
    return "https://registry.npmjs.org/%s/-/%s-%s.tgz" % (
        name, name, version)


def safe_members(tf, wanted):
    """Match wanted member paths against the tarball, rejecting unsafe ones."""
    by_name = {}
    for m in tf.getmembers():
        if not m.isfile():
            continue
        # npm tarballs wrap everything in package/.
        rel = m.name[len("package/"):] if m.name.startswith("package/") \
            else m.name
        if os.path.isabs(rel) or ".." in rel.split("/"):
            continue
        by_name[rel] = m
    found, missing = [], []
    for w in wanted:
        (found if w in by_name else missing).append(w)
    return [(w, by_name[w]) for w in found], missing


def cmd_resolve(args):
    entries = []
    failures = 0
    for name, version, wanted in CANDIDATES:
        url = tarball_url(name, version)
        print("resolve %s@%s ..." % (name, version), flush=True)
        try:
            blob = download(url)
        except Exception as e:  # noqa: BLE001 - report and continue
            print("  DOWNLOAD FAILED: %s" % e)
            failures += 1
            continue
        sha = hashlib.sha256(blob).hexdigest()
        tmp = os.path.join(args.dest or HERE, ".resolve-%s-%s.tgz"
                           % (name, version))
        with open(tmp, "wb") as f:
            f.write(blob)
        try:
            with tarfile.open(tmp, "r:gz") as tf:
                found, missing = safe_members(tf, wanted)
        finally:
            os.unlink(tmp)
        for m in missing:
            print("  MISSING MEMBER (dropped): %s" % m)
        entries.append({
            "name": name,
            "version": version,
            "tarball": url,
            "sha256": sha,
            "bytes": len(blob),
            "files": [w for w, _ in found],
        })
    manifest = {"schema": 1, "packages": entries}
    with open(args.out, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=False)
        f.write("\n")
    n_files = sum(len(e["files"]) for e in entries)
    print("wrote %s: %d packages, %d files, %d download failures"
          % (args.out, len(entries), n_files, failures))
    return 1 if failures else 0


def cmd_fetch(args):
    with open(args.manifest) as f:
        manifest = json.load(f)
    os.makedirs(args.dest, exist_ok=True)
    n_files = 0
    for entry in manifest["packages"]:
        label = "%s-%s" % (entry["name"], entry["version"])
        pkg_dir = os.path.join(args.dest, label)
        marker = os.path.join(pkg_dir, ".fetch-complete")
        if os.path.exists(marker):
            print("cached %s" % label)
            n_files += len(entry["files"])
            continue
        print("fetch %s ..." % label, flush=True)
        blob = download(entry["tarball"])
        sha = hashlib.sha256(blob).hexdigest()
        if sha != entry["sha256"]:
            print("FATAL: sha256 mismatch for %s: got %s, manifest pins %s"
                  % (label, sha, entry["sha256"]))
            return 1
        tmp = os.path.join(args.dest, ".tmp-%s.tgz" % label)
        with open(tmp, "wb") as f:
            f.write(blob)
        try:
            with tarfile.open(tmp, "r:gz") as tf:
                found, missing = safe_members(tf, entry["files"])
                if missing:
                    print("FATAL: manifest members missing from tarball "
                          "for %s: %s" % (label, missing))
                    return 1
                for rel, member in found:
                    target = os.path.join(pkg_dir, rel)
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with tf.extractfile(member) as src, \
                            open(target, "wb") as dst:
                        dst.write(src.read())
                    n_files += 1
        finally:
            os.unlink(tmp)
        with open(marker, "w") as f:
            f.write(entry["sha256"] + "\n")
    print("fetched %d files into %s" % (n_files, args.dest))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)
    pr = sub.add_parser("resolve", help="build a pinned manifest")
    pr.add_argument("--out", default=DEFAULT_MANIFEST)
    pr.add_argument("--dest", default=None, help="scratch dir for temp files")
    pf = sub.add_parser("fetch", help="fetch per the committed manifest")
    pf.add_argument("--manifest", default=DEFAULT_MANIFEST)
    pf.add_argument("--dest", default=DEFAULT_DEST)
    args = p.parse_args(argv)
    if args.cmd == "resolve":
        return cmd_resolve(args)
    return cmd_fetch(args)


if __name__ == "__main__":
    sys.exit(main())
