#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Assert the pinned optimizer release links the same cache library we do.

The release lane already checks that `install/debian/optimizer-pin.json` names
the tag this release needs. That is a string comparison, and it cannot see the
thing that actually breaks a serving pair: the two halves linking DIFFERENT
cache library commits.

Why that is fatal rather than untidy. The cache volume's filename carries the
library's on-disk format major. The daemon owns the volume; the module attaches
to it. A module built against a different major resolves to a different
filename, so its startup probe CREATES a second volume file beside the daemon's
-- which is exactly the condition `DaemonAdapter::Resolve` refuses to start on.
A format-skewed pair therefore ships an Apache that does not come up at all, on
every daemon-adapter deployment, until someone changes the pin. Nothing else in
the release lane looks at this.

So: read the cache-library commit this module builds, read the one the pinned
daemon commit builds, and refuse if they differ. Usage:

    tools/ci/check_optimizer_pin_pair.py <pin.json> <optimizer MODULE.bazel> \\
        [bazel/repositories.bzl]
    tools/ci/check_optimizer_pin_pair.py --self-test
"""

import re
import sys
import tempfile
import os

CYCLONE_RE = re.compile(r'^\s*CYCLONE_COMMIT\s*=\s*"([0-9a-f]{40})"', re.M)
# The optimizer declares the library as a Bzlmod repo rule; take the `commit`
# from inside the call whose `name` is cyclone, never the first commit in the
# file (MODULE.bazel pins many repositories).
OPTIMIZER_RE = re.compile(
    r'name\s*=\s*"cyclone"[^)]*?commit\s*=\s*"([0-9a-f]{40})"', re.S)


class PinError(Exception):
    pass


def module_cyclone_commit(repositories_bzl_text):
    m = CYCLONE_RE.search(repositories_bzl_text)
    if not m:
        raise PinError("could not read CYCLONE_COMMIT from bazel/repositories.bzl")
    return m.group(1)


def optimizer_cyclone_commit(module_bazel_text, mps2_commit):
    m = OPTIMIZER_RE.search(module_bazel_text)
    if not m:
        raise PinError(
            "could not find the cyclone pin in the optimizer's MODULE.bazel at "
            "%s -- the daemon repo moved its pin somewhere this gate cannot "
            "read. Fix the gate rather than skipping it." % mps2_commit)
    return m.group(1)


def check(pin, repositories_bzl_text, module_bazel_text):
    tag = pin.get("tag", "")
    mps2_commit = pin.get("mps2_commit", "")
    if not mps2_commit or mps2_commit.startswith("PLACEHOLDER"):
        raise PinError(
            "optimizer-pin.json has no mps2_commit (tag %r) -- record the "
            "daemon commit the pinned release was built from." % tag)
    ours = module_cyclone_commit(repositories_bzl_text)
    theirs = optimizer_cyclone_commit(module_bazel_text, mps2_commit)
    if ours != theirs:
        raise PinError(
            "cache-library pin mismatch: this module builds %s but the pinned "
            "optimizer release %s (daemon commit %s) builds %s. The pair must "
            "link the SAME commit -- a format skew ships a web server that "
            "refuses to start. Cut an optimizer release from a daemon commit "
            "at this cache-library pin, then re-record "
            "install/debian/optimizer-pin.json." % (ours, tag, mps2_commit, theirs))
    return ours


OPTIMIZER_SAMPLE = '''
new_git_repository(
    name = "boringssl",
    commit = "1111111111111111111111111111111111111111",
)

new_git_repository(
    name = "cyclone",
    build_file = "//third_party:cyclone.BUILD",
    commit = "%s",
    remote = "git@github.com:We-Amp/cyclone-cache.git",
)
'''

BZL_SAMPLE = 'CYCLONE_COMMIT = "%s"\n'
A = "6286a068182221a7e089c319a259b90ed15a4a31"
B = "edecf16a4c07fbd5f07ba1c7121b9fcacfc2da07"


def self_test():
    cases = []

    def case(name, ok, fn):
        try:
            fn()
            got = True
            why = ""
        except PinError as exc:
            got = False
            why = str(exc)
        cases.append((name, ok == got, why))

    case("matching pair passes", True, lambda: check(
        {"tag": "optimizer-v1.16.0", "mps2_commit": "deadbeef"},
        BZL_SAMPLE % A, OPTIMIZER_SAMPLE % A))
    case("skewed pair is refused", False, lambda: check(
        {"tag": "optimizer-v1.16.0", "mps2_commit": "deadbeef"},
        BZL_SAMPLE % A, OPTIMIZER_SAMPLE % B))
    case("placeholder mps2_commit is refused", False, lambda: check(
        {"tag": "PLACEHOLDER-x", "mps2_commit": "PLACEHOLDER"},
        BZL_SAMPLE % A, OPTIMIZER_SAMPLE % A))
    case("missing mps2_commit is refused", False, lambda: check(
        {"tag": "optimizer-v1.16.0"}, BZL_SAMPLE % A, OPTIMIZER_SAMPLE % A))
    case("unreadable optimizer pin is refused", False, lambda: check(
        {"tag": "optimizer-v1.16.0", "mps2_commit": "deadbeef"},
        BZL_SAMPLE % A, "bazel_dep(name = 'rules_cc')\n"))
    # The one that matters most: the FIRST commit in the optimizer's file
    # belongs to another repository, and must never be read as cyclone's.
    case("another repo's commit is not mistaken for cyclone's", False,
         lambda: check({"tag": "t", "mps2_commit": "deadbeef"},
                       BZL_SAMPLE % "1111111111111111111111111111111111111111",
                       OPTIMIZER_SAMPLE % A))

    failed = [c for c in cases if not c[1]]
    for name, ok, why in cases:
        print("  %-52s %s" % (name, "ok" if ok else "FAIL"))
        if not ok and why:
            print("      %s" % why)
    if failed:
        print("FAIL: %d of %d self-test cases" % (len(failed), len(cases)))
        return 1
    print("OK: all %d self-test cases behave as specified." % len(cases))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    import json
    pin_path, module_path = argv[1], argv[2]
    bzl_path = argv[3] if len(argv) == 4 else "bazel/repositories.bzl"
    with open(pin_path) as f:
        pin = json.load(f)
    with open(bzl_path) as f:
        bzl = f.read()
    with open(module_path) as f:
        mod = f.read()
    try:
        commit = check(pin, bzl, mod)
    except PinError as exc:
        print("::error::%s" % exc, file=sys.stderr)
        return 1
    print("optimizer pair links the same cache library commit: %s" % commit)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
