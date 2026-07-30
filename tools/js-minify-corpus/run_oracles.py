#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
"""Oracle harness for the tokenizer-based JS minifier corpus stress run.

For every corpus file:

  1. Classify goal (script vs module): synthetic index.json metadata for
     generated files, extension/name/content heuristics for real-world files.
     Reclassify on parse failure (script<->module) so a wrong first guess is
     not reported as a minifier bug.
  2. Verify the INPUT parses in node (`node --check`); inputs that parse
     under neither goal are recorded as input-unparseable (a generator or
     corpus issue, not a minifier finding) and skipped for output oracles.
  3. Run js_minify_probe --mode=tokenizer: record ok / unmodelable (exit 1,
     the byte-preserving pass-through contract) / crash (signal).
  4. If minified: node --check the OUTPUT (oracle a: output must parse).
  5. Idempotence: minify the minified output again; bytes must be identical
     (oracle c).
  6. Semantic equivalence (synthetic exec probes only): run original and
     minified in node, diff stdout + exit code (oracle b).

Output: JSONL per-file records + a summary.json + SUMMARY.md in the report
directory. Stdlib only; node and the probe binary are external.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile

NODE = os.environ.get("NODE", "node")

RE_MODULE_HINT = re.compile(
    rb"(?m)^\s*(import|export)\s+[\w{*'\"]|import\s*\.\s*meta")
RE_NAME_MODULE = re.compile(r"\.(mjs|module\.js|esm\.js|esm-bundler\.js)$")


def first_error_line(stderr):
    for line in stderr.splitlines():
        line = line.strip()
        if line and not line.startswith("at "):
            return line[:300]
    return ""


def node_check(data, goal, tmpdir):
    """Syntax-check bytes under the given goal. Returns (ok, error)."""
    suffix = ".mjs" if goal == "module" else ".js"
    fd, path = tempfile.mkstemp(prefix="check", suffix=suffix, dir=tmpdir)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        r = subprocess.run([NODE, "--check", path],
                           capture_output=True, text=True, timeout=60)
        if r.returncode == 0:
            return True, ""
        return False, first_error_line(r.stderr)
    except subprocess.TimeoutExpired:
        return False, "node --check timed out"
    finally:
        os.unlink(path)


def node_run(path, timeout=15):
    """Run a file in node. Returns (rc, stdout_bytes, timed_out)."""
    try:
        r = subprocess.run([NODE, path], capture_output=True, timeout=timeout)
        return r.returncode, r.stdout[:200000], False
    except subprocess.TimeoutExpired:
        return None, b"", True


def run_probe(probe, mode, in_path, out_path):
    """Returns dict(rc, out_bytes). rc<0 means killed by signal -rc."""
    r = subprocess.run([probe, "--mode=" + mode, in_path, out_path],
                       capture_output=True, timeout=120)
    out_bytes = os.path.getsize(out_path) if os.path.exists(out_path) else -1
    return {"rc": r.returncode, "out_bytes": out_bytes,
            "stderr": r.stderr.decode("utf-8", "replace")[:200]}


def classify(path, data, synthetic_meta):
    if synthetic_meta is not None:
        return "module" if synthetic_meta.get("module") else "script"
    base = os.path.basename(path)
    if base.endswith(".mjs") or RE_NAME_MODULE.search(base):
        return "module"
    if RE_MODULE_HINT.search(data):
        return "module"
    return "script"


def analyze_file(path, probe, work_dir, tmpdir, meta):
    """Full oracle pipeline for one corpus file. Returns a JSON record."""
    rec = {"file": path}
    with open(path, "rb") as f:
        data = f.read()
    rec["in_bytes"] = len(data)
    rec["goal"] = classify(path, data, meta)
    if meta is not None:
        rec["kind"] = meta.get("kind")
        rec["families"] = meta.get("families")
        rec["executable"] = bool(meta.get("executable"))
    else:
        rec["kind"] = "real"
        rec["executable"] = False

    # --- input parse (with goal reclassification fallback) ---------------
    ok, err = node_check(data, rec["goal"], tmpdir)
    if not ok:
        other = "module" if rec["goal"] == "script" else "script"
        ok2, _ = node_check(data, other, tmpdir)
        if ok2:
            rec["reclassified"] = "%s->%s" % (rec["goal"], other)
            rec["goal"] = other
            ok = True
    rec["input_parse"] = ok
    if not ok:
        rec["input_error"] = err

    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.relpath(path))
    tok_out = os.path.join(work_dir, safe + ".tok.js")
    tok_out2 = os.path.join(work_dir, safe + ".tok2.js")

    # --- tokenizer path ---------------------------------------------------
    tok = run_probe(probe, "tokenizer", path, tok_out)
    rec["tokenizer"] = ("ok" if tok["rc"] == 0 else
                        "unmodelable" if tok["rc"] == 1 else
                        "crash" if tok["rc"] < 0 else "probe-error")
    if tok["rc"] < 0:
        rec["crash_signal"] = -tok["rc"]
    rec["tok_out_bytes"] = tok["out_bytes"]
    if tok["rc"] == 0:
        with open(tok_out, "rb") as f:
            tok_data = f.read()
        pok, perr = node_check(tok_data, rec["goal"], tmpdir)
        rec["tok_output_parse"] = pok
        if not pok:
            rec["tok_output_error"] = perr
        # idempotence
        tok2 = run_probe(probe, "tokenizer", tok_out, tok_out2)
        if tok2["rc"] == 0:
            with open(tok_out2, "rb") as f:
                rec["idempotent"] = f.read() == tok_data
        else:
            rec["idempotent"] = False
            rec["idempotence_note"] = "second pass rc=%d" % tok2["rc"]

    # --- semantic equivalence (executable probes only) --------------------
    if rec["executable"] and rec["input_parse"]:
        golden_rc, golden_out, golden_to = node_run(path)
        rec["run_input"] = {"rc": golden_rc, "timeout": golden_to}
        if tok["rc"] == 0 and rec.get("tok_output_parse"):
            min_rc, min_out, min_to = node_run(tok_out)
            if min_to or golden_to:
                rec["semantic"] = "timeout"
            elif min_rc != golden_rc or min_out != golden_out:
                rec["semantic"] = "diff"
                rec["semantic_detail"] = {
                    "input_rc": golden_rc, "min_rc": min_rc,
                    "input_stdout": golden_out[:300].decode("utf-8", "replace"),
                    "min_stdout": min_out[:300].decode("utf-8", "replace"),
                }
            else:
                rec["semantic"] = "match"
    return rec


def load_synthetic_meta(synthetic_dir):
    idx = os.path.join(synthetic_dir, "index.json")
    if not os.path.exists(idx):
        return {}
    with open(idx) as f:
        data = json.load(f)
    return {os.path.join(synthetic_dir, e["file"]): e
            for e in data["files"]}


def collect_files(corpus_dirs):
    files = []
    for d in corpus_dirs:
        for root, _, names in os.walk(d):
            for n in sorted(names):
                if n.endswith((".js", ".mjs", ".cjs")):
                    files.append(os.path.join(root, n))
    return sorted(files)


def summarize(records):
    s = {"files": len(records),
         "in_bytes": sum(r["in_bytes"] for r in records)}
    by = {}

    def bump(field, key):
        by.setdefault(field, {}).setdefault(key, 0)
        by[field][key] += 1

    for r in records:
        bump("goal", r["goal"])
        bump("kind", r.get("kind", "real"))
        bump("tokenizer", r["tokenizer"])
        if not r["input_parse"]:
            bump("input_parse", "fail")
    s["by"] = by
    tok_ok = [r for r in records if r["tokenizer"] == "ok"]
    s["tok_parse_failures"] = [
        {"file": r["file"], "goal": r["goal"], "error": r["tok_output_error"]}
        for r in tok_ok if not r.get("tok_output_parse")]
    s["tok_semantic_diffs"] = [
        {"file": r["file"], "detail": r["semantic_detail"]}
        for r in records if r.get("semantic") == "diff"]
    s["tok_non_idempotent"] = [
        {"file": r["file"], "note": r.get("idempotence_note", "")}
        for r in tok_ok if r.get("idempotent") is False]
    s["crashes"] = [
        {"file": r["file"], "signal": r.get("crash_signal")}
        for r in records if r["tokenizer"] == "crash"]
    s["input_unparseable"] = [
        {"file": r["file"], "error": r.get("input_error", "")}
        for r in records if not r["input_parse"]]
    # Size stats over tokenizer-minified files.
    if tok_ok:
        ratios = sorted(r["tok_out_bytes"] / max(1, r["in_bytes"])
                        for r in tok_ok)
        s["tok_size_ratio_median"] = ratios[len(ratios) // 2]
    s["pass_through_rate"] = (
        by.get("tokenizer", {}).get("unmodelable", 0) / max(1, len(records)))
    return s


def write_summary_md(path, s, records):
    lines = []
    a = lines.append
    a("# JS minifier corpus oracle summary")
    a("")
    a("- files: %d (%.1f MB)" % (s["files"], s["in_bytes"] / 1e6))
    a("- goals: %s" % s["by"].get("goal", {}))
    a("- kinds: %s" % s["by"].get("kind", {}))
    a("- tokenizer: %s" % s["by"].get("tokenizer", {}))
    a("- pass-through rate (tokenizer unmodelable): %.1f%%"
      % (100 * s["pass_through_rate"]))
    if "tok_size_ratio_median" in s:
        a("- median minified/input size ratio: %.3f"
          % s["tok_size_ratio_median"])
    a("")
    for key, title in [
            ("tok_parse_failures", "Tokenizer output parse failures"),
            ("tok_semantic_diffs", "Semantic diffs (stdout mismatch)"),
            ("tok_non_idempotent", "Non-idempotent outputs"),
            ("crashes", "Crashes"),
            ("input_unparseable", "Inputs unparseable (corpus issues)")]:
        entries = s[key]
        a("## %s: %d" % (title, len(entries)))
        for e in entries[:40]:
            a("- `%s` %s" % (e["file"],
                             e.get("error") or e.get("note") or e.get("detail")
                             or ""))
        if len(entries) > 40:
            a("- ... and %d more" % (len(entries) - 40))
        a("")
    with open(path, "w") as f:
        f.write("\n".join(lines))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--probe", required=True, help="js_minify_probe binary")
    p.add_argument("--corpus", nargs="+", required=True)
    p.add_argument("--report", required=True)
    p.add_argument("--work", default=None, help="scratch dir for outputs")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--gate", action="store_true",
                   help="exit 1 if any correctness oracle (parse failure, "
                        "semantic diff, non-idempotence, crash) is non-zero. "
                        "Coverage stats (unmodelable, divergence, input-bad) "
                        "stay report-only and never affect the exit code.")
    args = p.parse_args(argv)

    os.makedirs(args.report, exist_ok=True)
    work = args.work or os.path.join(args.report, "work")
    os.makedirs(work, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="jsmc-node")

    meta = {}
    for d in args.corpus:
        meta.update(load_synthetic_meta(d))
    files = collect_files(args.corpus)
    if args.limit:
        files = files[:args.limit]
    print("oracle run: %d files, probe=%s" % (len(files), args.probe))

    records = []
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.jobs) as pool:
        futs = {pool.submit(analyze_file, f, args.probe, work, tmpdir,
                            meta.get(f)): f for f in files}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            records.append(fut.result())
            done += 1
            if done % 50 == 0:
                print("  %d/%d" % (done, len(files)), flush=True)
    records.sort(key=lambda r: r["file"])

    with open(os.path.join(args.report, "records.jsonl"), "w") as f:
        for r in records:
            f.write(json.dumps(r, sort_keys=True) + "\n")
    s = summarize(records)
    with open(os.path.join(args.report, "summary.json"), "w") as f:
        json.dump(s, f, indent=1, sort_keys=True)
        f.write("\n")
    write_summary_md(os.path.join(args.report, "SUMMARY.md"), s, records)
    print("parse-fail=%d semantic-diff=%d non-idempotent=%d crash=%d "
          "unmodelable=%d input-bad=%d" % (
              len(s["tok_parse_failures"]), len(s["tok_semantic_diffs"]),
              len(s["tok_non_idempotent"]), len(s["crashes"]),
              s["by"].get("tokenizer", {}).get("unmodelable", 0),
              len(s["input_unparseable"])))
    print("report: %s" % os.path.join(args.report, "SUMMARY.md"))
    if args.gate:
        correctness_failures = (len(s["tok_parse_failures"]) +
                                len(s["tok_semantic_diffs"]) +
                                len(s["tok_non_idempotent"]) +
                                len(s["crashes"]))
        if correctness_failures:
            print("GATE FAILED: %d correctness failure(s) — see %s" % (
                correctness_failures, os.path.join(args.report, "SUMMARY.md")))
            return 1
        print("gate: 0 correctness failures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
