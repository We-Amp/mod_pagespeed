#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Greedy chunk minimizer for minifier failure repros.

Given a corpus file and a failure kind, reduce the input while PRESERVING
the failure, using a simple ddmin-style pass: try removing halves, then
quarters, ... at line granularity, then do the same at byte granularity,
then finish with a line-by-line cleanup sweep. Bounded by --max-evals so a
run stays time-boxed; the smallest failing candidate found wins.

Failure kinds (the "oracle" the reduction preserves):
  parse        input parses, but tokenizer-minified output does not
               (same goal for both checks)
  unmodelable  input parses, but the tokenizer probe exits 1 (the
               byte-preserving pass-through path; minimizes the trigger)
  semantic     input runs in node and tokenizer-minified output runs, but
               stdout/exit-code differ (or minified no longer parses/runs)
  idempotence  minify(minify(x)) != minify(x)

Usage:
  minimize.py --probe <bin> --kind parse|unmodelable|semantic|idempotence \
      [--goal script|module] <input.js> <repro-out.js>

Stdlib only.
"""

import argparse
import os
import subprocess
import sys
import tempfile

NODE = os.environ.get("NODE", "node")


class Oracle(object):
    def __init__(self, probe, kind, goal, workdir):
        self.probe = probe
        self.kind = kind
        self.goal = goal
        self.workdir = workdir
        self.evals = 0
        self.max_evals = 400
        self._n = 0

    def _paths(self, data):
        self._n += 1
        src = os.path.join(self.workdir, "cand-%d.js" % self._n)
        with open(src, "wb") as f:
            f.write(data)
        return src

    def node_check(self, path):
        goal_path = path
        if self.goal == "module":
            goal_path = path + ".mjs"
            os.replace(path, goal_path)
        r = subprocess.run([NODE, "--check", goal_path],
                           capture_output=True, timeout=60)
        return r.returncode == 0, goal_path

    def minify(self, src):
        out = src + ".min"
        r = subprocess.run([self.probe, "--mode=tokenizer", src, out],
                           capture_output=True, timeout=120)
        return r.returncode, out

    def interesting(self, data):
        if self.evals >= self.max_evals:
            return False
        self.evals += 1
        src = self._paths(data)
        ok, src = self.node_check(src)
        if not ok:
            return False  # reduction must keep the input itself valid
        rc, min_path = self.minify(src)
        if self.kind == "unmodelable":
            return rc == 1  # input parses yet the tokenizer cannot model it
        if self.kind == "parse":
            if rc != 0:
                return False  # unmodelable pass-through is not the bug
            mok, min_path = self.node_check(min_path)
            return not mok
        if self.kind == "idempotence":
            if rc != 0:
                return False
            rc2, min2_path = self.minify(min_path)
            if rc2 != 0:
                return True  # second pass suddenly fails: non-idempotent
            with open(min_path, "rb") as a, open(min2_path, "rb") as b:
                return a.read() != b.read()
        # semantic
        if rc != 0:
            return False
        mok, min_path = self.node_check(min_path)
        def run(p):
            try:
                r = subprocess.run([NODE, p], capture_output=True, timeout=15)
                return r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                return "timeout", b""
        if not mok:
            return True  # minified no longer parses: meaning certainly lost
        return run(src) != run(min_path)


def chunks(items, n):
    """Split list into n near-equal contiguous chunks."""
    k, m = divmod(len(items), n)
    out, i = [], 0
    for c in range(n):
        size = k + (1 if c < m else 0)
        out.append(items[i:i + size])
        i += size
    return [c for c in out if c]


def ddmin(data, oracle, granularity):
    """Reduce `data` (list of units: lines or bytes) preserving failure."""
    n = 2
    while len(data) >= 2:
        if oracle.evals >= oracle.max_evals:
            break
        parts = chunks(data, min(n, len(data)))
        reduced = False
        for i in range(len(parts)):
            candidate = [u for j, p in enumerate(parts) if j != i
                         for u in p]
            blob = b"".join(candidate) if granularity == "bytes" \
                else "\n".join(candidate).encode("utf-8")
            if blob.strip() and oracle.interesting(blob):
                data = candidate
                n = max(2, n - 1)
                reduced = True
                break
        if not reduced:
            if n >= len(data):
                break
            n = min(len(data), n * 2)
    return data


def minimize(data, oracle):
    # Pass 1: line granularity (fast on big files).
    lines = data.decode("utf-8", "surrogateescape").split("\n")
    if len(lines) > 1:
        lines = ddmin(lines, oracle, "lines")
        data = "\n".join(lines).encode("utf-8", "surrogateescape")
    # Pass 2: line-by-line cleanup sweep.
    if len(lines) > 1:
        i = 0
        while i < len(lines) and oracle.evals < oracle.max_evals:
            candidate = lines[:i] + lines[i + 1:]
            blob = "\n".join(candidate).encode("utf-8", "surrogateescape")
            if blob.strip() and oracle.interesting(blob):
                lines = candidate
                data = blob
            else:
                i += 1
    # Pass 3: byte granularity ddmin on what is left (only when small).
    if len(data) <= 4000 and oracle.evals < oracle.max_evals:
        units = [bytes([b]) for b in data]
        units = ddmin(units, oracle, "bytes")
        data = b"".join(units)
    return data


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--probe", required=True)
    p.add_argument("--kind", choices=["parse", "unmodelable", "semantic", "idempotence"],
                   required=True)
    p.add_argument("--goal", choices=["script", "module"], default="script")
    p.add_argument("--max-evals", type=int, default=400)
    p.add_argument("input")
    p.add_argument("output")
    args = p.parse_args(argv)

    with open(args.input, "rb") as f:
        data = f.read()
    workdir = tempfile.mkdtemp(prefix="jsmc-min")
    oracle = Oracle(args.probe, args.kind, args.goal, workdir)
    oracle.max_evals = args.max_evals

    if not oracle.interesting(data):
        print("input does not reproduce the failure (kind=%s goal=%s)"
              % (args.kind, args.goal))
        return 1
    before = len(data)
    repro = minimize(data, oracle)
    with open(args.output, "wb") as f:
        f.write(repro)
    # Final confirmation.
    if not oracle.interesting(repro):
        print("WARNING: minimized candidate lost the failure; keeping last "
              "confirmed size")
    print("minimized %d -> %d bytes in %d evals -> %s"
          % (before, len(repro), oracle.evals, args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
