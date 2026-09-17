#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Deterministic expected-output goldens for the HTML parse event stream.

Runs every corpus input through the built html_parse_probe (the SPEC.md v1
contract the differential gate exercises) and records the expected result in
goldens/manifest.json:

  * per input: repo-relative path, input sha256, probe status
    (ok / size-limit / error-N / signal-N), output (event stream) sha256.
  * for the generator: sha256 of gen_extra_inputs.py and of this script,
    pinning the exact generator version the goldens were produced with.

Covered corpora (checked in or regenerated deterministically, no network):

  seeds/       hand-written battery: the 39-case doctype battery, nesting
               depth boundary (511/512/513), numeric-entity edges,
               comments/CDATA/literal-tag content, malformed and truncated
               tags, high-bit and NUL bytes, auto-close and attribute forms
  generated/   gen_extra_inputs.py output, regenerated into a temp dir on
               every run (byte-stable): ~1MB document, and two >10MB
               single-token inputs that trip size_limit_exceeded

The manifest is deterministic: entries sorted by input path, no timestamps,
no absolute paths, no machine identifiers. It is vendored into the
ModPageSpeed 2.0 repo through the same pin as the kernel, so
one shared expectation gates both repos: any behavior change to the
lexer/parser must land together with its golden diff.

Modes:

  gen_goldens.py            regenerate goldens/manifest.json in place
  gen_goldens.py --check    regenerate to memory and diff against the
                            committed manifest; non-zero exit + unified diff
                            on any drift (the CI freshness gate)

Stdlib only; needs the probe built first:
  bazel build --config=macos-asan -c opt //pagespeed/kernel/html:html_parse_probe
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
DEFAULT_PROBE = os.path.join(ROOT, "bazel-bin", "pagespeed", "kernel", "html",
                             "html_parse_probe")
DEFAULT_MANIFEST = os.path.join(HERE, "goldens", "manifest.json")
EXTRA_GENERATOR = os.path.join(HERE, "gen_extra_inputs.py")

CHECKED_IN_DIRS = ["seeds"]
HTML_EXTS = (".html", ".htm")

# Probe exit codes per SPEC.md SS4.
STATUS_BY_RC = {0: "ok", 3: "size-limit"}


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
            if name.endswith(HTML_EXTS):
                yield (rel_dir + "/" + name, os.path.join(fs_dir, name))


def enumerate_generated(dest):
    """Regenerate the large inputs into dest; yield their files."""
    subprocess.run(
        [sys.executable, EXTRA_GENERATOR, "--dest", dest],
        check=True, stdout=subprocess.DEVNULL)
    for name in sorted(os.listdir(dest)):
        if name.endswith(HTML_EXTS):
            yield ("generated/" + name, os.path.join(dest, name))


def run_probe(probe, in_path):
    """Run the probe; return (status, output_sha256_or_None)."""
    proc = subprocess.run([probe, in_path],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    rc = proc.returncode
    if rc in STATUS_BY_RC:
        status = STATUS_BY_RC[rc]
    elif rc < 0:
        status = "signal-%d" % -rc
    else:
        status = "error-%d" % rc
    out_sha = sha256_bytes(proc.stdout) if proc.stdout else None
    return status, out_sha


def build_manifest(probe):
    entries = []
    with tempfile.TemporaryDirectory(prefix="html-goldens-") as tmp:
        gen_dir = os.path.join(tmp, "generated")
        inputs = list(enumerate_checked_in())
        inputs.extend(enumerate_generated(gen_dir))
        for manifest_path, fs_path in sorted(inputs):
            status, out_sha = run_probe(probe, fs_path)
            entries.append({
                "input": manifest_path,
                "input_sha256": sha256_file(fs_path),
                "status": status,
                "output_sha256": out_sha,
            })
    return {
        "schema": 1,
        "spec": "SPEC.md v1",
        "generator_sha256": {
            "gen_goldens.py": sha256_file(os.path.abspath(__file__)),
            "gen_extra_inputs.py": sha256_file(EXTRA_GENERATOR),
        },
        "entries": entries,
    }


def render(manifest):
    return json.dumps(manifest, indent=1) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--probe", default=DEFAULT_PROBE,
                   help="path to the built html_parse_probe binary")
    p.add_argument("--manifest", default=DEFAULT_MANIFEST,
                   help="path of the committed goldens manifest")
    p.add_argument("--check", action="store_true",
                   help="verify the committed manifest instead of writing it")
    args = p.parse_args(argv)

    if not os.path.exists(args.probe):
        print("error: probe not found at %s\n"
              "build it first: bazel build "
              "//pagespeed/kernel/html:html_parse_probe" % args.probe,
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
        print("error: goldens manifest is stale. Parser behavior (or a "
              "corpus/generator input) changed without regenerating the "
              "goldens.\nRegenerate and commit the diff in the same PR:\n"
              "  bazel build //pagespeed/kernel/html:html_parse_probe\n"
              "  python3 tools/html-parse-corpus/gen_goldens.py",
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
