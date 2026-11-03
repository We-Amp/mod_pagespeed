#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Differential run of two parser builds over an HTML corpus.

For every input file this records, for a BASE and a TIP build:

  (a) the parse event stream of html_parse_probe (SPEC.md v1), and
  (b) the HTML that static_rewriter writes with a fixed filter set:
      CoreFilters plus collapse_whitespace, inline_import_to_link,
      outline_css and rewrite_css.

Inputs are read from files; the rewriter's fetcher is expected to have no
network (run this inside a container started with --network none), so no
resource is fetched and the output depends on the input bytes only.

Files whose bytes contain no "<svg" or "<math" start tag (the name followed
by white space, "/" or ">", ASCII case-insensitive) must give byte-identical (a) and (b) on both sides; any
difference there makes the run fail. Every difference on the other files is
listed, with unified diffs of the event streams and the rewritten HTML, for
review.

Corpus:
  * every .html/.htm file under install/, test/ and
    tools/html-parse-corpus/seeds/ of --repo;
  * the inputs gen_extra_inputs.py generates;
  * every .html/.htm file under each --extra-root (read only).

Each bin directory holds html_parse_probe and static_rewriter, copied from
bazel-bin of the build being compared:

  differential.py --base-bin B --tip-bin T --repo . \\
      --extra-root ../pagespeed-optimizer --out OUT

Writes OUT/report.txt (counts and the per-file list) and OUT/diffs/.
Exit status: 0 when every file without SVG or MathML is identical, 1 when
one is not, 2 on a harness error.
"""

import argparse
import concurrent.futures
import difflib
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRA_GENERATOR = os.path.join(HERE, "gen_extra_inputs.py")
HTML_EXTS = (".html", ".htm")
REPO_DIRS = ["install", "test", os.path.join("tools", "html-parse-corpus",
                                             "seeds")]
SKIP_DIRS = {".git", "node_modules"}
FOREIGN_RE = re.compile(rb"<(svg|math)[\s/>]", re.IGNORECASE)
REWRITER_FLAGS = [
    "--rewrite_level=CoreFilters",
    "--rewriters=collapse_whitespace,inline_import_to_link,outline_css,"
    "rewrite_css",
]
TIMEOUT_SEC = 300


def walk_html(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames
                             if d not in SKIP_DIRS and not d.startswith("bazel-"))
        for name in sorted(filenames):
            if name.lower().endswith(HTML_EXTS):
                path = os.path.join(dirpath, name)
                if os.path.isfile(path) and not os.path.islink(path):
                    yield path


def collect_inputs(repo, extra_roots, gen_dir):
    inputs = []
    for rel in REPO_DIRS:
        base = os.path.join(repo, rel)
        for path in walk_html(base):
            inputs.append(("repo/" + os.path.relpath(path, repo), path))
    subprocess.run([sys.executable, EXTRA_GENERATOR, "--dest", gen_dir],
                   check=True, stdout=subprocess.DEVNULL)
    for path in walk_html(gen_dir):
        inputs.append(("generated/" + os.path.relpath(path, gen_dir), path))
    for root in extra_roots:
        label = os.path.basename(os.path.normpath(root))
        for path in walk_html(root):
            inputs.append((label + "/" + os.path.relpath(path, root), path))
    return sorted(inputs)


def run(cmd):
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=TIMEOUT_SEC)
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired:
        return "timeout", b""


def probe(bin_dir, path):
    rc, out = run([os.path.join(bin_dir, "html_parse_probe"), path])
    return rc, out


def rewrite(bin_dir, path, tmp_root):
    out_dir = tempfile.mkdtemp(dir=tmp_root)
    name = os.path.basename(path)
    rc, _ = run([os.path.join(bin_dir, "static_rewriter")] + REWRITER_FLAGS +
                [os.path.dirname(path), out_dir, name])
    out_path = os.path.join(out_dir, name)
    data = b""
    if os.path.exists(out_path):
        with open(out_path, "rb") as f:
            data = f.read()
    for entry in os.listdir(out_dir):
        os.remove(os.path.join(out_dir, entry))
    os.rmdir(out_dir)
    return rc, data


def one(label, path, base_bin, tip_bin, tmp_root):
    with open(path, "rb") as f:
        foreign = FOREIGN_RE.search(f.read()) is not None
    result = {"label": label, "path": path, "foreign": foreign}
    for side, bin_dir in (("base", base_bin), ("tip", tip_bin)):
        result[side + "_probe"] = probe(bin_dir, path)
        result[side + "_rewrite"] = rewrite(bin_dir, path, tmp_root)
    return result


def text_diff(a, b, name_a, name_b):
    return "".join(difflib.unified_diff(
        a.decode("utf-8", "backslashreplace").splitlines(keepends=True),
        b.decode("utf-8", "backslashreplace").splitlines(keepends=True),
        fromfile=name_a, tofile=name_b))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--base-bin", required=True)
    p.add_argument("--tip-bin", required=True)
    p.add_argument("--repo", required=True)
    p.add_argument("--extra-root", action="append", default=[])
    p.add_argument("--out", required=True)
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = p.parse_args(argv)

    for bin_dir in (args.base_bin, args.tip_bin):
        for tool in ("html_parse_probe", "static_rewriter"):
            if not os.access(os.path.join(bin_dir, tool), os.X_OK):
                print("error: %s missing in %s" % (tool, bin_dir),
                      file=sys.stderr)
                return 2

    os.makedirs(os.path.join(args.out, "diffs"), exist_ok=True)
    tmp_root = tempfile.mkdtemp(dir=args.out, prefix="tmp-")
    gen_dir = os.path.join(tmp_root, "generated")
    inputs = collect_inputs(args.repo, args.extra_root, gen_dir)

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(
            lambda item: one(item[0], item[1], args.base_bin, args.tip_bin,
                             tmp_root), inputs))

    counts = {"files": 0, "identical": 0, "differ_plain": 0,
              "differ_foreign": 0, "foreign": 0}
    lines = []
    for n, r in enumerate(results):
        counts["files"] += 1
        counts["foreign"] += r["foreign"]
        what = []
        if r["base_probe"] != r["tip_probe"]:
            what.append("events")
        if r["base_rewrite"] != r["tip_rewrite"]:
            what.append("rewrite")
        if not what:
            counts["identical"] += 1
            continue
        kind = "foreign" if r["foreign"] else "PLAIN"
        counts["differ_foreign" if r["foreign"] else "differ_plain"] += 1
        diff_name = "%04d.diff" % n
        with open(os.path.join(args.out, "diffs", diff_name), "w",
                  encoding="utf-8") as f:
            f.write("input: %s\n" % r["label"])
            for part, key in (("events", "probe"), ("rewrite", "rewrite")):
                (brc, bout), (trc, tout) = r["base_" + key], r["tip_" + key]
                f.write("== %s: base rc=%s tip rc=%s\n" % (part, brc, trc))
                f.write(text_diff(bout, tout, "base", "tip"))
        lines.append("%s %s %s diffs/%s" % (kind, ",".join(what), r["label"],
                                            diff_name))

    summary = ("files=%(files)d with_svg_or_math=%(foreign)d "
               "identical=%(identical)d differ_with_svg_or_math="
               "%(differ_foreign)d differ_without=%(differ_plain)d" % counts)
    with open(os.path.join(args.out, "report.txt"), "w",
              encoding="utf-8") as f:
        f.write(summary + "\n")
        for line in lines:
            f.write(line + "\n")
    for root, dirs, files in os.walk(tmp_root, topdown=False):
        for name in files:
            os.remove(os.path.join(root, name))
        for name in dirs:
            os.rmdir(os.path.join(root, name))
    os.rmdir(tmp_root)
    print(summary)
    for line in lines:
        print(line)
    return 1 if counts["differ_plain"] else 0


if __name__ == "__main__":
    sys.exit(main())
