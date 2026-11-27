#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Local conformance gate for the HTML parse corpus.

Optimizer-local counterpart to the vendored gen_goldens.py --check. The vendored
generator is kept byte-identical to the canonical mod_pagespeed 1.15 copy
(the shared manifest pins its sha256, D1 style), so it cannot learn about
EXCLUSIONS.md here. This checker therefore lives next to it as an
optimizer-owned file and gates: local probe output vs the vendored shared
manifest, honoring the exclusion list.

For every input in goldens/manifest.json:

  * the checked-in (or deterministically regenerated) input bytes must
    still match the manifest's input_sha256 (corpus drift check);
  * non-excluded inputs: the probe's status (ok / size-limit / error-N /
    signal-N per SPEC.md SS4) and output sha256 must equal the manifest
    entry -- byte-level behavioral conformance with 1.15;
  * excluded inputs (EXCLUSIONS.md): no cross-product golden is compared,
    but the probe must still parse without crashing (the crash oracle of
    SPEC.md SS5), and the exclusion must remain NECESSARY -- if the probe
    output now equals the manifest entry, the products agree and the
    exclusion is stale, which fails the gate (drop it canonically).

The generator self-hashes in the manifest are verified against the local
gen_goldens.py / gen_extra_inputs.py, so tooling drift on either side is
caught here as well.

Exclusion format convention (parsed from EXCLUSIONS.md): one bullet per
input, starting with a backtick-quoted manifest path:

  - `seeds/example.html` -- nature of the divergence; reason excluded.

Stdlib only; needs the probe built first:
  bazel build //lib/html:html_parse_probe   (or ./tools/docker-test.sh build)
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_PROBE = os.path.join(ROOT, "bazel-bin", "lib", "html", "html_parse_probe")
DEFAULT_MANIFEST = os.path.join(HERE, "goldens", "manifest.json")
DEFAULT_EXCLUSIONS = os.path.join(HERE, "EXCLUSIONS.md")
EXTRA_GENERATOR = os.path.join(HERE, "gen_extra_inputs.py")

# Probe exit codes per SPEC.md SS4.
STATUS_BY_RC = {0: "ok", 3: "size-limit"}

EXCLUSION_RE = re.compile(r"^-\s+`((?:seeds|generated)/[^`]+)`")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    with open(path, "rb") as f:
        return sha256_bytes(f.read())


def load_exclusions(path):
    excluded = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = EXCLUSION_RE.match(line.strip())
            if m:
                excluded.add(m.group(1))
    return excluded


def input_fs_path(manifest_path, gen_dir):
    rel_dir, name = manifest_path.split("/", 1)
    if rel_dir == "seeds":
        return os.path.join(HERE, "seeds", name)
    return os.path.join(gen_dir, name)


def run_probe(probe, in_path):
    """Run the probe; return (status, output_sha256_or_None)."""
    proc = subprocess.run(
        [probe, in_path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )
    rc = proc.returncode
    if rc in STATUS_BY_RC:
        status = STATUS_BY_RC[rc]
    elif rc < 0:
        status = f"signal-{-rc}"
    else:
        status = f"error-{rc}"
    out_sha = sha256_bytes(proc.stdout) if proc.stdout else None
    return status, out_sha


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--probe",
        default=DEFAULT_PROBE,
        help="path to the built html_parse_probe binary",
    )
    p.add_argument(
        "--manifest",
        default=DEFAULT_MANIFEST,
        help="path of the vendored goldens manifest",
    )
    p.add_argument(
        "--exclusions", default=DEFAULT_EXCLUSIONS, help="path of the exclusions file"
    )
    args = p.parse_args(argv)

    if not os.path.exists(args.probe):
        print(
            f"error: probe not found at {args.probe}\n"
            "build it first: bazel build //lib/html:html_parse_probe",
            file=sys.stderr,
        )
        return 2

    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    excluded = load_exclusions(args.exclusions)

    failures = []

    # Tooling drift guard: the manifest pins the generator versions.
    gen_sha = manifest.get("generator_sha256", {})
    for name, fs_path in (
        ("gen_goldens.py", os.path.join(HERE, "gen_goldens.py")),
        ("gen_extra_inputs.py", EXTRA_GENERATOR),
    ):
        want = gen_sha.get(name)
        got = sha256_file(fs_path)
        if want != got:
            failures.append(
                f"generator drift: {name} sha256 {got}, manifest pins {want} "
                "(re-sync the vendored corpus)"
            )

    known_inputs = {e["input"] for e in manifest["entries"]}
    for path in sorted(excluded - known_inputs):
        failures.append(f"exclusion names a non-manifest input: {path}")

    compared = excluded_ok = 0
    with tempfile.TemporaryDirectory(prefix="html-conformance-") as tmp:
        gen_dir = os.path.join(tmp, "generated")
        subprocess.run(
            [sys.executable, EXTRA_GENERATOR, "--dest", gen_dir],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        for entry in manifest["entries"]:
            manifest_path = entry["input"]
            fs_path = input_fs_path(manifest_path, gen_dir)
            got_input_sha = sha256_file(fs_path)
            if got_input_sha != entry["input_sha256"]:
                failures.append(
                    "input drift: {} sha256 {}, manifest pins {}".format(
                        manifest_path, got_input_sha, entry["input_sha256"]
                    )
                )
                continue
            status, out_sha = run_probe(args.probe, fs_path)
            if manifest_path in excluded:
                if status not in STATUS_BY_RC.values():
                    failures.append(
                        f"excluded input {manifest_path} crashed or errored under the "
                        f"probe: status {status}"
                    )
                elif status == entry["status"] and out_sha == entry["output_sha256"]:
                    failures.append(
                        f"stale exclusion: {manifest_path} now matches the shared golden; "
                        "drop it from EXCLUSIONS.md (canonically, in the "
                        "1.15 repo)"
                    )
                else:
                    excluded_ok += 1
                continue
            compared += 1
            if status != entry["status"]:
                failures.append(
                    "status mismatch: {} probe={} manifest={}".format(
                        manifest_path, status, entry["status"]
                    )
                )
            elif out_sha != entry["output_sha256"]:
                failures.append(
                    "output mismatch: {} probe={} manifest={}".format(
                        manifest_path, out_sha, entry["output_sha256"]
                    )
                )

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        print(
            f"error: {len(failures)} conformance failure(s); "
            f"{compared} inputs compared, {excluded_ok} excluded "
            "(divergent as expected)",
            file=sys.stderr,
        )
        return 1
    print(
        f"manifest conformance ok: {compared} inputs compared, "
        f"{excluded_ok} excluded (divergent as expected)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
