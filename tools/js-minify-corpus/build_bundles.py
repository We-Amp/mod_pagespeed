#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Build the bundled-output corpus slice from first-party fixture sources.

The real-world manifest slice (fetch_corpus.py) covers library dist files;
this script covers BUNDLED output shapes the spike was missing: webpack
runtime boilerplate + eval-free module wrappers, bundler-minified output,
legal-banner comments, sourcemap comments, a lazy chunk, and a bundled ESM
(module-goal) file. Bundles are produced locally from the pinned toolchain in
bundle-fixtures/ (package-lock.json pins the full tree); the fixture entry
sources are first-party, so no third-party code is ever committed or fetched
beyond the pinned npm devDependencies.

Modes:

  build   npm ci (once), run webpack/rollup/esbuild, verify output sha256
          against manifest.json's "bundles" section (drift is a WARNING, not
          fatal: tool versions are the pin, hashes are the drift signal), and
          write index.json (oracle goal/kind metadata) into the dest dir.

  record  Same build, then update manifest.json's "bundles" section with the
          freshly computed hashes. Commit the result when bumping tools or
          changing fixture sources.

Artifacts land in .corpus/bundles/ (gitignored). Stdlib only; node + npm are
external. Network is needed for the first npm ci only.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "bundle-fixtures")
DEFAULT_DEST = os.path.join(HERE, ".corpus", "bundles")
DEFAULT_MANIFEST = os.path.join(HERE, "manifest.json")

TOOLS = ["webpack", "webpack-cli", "terser-webpack-plugin", "rollup", "esbuild"]

LEGAL = ("js-minify-corpus bundle fixture (esbuild)\n"
         "Copyright (c) 2026 We-Amp B.V. Synthetic fixture, not a product "
         "build.\n"
         "Bundled from first-party sources in "
         "tools/js-minify-corpus/bundle-fixtures/src/.")

# Per-output oracle metadata (index.json): goal + shape families. Files the
# build emits that are missing here are still indexed (as bundle-misc) with a
# warning, so a config change can never silently drop oracle metadata.
INDEX = {
    "webpack-unminified.js": {
        "module": False,
        "families": ["webpack-runtime", "module-wrappers",
                     "lazy-chunk-runtime"],
    },
    "webpack-unminified.lazy-chunk.js": {
        "module": False,
        "families": ["webpack-lazy-chunk"],
    },
    "webpack-minified-banner.js": {
        "module": False,
        "families": ["webpack-runtime", "bundler-minified", "legal-banner"],
    },
    "webpack-minified-banner.lazy-chunk.js": {
        "module": False,
        "families": ["webpack-lazy-chunk", "bundler-minified",
                     "legal-banner"],
    },
    "webpack-sourcemap.js": {
        "module": False,
        "families": ["webpack-runtime", "module-wrappers",
                     "sourcemap-comment"],
    },
    "webpack-sourcemap.lazy-chunk.js": {
        "module": False,
        "families": ["webpack-lazy-chunk", "sourcemap-comment"],
    },
    "rollup-iife-banner.js": {
        "module": False,
        "families": ["rollup-iife", "legal-banner"],
    },
    "rollup-esm.js": {
        "module": True,
        "families": ["rollup-esm", "module-goal"],
    },
    "esbuild-minified-banner-map.js": {
        "module": False,
        "families": ["esbuild", "bundler-minified", "legal-banner",
                     "sourcemap-comment"],
    },
}


def tool_bin(name):
    return os.path.join(FIXTURES, "node_modules", ".bin", name)


def run(cmd, env):
    print("+ %s" % " ".join(os.path.basename(c) for c in cmd[:2]), flush=True)
    r = subprocess.run(cmd, cwd=FIXTURES, env=env,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-3000:])
        sys.stderr.write(r.stderr[-3000:])
        raise RuntimeError("command failed (%d): %s" % (r.returncode, cmd[0]))


def ensure_toolchain():
    if all(os.path.exists(tool_bin(t)) for t in
           ("webpack", "rollup", "esbuild")):
        print("toolchain present (node_modules), skipping npm ci")
        return
    print("== npm ci (pinned bundle-fixtures toolchain)")
    subprocess.run(["npm", "ci", "--no-audit", "--no-fund"],
                   cwd=FIXTURES, check=True)


def installed_versions():
    versions = {}
    for t in TOOLS:
        pj = os.path.join(FIXTURES, "node_modules", t, "package.json")
        with open(pj) as f:
            versions[t] = json.load(f)["version"]
    return versions


def check_tool_pins(manifest):
    pins = manifest.get("bundles", {}).get("tools", {})
    for tool, ver in installed_versions().items():
        want = pins.get(tool)
        if want and want != ver:
            print("WARNING: %s installed %s but manifest pins %s"
                  % (tool, ver, want))


def clean_dest(dest):
    os.makedirs(dest, exist_ok=True)
    for n in os.listdir(dest):
        if n.endswith((".js", ".js.map", "index.json")):
            os.unlink(os.path.join(dest, n))


def build_all(dest):
    env = dict(os.environ)
    env["JSM_BUNDLE_DIR"] = dest
    env["NO_COLOR"] = "1"
    run([tool_bin("webpack"), "--config", "webpack.config.js"], env)
    run([tool_bin("rollup"), "-c", "rollup.config.mjs"], env)
    run([tool_bin("esbuild"),
         os.path.join("src", "entry.js"),
         "--bundle",
         "--minify",
         "--sourcemap",
         "--banner:js=/*\n%s\n*/" % LEGAL,
         "--outfile=%s" % os.path.join(
             dest, "esbuild-minified-banner-map.js")], env)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def collect_outputs(dest):
    out = {}
    for n in sorted(os.listdir(dest)):
        p = os.path.join(dest, n)
        if n.endswith(".js") and os.path.isfile(p):
            out[n] = {"sha256": sha256(p), "bytes": os.path.getsize(p)}
    return out


def write_index(dest, outputs):
    files = []
    for name in sorted(outputs):
        meta = INDEX.get(name)
        if meta is None:
            print("WARNING: %s has no INDEX entry; indexing as bundle-misc"
                  % name)
            meta = {"module": False, "families": ["bundle-misc"]}
        files.append({"file": name, "module": meta["module"],
                      "kind": "bundle", "families": meta["families"],
                      "executable": False})
    with open(os.path.join(dest, "index.json"), "w") as f:
        json.dump({"files": files}, f, indent=2)
        f.write("\n")


def verify_hashes(outputs, manifest):
    expected = manifest.get("bundles", {}).get("outputs", {})
    drift = 0
    for name, got in sorted(outputs.items()):
        want = expected.get(name, {}).get("sha256")
        if want is None:
            print("UNPINNED  %s (no hash recorded)" % name)
            drift += 1
        elif want != got["sha256"]:
            print("DRIFT     %s (got %s, pinned %s)"
                  % (name, got["sha256"][:12], want[:12]))
            drift += 1
        else:
            print("pin ok    %s" % name)
    for name in sorted(set(expected) - set(outputs)):
        print("MISSING   %s (pinned but not built)" % name)
        drift += 1
    return drift


def record(manifest_path, outputs, manifest):
    bundles = manifest.setdefault("bundles", {})
    bundles["fixtures"] = "bundle-fixtures/"
    bundles["tools"] = installed_versions()
    bundles["outputs"] = outputs
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("recorded %d output hashes in %s" % (len(outputs), manifest_path))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("cmd", nargs="?", default="build",
                   choices=["build", "record"])
    p.add_argument("--dest", default=DEFAULT_DEST)
    p.add_argument("--manifest", default=DEFAULT_MANIFEST)
    args = p.parse_args(argv)

    with open(args.manifest) as f:
        manifest = json.load(f)

    ensure_toolchain()
    check_tool_pins(manifest)
    clean_dest(args.dest)
    build_all(args.dest)
    outputs = collect_outputs(args.dest)
    if not outputs:
        print("FATAL: no bundle outputs produced", file=sys.stderr)
        return 1
    write_index(args.dest, outputs)
    if args.cmd == "record":
        record(args.manifest, outputs, manifest)
    drift = verify_hashes(outputs, manifest)
    print("built %d bundles into %s (%d hash warnings)"
          % (len(outputs), args.dest, drift))
    return 0


if __name__ == "__main__":
    sys.exit(main())
