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
VERSION.txt carried the product line (2.N == 1.(N+15): the 1.19.0 packages
paired with VERSION.txt 2.4.0). Nothing more ships on that numbering, but the
tags this lane already carries on it (module-v1.19.1-pre.1 and -pre.2, the
dry runs of this lane; the 1.15.0 to 1.19.0 packages predate it and have no
module-v tag) must still verify, so the equality is enforced from 2.5.0 on
only; below it the check passes with a notice.

--dry-run means the pair is not final yet: the pin does not name the
optimizer tag this release needs (the lane passes the flag only then). In
that state a mismatch is reported as a warning (a `::warning::` annotation
and a line in $GITHUB_STEP_SUMMARY) instead of failing, and a `-pre` tag is
not checked at all. Once the pin names the paired tag, the flag is not passed
and a mismatch fails, a dry run included.

A pushed `-pre` tag runs the lane as a real run, like any other tag (the lane
releases every prerelease as a GitHub pre-release and special-cases none), so
without --dry-run a `-pre` tag is checked like a release candidate: from 2.5.0
on its X.Y.Z core must equal VERSION.txt. VERSION.txt may equal either the
full version or its X.Y.Z core (an rc is cut from a commit whose VERSION.txt
already names the release it leads to).

Usage:

    tools/ci/check_module_optimizer_version.py --module-version V \\
        --optimizer-version-file FILE [--dry-run]
    tools/ci/check_module_optimizer_version.py --enforced --module-version V
    tools/ci/check_module_optimizer_version.py --self-test

FILE is read only when the check applies (2.5.0 and later), so a lane may
skip fetching it below 2.5.0. --enforced exits 0 when the check applies to V
and 1 when it does not (the lane fetches VERSION.txt only then).

Exit 0 on pass, a skip (notice) or a --dry-run warning; 1 on a mismatch; 2 on
bad input, including a missing or empty VERSION file where one is needed.
"""

import os
import re
import sys

ENFORCED_SINCE = (2, 5, 0)
SINCE = ".".join(map(str, ENFORCED_SINCE))
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
    """`pre`, `pre.1`, `pre1`: the lane's dry-run tags."""
    return re.match(r'^pre(?:[.0-9]|$)', prerelease) is not None


def enforced(module_version):
    return parse(module_version)[0] >= ENFORCED_SINCE


def check(module_version, read_optimizer_version, dry_run=False):
    """Return (verdict, message); verdict is 'pass', 'skip', 'warn' or 'fail'.

    read_optimizer_version is a callable, called only when the check applies.
    """
    core, pre = parse(module_version)
    if dry_run and is_dry_run_tag(pre):
        return "skip", (
            "module %s is a dry-run tag (-%s) and the pair is not final; the "
            "version-equality guard ignores it" % (module_version, pre))
    if core < ENFORCED_SINCE:
        return "skip", (
            "module %s is below %s: the legacy 1.x numbering paired it with an "
            "optimizer VERSION.txt of a different number; the equality is "
            "enforced from %s on" % (module_version, SINCE, SINCE))
    want_core = "%d.%d.%d" % core
    have = read_optimizer_version().strip()
    if not have:
        raise VersionError("the pinned optimizer's VERSION.txt is empty")
    if have in (module_version, want_core):
        return "pass", (
            "module %s and the pinned optimizer's VERSION.txt %s agree"
            % (module_version, have))
    msg = ("module %s is paired with an optimizer whose VERSION.txt says '%s'. "
           "From %s on the module and the optimizer release one number: cut "
           "optimizer-v%s from an optimizer commit whose VERSION.txt is %s, "
           "record it in install/debian/optimizer-pin.json (tag, "
           "optimizer_commit, sha256sums_sha256), then tag the module."
           % (module_version, have, SINCE, want_core, want_core))
    if dry_run:
        return "warn", "pair not final (dry run): " + msg
    return "fail", msg


CASES = [
    # module, optimizer VERSION.txt, dry_run, verdict
    ("2.5.0", "2.5.0", False, "pass"),
    ("2.5.0", "2.5.0\n", False, "pass"),
    ("2.5.0", "  2.5.0  \n", False, "pass"),
    ("2.5.0", "2.4.0", False, "fail"),
    ("2.5.0", "2.4.0", True, "warn"),
    ("2.5.1", "2.5.0", False, "fail"),
    ("2.5.0-rc.1", "2.5.0", False, "pass"),
    ("2.5.0-rc.1", "2.5.0-rc.1", False, "pass"),
    ("2.5.0-rc.1", "2.4.0", False, "fail"),
    ("2.10.0", "2.10.0", False, "pass"),
    ("2.10.0", "2.1.0", False, "fail"),
    ("3.0.0", "2.9.0", False, "fail"),
    # legacy mapping: module 1.x paired with the 2.N product line
    ("1.19.0", "2.4.0", False, "skip"),
    ("1.19.1", "2.4.0", False, "skip"),
    ("1.19.1-rc.1", "2.4.0", False, "skip"),
    ("2.4.0", "2.3.0", False, "skip"),
    # numeric, not lexical: 1.100.0 is still below 2.5.0
    ("1.100.0", "2.4.0", False, "skip"),
    # -pre: ignored only while the pair is not final (--dry-run) ...
    ("2.5.0-pre.1", "2.4.0", True, "skip"),
    # ... a pushed -pre tag is a real run, checked on its X.Y.Z core
    ("2.5.0-pre.1", "2.4.0", False, "fail"),
    ("2.5.0-pre.1", "2.5.0", False, "pass"),
    # the published 1.19.1-pre tags keep verifying (legacy), either way
    ("1.19.1-pre.1", "2.4.0", False, "skip"),
    ("1.19.1-pre.2", "2.4.0", True, "skip"),
    # a prerelease that merely starts with the letters is not a dry-run tag
    ("2.5.0-preview.1", "2.4.0", True, "warn"),
]

BAD_INPUT = ["2.5", "v2.5.0", "2.5.0+r1", "02.5.0", "", "2.5.0-"]


def _missing():
    raise VersionError("no VERSION file")


def self_test():
    failed = 0
    for module, optimizer, dry, want in CASES:
        got, msg = check(module, lambda: optimizer, dry)
        ok = got == want
        failed += not ok
        print("  %-16s %-8s %-7s %-5s %s" % (
            module, optimizer.strip(), "dry" if dry else "real", got,
            "ok" if ok else "FAIL (want %s)" % want))
        if not ok:
            print("      %s" % msg)
    for bad in BAD_INPUT:
        try:
            check(bad, lambda: "2.5.0")
        except VersionError:
            print("  %-16r refused                ok" % bad)
        else:
            print("  %-16r accepted               FAIL (want refused)" % bad)
            failed += 1
    # An empty or missing VERSION file is refused where it is needed, and
    # never read below 2.5.0.
    extra = 0
    for name, fn in [("empty VERSION.txt refused", lambda: check("2.5.0", lambda: " \n")),
                     ("missing VERSION.txt refused", lambda: check("2.5.0", _missing))]:
        extra += 1
        try:
            fn()
        except VersionError:
            print("  %-38s ok" % name)
        else:
            print("  %-38s FAIL" % name)
            failed += 1
    extra += 1
    name = "below 2.5.0 the file is never read"
    try:
        got = check("1.19.1", _missing)[0]
    except VersionError:
        got = "read (refused)"
    if got == "skip":
        print("  %-38s ok" % name)
    else:
        print("  %-38s FAIL (got %s)" % (name, got))
        failed += 1
    # main(): exit codes end to end, including a missing file (2, not a
    # traceback) and the --enforced probe.
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        v = os.path.join(t, "VERSION.txt")
        with open(v, "w") as f:
            f.write("2.4.0\n")
        missing = os.path.join(t, "missing")
        summary = os.path.join(t, "summary.md")
        old = os.environ.get("GITHUB_STEP_SUMMARY")
        os.environ["GITHUB_STEP_SUMMARY"] = summary
        devnull = open(os.devnull, "w")
        saved = sys.stdout, sys.stderr
        sys.stdout = sys.stderr = devnull
        try:
            runs = [
                ("exit 1 on a real mismatch", ["--module-version", "2.5.0", "--optimizer-version-file", v], 1),
                ("exit 0 on a dry-run warning", ["--module-version", "2.5.0", "--optimizer-version-file", v, "--dry-run"], 0),
                ("exit 2 on a missing file", ["--module-version", "2.5.0", "--optimizer-version-file", missing], 2),
                ("exit 0, missing file below 2.5.0", ["--module-version", "1.19.1", "--optimizer-version-file", missing], 0),
                ("exit 2 on a bad version", ["--module-version", "2.5", "--optimizer-version-file", v], 2),
                ("--enforced 2.5.0 -> 0", ["--enforced", "--module-version", "2.5.0"], 0),
                ("--enforced 1.19.1 -> 1", ["--enforced", "--module-version", "1.19.1"], 1),
                ("--enforced 2.5.0-pre.1 -> 0", ["--enforced", "--module-version", "2.5.0-pre.1"], 0),
            ]
            results = [(n, main(["x"] + a), want) for n, a, want in runs]
        finally:
            sys.stdout, sys.stderr = saved
            devnull.close()
            if old is None:
                del os.environ["GITHUB_STEP_SUMMARY"]
            else:
                os.environ["GITHUB_STEP_SUMMARY"] = old
        for n, got, want in results:
            extra += 1
            ok = got == want
            failed += not ok
            print("  %-38s %s" % (n, "ok" if ok else "FAIL (exit %s, want %s)" % (got, want)))
        extra += 1
        text = open(summary).read() if os.path.exists(summary) else ""
        if "pair not final" in text and "VERSION.txt says '2.4.0'" in text:
            print("  %-38s ok" % "dry-run warning in the step summary")
        else:
            print("  %-38s FAIL" % "dry-run warning in the step summary")
            failed += 1
    total = len(CASES) + len(BAD_INPUT) + extra
    if failed:
        print("FAIL: %d of %d self-test cases" % (failed, total))
        return 1
    print("OK: all %d self-test cases behave as specified." % total)
    return 0


def summarize(text):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a") as f:
            f.write("### Module / optimizer version guard\n\n:warning: %s\n\n" % text)


def main(argv):
    args = argv[1:]
    if args == ["--self-test"]:
        return self_test()
    if len(args) == 3 and args[0] == "--enforced" and args[1] == "--module-version":
        try:
            return 0 if enforced(args[2]) else 1
        except VersionError as exc:
            print("::error::module version %s" % exc, file=sys.stderr)
            return 2
    dry_run = "--dry-run" in args
    args = [a for a in args if a != "--dry-run"]
    opts = dict(zip(args[0::2], args[1::2]))
    if (len(args) != 4 or set(opts) != {"--module-version",
                                        "--optimizer-version-file"}):
        print(__doc__, file=sys.stderr)
        return 2
    path = opts["--optimizer-version-file"]

    def read():
        try:
            with open(path) as f:
                return f.read()
        except OSError as exc:
            raise VersionError("cannot read the pinned optimizer's VERSION.txt "
                               "(%s): %s" % (path, exc.strerror))

    try:
        verdict, msg = check(opts["--module-version"], read, dry_run)
    except VersionError as exc:
        print("::error::%s" % exc, file=sys.stderr)
        return 2
    if verdict == "fail":
        print("::error::%s" % msg, file=sys.stderr)
        return 1
    if verdict == "warn":
        print("::warning::%s" % msg)
        summarize(msg)
        return 0
    print("%s%s" % ("::notice::" if verdict == "skip" else "", msg))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
