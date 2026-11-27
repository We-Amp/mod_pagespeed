#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Fail on an equality test against the serve-variants HTML cache mode.

DaemonHtmlCache modes are ordered: every mode from serve-variants on stores
pages and serves their Markdown copies (serve-cached-html also serves the
stored page itself). Code that asks `mode == serve-variants` or
`mode != serve-variants` quietly treats every later mode as "does not serve",
which is how a new mode ends up building a report-only engine on one server
and not on another. Ask DaemonHtmlCacheServes(mode) (or compare with `>=` /
`<`) instead.

This scans the C and C++ sources git tracks for `==` or `!=` next to
kDaemonHtmlCacheServeVariants or DaemonHtmlMode::kServeVariants, across line
breaks, and fails on any hit outside ALLOW.

Usage:

    tools/ci/check_daemon_html_mode_audit.py [<repo root>]
    tools/ci/check_daemon_html_mode_audit.py --self-test
"""

import os
import re
import subprocess
import sys
import tempfile

# path -> why an equality test there is right.  Empty: no site needs one.
ALLOW = {}

EXTENSIONS = (".cc", ".cpp", ".h", ".hpp", ".c")

_NAME = r"(?:\w+\s*::\s*)*k(?:DaemonHtmlCache)?ServeVariants\b"
PATTERN = re.compile(r"[!=]=\s*" + _NAME + r"|\b" + _NAME + r"\s*[!=]=")


def _strip_comments(text):
    # Keep the line count: a comment becomes spaces and its newlines.
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


def scan_text(path, text):
    """[(path, line, snippet)] for every equality test in `text`."""
    hits = []
    code = _strip_comments(text)
    for m in PATTERN.finditer(code):
        line = code.count("\n", 0, m.start()) + 1
        hits.append((path, line, " ".join(m.group(0).split())))
    return hits


def tracked_sources(root):
    out = subprocess.check_output(["git", "-C", root, "ls-files", "-z"])
    for name in out.decode("utf-8", "replace").split("\0"):
        if name.endswith(EXTENSIONS):
            yield name


def check(root, files=None):
    hits = []
    for rel in (files if files is not None else tracked_sources(root)):
        if rel in ALLOW:
            continue
        path = os.path.join(root, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue  # a tracked file deleted in the work tree
        hits.extend(scan_text(rel, text))
    return hits


def self_test():
    cases = 0
    caught = [
        "if (mode == RewriteOptions::kDaemonHtmlCacheServeVariants) {}",
        "if (o->daemon_html_cache() !=\n    RewriteOptions::kDaemonHtmlCacheServeVariants)",
        "x = RewriteOptions::kDaemonHtmlCacheServeVariants == m;",
        "if (config().mode == DaemonHtmlMode::kServeVariants) {}",
        "b = kDaemonHtmlCacheServeVariants!=mode;",
        "if (mode == net_instaweb :: RewriteOptions :: kDaemonHtmlCacheServeVariants)",
    ]
    for text in caught:
        assert len(scan_text("f.cc", text)) == 1, text
        cases += 1
    passed = [
        "if (mode >= RewriteOptions::kDaemonHtmlCacheServeVariants) {}",
        "if (config_.mode < DaemonHtmlMode::kServeVariants) return;",
        "case RewriteOptions::kDaemonHtmlCacheServeVariants:",
        "*value = kDaemonHtmlCacheServeVariants;",
        "EXPECT_EQ(RewriteOptions::kDaemonHtmlCacheServeVariants, m);",
        "// mode == kDaemonHtmlCacheServeVariants is what not to write",
        "/* mode != DaemonHtmlMode::kServeVariants */",
        "if (mode == kDaemonHtmlCacheServeVariantsLegacy) {}",
    ]
    for text in passed:
        assert scan_text("f.cc", text) == [], text
        cases += 1
    # The line number survives a comment that spans lines.
    hits = scan_text("f.cc", "/* a\nb */\nif (m == kServeVariants) {}")
    assert hits == [("f.cc", 3, "== kServeVariants")], hits
    cases += 1
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "a.cc"), "w", encoding="utf-8") as f:
            f.write("bool s(int m) { return m != kServeVariants; }\n")
        with open(os.path.join(d, "b.cc"), "w", encoding="utf-8") as f:
            f.write("bool s(int m) { return m >= kServeVariants; }\n")
        assert len(check(d, ["a.cc", "b.cc"])) == 1
        ALLOW["a.cc"] = "self-test"
        try:
            assert check(d, ["a.cc", "b.cc"]) == []
        finally:
            del ALLOW["a.cc"]
        cases += 2
    print("check_daemon_html_mode_audit self-test: %d cases passed" % cases)
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) > 1:
        print(__doc__, file=sys.stderr)
        return 2
    root = argv[0] if argv else "."
    hits = check(root)
    for path, line, snippet in hits:
        print("::error file=%s,line=%d::%s compares the HTML cache mode with "
              "serve-variants by equality; use DaemonHtmlCacheServes(mode) or "
              ">= so every later mode is accepted where serve-variants is" %
              (path, line, snippet))
    if hits:
        return 1
    print("No equality test against the serve-variants mode outside the "
          "allowlist")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
