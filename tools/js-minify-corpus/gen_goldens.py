#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Deterministic expected-output goldens for the tokenizer-based JS minifier.

Runs every input of the DETERMINISTIC corpora through the built
js_minify_probe (tokenizer mode, the same contract the CI gate exercises) and
records the expected result in goldens/manifest.json:

  * per input: repo-relative path, input sha256, probe status
    (ok / unmodelable / error-N / signal-N), output sha256. For unmodelable
    inputs the output equals the input by contract -- recorded anyway so a
    contract break is caught too.
  * for the generator: sha256 of generate_synthetic.py and of this script,
    pinning the exact generator version the goldens were produced with.

Covered corpora (checked in or regenerated deterministically, no network):

  bundle-fixtures/src/   first-party bundle entry sources (checked in)
  findings/repros/       committed clean-room repro shapes (checked in)
  synthetic/             generate_synthetic.py output, regenerated into a
                         temp dir on every run (seeded, byte-stable)

The FETCHED real-world corpus and the locally-built bundle outputs are
network-dependent and deliberately excluded; they stay covered by the
existing oracle gate and the release-readiness cross-product run.

The manifest is deterministic: entries sorted by input path, no timestamps,
no absolute paths, no machine identifiers. It is vendored into the
ModPageSpeed 2.0 repo through the same pin as the kernel, so
one shared expectation gates both repos: any behavior change to the minifier
must land together with its golden diff.

Modes:

  gen_goldens.py            regenerate goldens/manifest.json in place
  gen_goldens.py --check    regenerate to memory and diff against the
                            committed manifest; non-zero exit + unified diff
                            on any drift (the CI freshness gate)

Stdlib only; needs the probe built first:
  bazel build //tools/js-minify-corpus:js_minify_probe
"""

import argparse
import difflib
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_PROBE = os.path.join(ROOT, "bazel-bin", "tools", "js-minify-corpus",
                             "js_minify_probe")
DEFAULT_MANIFEST = os.path.join(HERE, "goldens", "manifest.json")
SYNTHETIC_GENERATOR = os.path.join(HERE, "generate_synthetic.py")

# Checked-in input corpora: (repo-relative-to-HERE prefix, extensions).
CHECKED_IN_DIRS = [
    os.path.join("bundle-fixtures", "src"),
    os.path.join("findings", "repros"),
]
JS_EXTS = (".js", ".mjs")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    with open(path, "rb") as f:
        return sha256_bytes(f.read())


def enumerate_checked_in():
    """Yield (manifest_path, fs_path) for the committed input files."""
    for rel_dir in CHECKED_IN_DIRS:
        fs_dir = os.path.join(HERE, rel_dir)
        for name in sorted(os.listdir(fs_dir)):
            if name.endswith(JS_EXTS):
                yield (rel_dir.replace(os.sep, "/") + "/" + name,
                       os.path.join(fs_dir, name))


def enumerate_synthetic(dest):
    """Regenerate the synthetic corpus into dest; yield its files."""
    subprocess.run(
        [sys.executable, SYNTHETIC_GENERATOR, "--dest", dest],
        check=True, stdout=subprocess.DEVNULL)
    for name in sorted(os.listdir(dest)):
        if name.endswith(JS_EXTS):
            yield ("synthetic/" + name, os.path.join(dest, name))


def run_probe(probe, in_path, out_path):
    """Run the probe; return (status, output_sha256_or_None)."""
    # The same out_path is reused across inputs: remove it first so a probe
    # that dies without writing output yields None, not the previous input's
    # stale bytes.
    if os.path.exists(out_path):
        os.remove(out_path)
    proc = subprocess.run(
        [probe, "--mode=tokenizer", in_path, out_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rc = proc.returncode
    if rc == 0:
        status = "ok"
    elif rc == 1:
        status = "unmodelable"
    elif rc < 0:
        status = "signal-%d" % -rc
    else:
        status = "error-%d" % rc
    out_sha = sha256_file(out_path) if os.path.exists(out_path) else None
    return status, out_sha


def build_manifest(probe):
    entries = []
    with tempfile.TemporaryDirectory(prefix="js-goldens-") as tmp:
        synth_dir = os.path.join(tmp, "synthetic")
        out_path = os.path.join(tmp, "probe-out.js")
        inputs = list(enumerate_checked_in())
        inputs.extend(enumerate_synthetic(synth_dir))
        for manifest_path, fs_path in sorted(inputs):
            status, out_sha = run_probe(probe, fs_path, out_path)
            entries.append({
                "input": manifest_path,
                "input_sha256": sha256_file(fs_path),
                "status": status,
                "output_sha256": out_sha,
            })
    return {
        "schema": 1,
        "probe_mode": "tokenizer",
        "generator_sha256": {
            "gen_goldens.py": sha256_file(os.path.abspath(__file__)),
            "generate_synthetic.py": sha256_file(SYNTHETIC_GENERATOR),
        },
        "entries": entries,
    }


def render(manifest):
    return json.dumps(manifest, indent=1) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--probe", default=DEFAULT_PROBE,
                   help="path to the built js_minify_probe binary")
    p.add_argument("--manifest", default=DEFAULT_MANIFEST,
                   help="path of the committed goldens manifest")
    p.add_argument("--check", action="store_true",
                   help="verify the committed manifest instead of writing it")
    args = p.parse_args(argv)

    if not os.path.exists(args.probe):
        print("error: probe not found at %s\n"
              "build it first: bazel build "
              "//tools/js-minify-corpus:js_minify_probe" % args.probe,
              file=sys.stderr)
        return 2

    fresh = render(build_manifest(args.probe))

    if args.check:
        if not os.path.exists(args.manifest):
            print("error: committed manifest missing: %s" % args.manifest,
                  file=sys.stderr)
            return 1
        with open(args.manifest, "r", encoding="utf-8") as f:
            committed = f.read()
        if committed == fresh:
            print("goldens manifest is fresh (%s)"
                  % os.path.relpath(args.manifest, ROOT))
            return 0
        sys.stdout.writelines(difflib.unified_diff(
            committed.splitlines(keepends=True),
            fresh.splitlines(keepends=True),
            fromfile="committed/manifest.json",
            tofile="regenerated/manifest.json"))
        print("error: goldens manifest is stale. Minifier behavior (or a "
              "corpus/generator input) changed without regenerating the "
              "goldens.\nRegenerate and commit the diff in the same PR:\n"
              "  bazel build //tools/js-minify-corpus:js_minify_probe\n"
              "  python3 tools/js-minify-corpus/gen_goldens.py",
              file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.manifest), exist_ok=True)
    with open(args.manifest, "w", encoding="utf-8") as f:
        f.write(fresh)
    print("wrote %s (%d entries)"
          % (os.path.relpath(args.manifest, ROOT),
             len(json.loads(fresh)["entries"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
