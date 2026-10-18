#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Assert the module release and its paired optimizer carry one version.

From 2.5.0 on every artifact of a release carries the same number: the module
packages, the X-Mod-Pagespeed header, the module-v / optimizer-v / v tags and
the optimizer's VERSION.txt. The release lane already checks that
`install/debian/optimizer-pin.json` names `optimizer-v<this version>` (a string
the pin records) and that the pair links one cache-library commit
(check_optimizer_pin_pair.py). Neither looks at the number the optimizer
itself reports: VERSION.txt at the pinned `optimizer_commit`, which its
packages, its --version and its health endpoint are generated from. This does.

Before 2.5.0 the module line was numbered 1.x while the optimizer's
VERSION.txt carried the product line (2.N == 1.(N+15): module 1.19.0 paired
with VERSION.txt 2.4.0). A hotfix on that line must still release, so the
equality is enforced only from 2.5.0 on; below it the check passes with a
notice. Dry-run tags (`-pre.N`, e.g. module-v1.19.1-pre.1) are not releases
and are ignored the same way.

A release candidate (module-v2.5.0-rc.1) is cut from an optimizer commit whose
VERSION.txt already names the release it leads to, so VERSION.txt may equal
either the full version or its X.Y.Z core.

Usage:

    tools/ci/check_module_optimizer_version.py --module-version V \\
        --optimizer-version-file FILE [--dry-run]
    tools/ci/check_module_optimizer_version.py --self-test

Exit 0 on pass (or a skip, with a notice), 1 on a mismatch (a warning and
exit 0 under --dry-run), 2 on bad input.
"""

import re
import sys

ENFORCED_SINCE = (2, 5, 0)
SEMVER_RE = re.compile(
    r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
    r'(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$')


class VersionError(Exception):
    pass


def parse(version):
    m = SEMVER_RE.match(version)
    if not m:
        raise VersionError(
            "'%s' is not X.Y.Z[-prerelease] (no build metadata)" % version)
    core = tuple(int(m.group(i)) for i in (1, 2, 3))
    return core, m.group(4) or ""


def is_dry_run_tag(prerelease):
    """`pre`, `pre.1`, `pre1`: the lane's dry-run tags, never a release."""
    return re.match(r'^pre(?:[.0-9]|$)', prerelease) is not None


def check(module_version, optimizer_version):
    """Return (verdict, message); verdict is 'pass', 'skip' or 'fail'."""
    core, pre = parse(module_version)
    if is_dry_run_tag(pre):
        return "skip", (
            "module %s is a dry-run tag (-%s); the version-equality guard "
            "ignores it" % (module_version, pre))
    if core < ENFORCED_SINCE:
        return "skip", (
            "module %s is below %s: the legacy 1.x numbering pairs it with an "
            "optimizer VERSION.txt of a different number; the equality is "
            "enforced from %s on" % (module_version,
                                     ".".join(map(str, ENFORCED_SINCE)),
                                     ".".join(map(str, ENFORCED_SINCE))))
    want_core = "%d.%d.%d" % core
    have = optimizer_version.strip()
    if have in (module_version, want_core):
        return "pass", (
            "module %s and the pinned optimizer's VERSION.txt %s agree"
            % (module_version, have))
    return "fail", (
        "module %s is paired with an optimizer whose VERSION.txt says '%s'. "
        "From %s on the module and the optimizer release one number: cut "
        "optimizer-v%s from an optimizer commit whose VERSION.txt is %s, "
        "record it in install/debian/optimizer-pin.json (tag, optimizer_commit, "
        "sha256sums_sha256), then tag the module."
        % (module_version, have, ".".join(map(str, ENFORCED_SINCE)),
           module_version, want_core))


CASES = [
    # module, optimizer VERSION.txt, verdict
    ("2.5.0", "2.5.0", "pass"),
    ("2.5.0", "2.5.0\n", "pass"),
    ("2.5.0", "2.4.0", "fail"),
    ("2.5.1", "2.5.0", "fail"),
    ("2.5.0-rc.1", "2.5.0", "pass"),
    ("2.5.0-rc.1", "2.5.0-rc.1", "pass"),
    ("2.5.0-rc.1", "2.4.0", "fail"),
    ("2.10.0", "2.10.0", "pass"),
    ("2.10.0", "2.1.0", "fail"),
    ("3.0.0", "2.9.0", "fail"),
    # legacy mapping: module 1.x paired with the 2.N product line
    ("1.19.0", "2.4.0", "skip"),
    ("1.19.1", "2.4.0", "skip"),
    ("1.19.1-rc.1", "2.4.0", "skip"),
    ("2.4.0", "2.3.0", "skip"),
    # numeric, not lexical: 1.100.0 is still below 2.5.0
    ("1.100.0", "2.4.0", "skip"),
    # dry-run tags are ignored, whatever the version
    ("1.19.1-pre.1", "2.4.0", "skip"),
    ("1.19.1-pre.2", "2.4.0", "skip"),
    ("2.5.0-pre.1", "2.4.0", "skip"),
    # ...but a prerelease that merely starts with the letters is not one
    ("2.5.0-preview.1", "2.4.0", "fail"),
]

BAD_INPUT = ["2.5", "v2.5.0", "2.5.0+r1", "02.5.0", "", "2.5.0-"]


def self_test():
    failed = 0
    for module, optimizer, want in CASES:
        got, msg = check(module, optimizer)
        ok = got == want
        failed += not ok
        print("  %-18s %-12s %-5s %s" % (module, optimizer.strip(), got,
                                         "ok" if ok else "FAIL (want %s)" % want))
        if not ok:
            print("      %s" % msg)
    for bad in BAD_INPUT:
        try:
            check(bad, "2.5.0")
        except VersionError:
            print("  %-18r refused        ok" % bad)
        else:
            print("  %-18r accepted       FAIL (want refused)" % bad)
            failed += 1
    total = len(CASES) + len(BAD_INPUT)
    if failed:
        print("FAIL: %d of %d self-test cases" % (failed, total))
        return 1
    print("OK: all %d self-test cases behave as specified." % total)
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    args = argv[1:]
    dry_run = "--dry-run" in args
    args = [a for a in args if a != "--dry-run"]
    opts = dict(zip(args[0::2], args[1::2]))
    if (len(args) != 4 or set(opts) != {"--module-version",
                                        "--optimizer-version-file"}):
        print(__doc__, file=sys.stderr)
        return 2
    with open(opts["--optimizer-version-file"]) as f:
        optimizer_version = f.read()
    try:
        verdict, msg = check(opts["--module-version"], optimizer_version)
    except VersionError as exc:
        print("::error::module version %s" % exc, file=sys.stderr)
        return 2
    if verdict == "fail":
        if dry_run:
            print("::warning::dry run: %s" % msg)
            return 0
        print("::error::%s" % msg, file=sys.stderr)
        return 1
    print("%s%s" % ("::notice::" if verdict == "skip" else "", msg))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
