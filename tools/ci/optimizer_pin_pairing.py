#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Decide whether a module release ships against an older optimizer release.

A module release normally ships with the optimizer release of the same
version: `install/debian/optimizer-pin.json` names `optimizer-v<version>`, and
the lane refuses any other pin. A module-only release (a fix in the module
alone) ships with the optimizer release that is already out instead, and its
packages depend on that version. The tagged tree opts in explicitly, in the
pin, for one module version:

    "unpaired_for": "2.8.2"

so that the choice is recorded with the release commit, and it is bound to
that release: a later cut that forgets to move the pin (or to re-set the
field) fails instead of shipping unpaired by accident. An absent field is the
normal, paired case.

For a final version X.Y.Z, `unpaired_for` must be exactly X.Y.Z, and the pin
must name a final optimizer release `optimizer-vA.B.C` older than X.Y.Z. A
field bound to another version, a pin that names this version's own pair, a
newer one or a pre-release is refused: the field is stale or wrong, and the
cut pull request fixes it. For a pre-release version (an rc, a rehearsal, a
-pre dry run) the field does not apply and the lane's pre-release rules
decide; a notice says so.

The rest of the checks do not change in a module-only release: the pinned
release's SHA256SUMS hash, the cache library commit, and the optimizer's
VERSION.txt, which must equal the pinned version
(check_module_optimizer_version.py --pinned-version). The release lane also
refuses it when an optimizer release of the module version exists.

Usage:

    tools/ci/optimizer_pin_pairing.py --pin FILE --version V
    tools/ci/optimizer_pin_pairing.py --self-test

Prints `pair_unpaired=true` or `pair_unpaired=false` (a $GITHUB_OUTPUT line).
Exit 0 on a decision, 1 on a refused pin, 2 on bad input.
"""

import json
import os
import re
import sys
import tempfile

SEMVER_RE = re.compile(
    r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
    r'(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$')
FINAL_RE = re.compile(r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')
FIELD = "unpaired_for"


class InputError(Exception):
    pass


class PinRefused(Exception):
    pass


def decide(pin, version):
    """Return (unpaired, notice or None); raise PinRefused or InputError."""
    m = SEMVER_RE.match(version)
    if not m:
        raise InputError("version '%s' is not X.Y.Z[-prerelease]" % version)
    core = tuple(int(m.group(i)) for i in (1, 2, 3))
    if FIELD not in pin:
        return False, None
    bound = pin[FIELD]
    if not isinstance(bound, str) or not FINAL_RE.match(bound):
        raise PinRefused(
            "optimizer-pin.json: %s is %r; it names the one final module "
            "version X.Y.Z that ships unpaired" % (FIELD, bound))
    tag = pin.get("tag", "")
    if m.group(4):
        return False, (
            "optimizer-pin.json says %s %s, which applies to that final "
            "version only; %s is a pre-release, so the pre-release rules of "
            "the lane decide the pair" % (FIELD, bound, version))
    if bound != version:
        raise PinRefused(
            "optimizer-pin.json says %s %s, but this release is %s. A "
            "module-only release binds the pin to one version: for a paired "
            "release pin optimizer-v%s and drop %s; for another module-only "
            "release set %s to %s" % (FIELD, bound, version, version, FIELD,
                                      FIELD, version))
    p = re.match(r'^optimizer-v(.*)$', tag)
    if not p or not FINAL_RE.match(p.group(1)):
        raise PinRefused(
            "optimizer-pin.json says %s %s but its tag %r is not a final "
            "optimizer release optimizer-vA.B.C" % (FIELD, bound, tag))
    pinned = tuple(int(x) for x in p.group(1).split("."))
    if pinned == core:
        raise PinRefused(
            "optimizer-pin.json says %s %s but %s is the pair of %s: drop %s"
            % (FIELD, bound, tag, version, FIELD))
    if pinned > core:
        raise PinRefused(
            "optimizer-pin.json says %s %s but %s is newer than the module "
            "release %s" % (FIELD, bound, tag, version))
    return True, None


def unp(tag, bound):
    return {"tag": tag, FIELD: bound}


CASES = [
    # pin, version, expected: True/False (unpaired) or "refused"
    ({"tag": "optimizer-v2.8.2"}, "2.8.2", False),
    ({"tag": "optimizer-v2.8.1"}, "2.8.2", False),  # the lane then refuses the pin
    (unp("optimizer-v2.8.1", "2.8.2"), "2.8.2", True),
    (unp("optimizer-v2.8.1", "2.9.0"), "2.9.0", True),
    (unp("optimizer-v2.9.0", "2.10.0"), "2.10.0", True),
    # a stale binding: the next cut forgot to move the pin or the field
    (unp("optimizer-v2.8.1", "2.8.2"), "2.8.3", "refused"),
    (unp("optimizer-v2.8.1", "2.8.2"), "2.9.0", "refused"),
    (unp("optimizer-v2.8.1", "2.8.2"), "3.0.0", "refused"),
    (unp("optimizer-v2.8.1", "2.8.2"), "2.8.1", "refused"),
    # a wrong pin or field: refused, never silently paired or unpaired
    (unp("optimizer-v2.8.2", "2.8.2"), "2.8.2", "refused"),
    (unp("optimizer-v2.9.0", "2.8.2"), "2.8.2", "refused"),
    (unp("optimizer-v2.8.2-rc.1", "2.8.3"), "2.8.3", "refused"),
    (unp("PLACEHOLDER", "2.8.2"), "2.8.2", "refused"),
    (unp("optimizer-v2.8.1", "2.8.2-rc.1"), "2.8.2", "refused"),
    (unp("optimizer-v2.8.1", "v2.8.2"), "2.8.2", "refused"),
    (unp("optimizer-v2.8.1", ""), "2.8.2", "refused"),
    (unp("optimizer-v2.8.1", True), "2.8.2", "refused"),
    (unp("optimizer-v2.8.1", None), "2.8.2", "refused"),
    # pre-releases: the field does not apply (rehearsals, rc, -pre dry runs)
    (unp("optimizer-v2.8.1", "2.8.2"), "2.8.1-rehearsal.7", False),
    (unp("optimizer-v2.8.1", "2.8.2"), "2.8.2-rc.1", False),
    (unp("optimizer-v2.8.1", "2.8.2"), "2.9.0-rc.1", False),
]

BAD_VERSIONS = ["2.8", "v2.8.2", "2.8.2+r1", ""]


def self_test():
    failed = 0
    for pin, version, want in CASES:
        try:
            got = decide(pin, version)[0]
        except PinRefused:
            got = "refused"
        ok = got == want
        failed += not ok
        print("  %-48s %-18s %-8s %s" % (
            json.dumps(pin, sort_keys=True), version, got,
            "ok" if ok else "FAIL (want %s)" % want))
    for bad in BAD_VERSIONS:
        try:
            decide({"tag": "optimizer-v2.8.1"}, bad)
        except InputError:
            print("  version %-16r refused  ok" % bad)
        else:
            print("  version %-16r accepted FAIL" % bad)
            failed += 1
    extra = 0
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "pin.json")
        runs = [
            ("exit 0, unpaired", unp("optimizer-v2.8.1", "2.8.2"), "2.8.2", 0, "pair_unpaired=true"),
            ("exit 0, paired", {"tag": "optimizer-v2.8.2"}, "2.8.2", 0, "pair_unpaired=false"),
            ("exit 1, stale binding", unp("optimizer-v2.8.1", "2.8.2"), "2.9.0", 1, ""),
            ("exit 2, bad version", {"tag": "optimizer-v2.8.1"}, "2.8", 2, ""),
            ("exit 2, pin not JSON", None, "2.8.2", 2, ""),
        ]
        for name, pin, version, want_rc, want_out in runs:
            with open(p, "w") as f:
                f.write("{" if pin is None else json.dumps(pin))
            out = os.path.join(t, "out")
            saved = sys.stdout, sys.stderr
            with open(out, "w") as fh:
                sys.stdout = sys.stderr = fh
                try:
                    rc = main(["x", "--pin", p, "--version", version])
                finally:
                    sys.stdout, sys.stderr = saved
            text = open(out).read()
            ok = rc == want_rc and (not want_out or want_out in text.splitlines())
            extra += 1
            failed += not ok
            print("  %-38s %s" % (name, "ok" if ok else "FAIL (exit %s): %s" % (rc, text)))
    total = len(CASES) + len(BAD_VERSIONS) + extra
    if failed:
        print("FAIL: %d of %d self-test cases" % (failed, total))
        return 1
    print("OK: all %d self-test cases behave as specified." % total)
    return 0


def main(argv):
    args = argv[1:]
    if args == ["--self-test"]:
        return self_test()
    opts = dict(zip(args[0::2], args[1::2]))
    if len(args) != 4 or set(opts) != {"--pin", "--version"}:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        with open(opts["--pin"]) as f:
            pin = json.load(f)
        if not isinstance(pin, dict):
            raise ValueError("not a JSON object")
    except (OSError, ValueError) as exc:
        print("::error::cannot read the optimizer pin %s: %s" % (opts["--pin"], exc), file=sys.stderr)
        return 2
    try:
        unpaired, notice = decide(pin, opts["--version"])
    except InputError as exc:
        print("::error::%s" % exc, file=sys.stderr)
        return 2
    except PinRefused as exc:
        print("::error::%s" % exc, file=sys.stderr)
        return 1
    if notice:
        print("::notice::%s" % notice, file=sys.stderr)
    print("pair_unpaired=%s" % ("true" if unpaired else "false"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
