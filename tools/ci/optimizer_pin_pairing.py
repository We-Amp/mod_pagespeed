#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Decide whether a module release ships against an older optimizer release.

A module release normally ships with the optimizer release of the same
version: `install/debian/optimizer-pin.json` names `optimizer-v<version>`, and
the lane refuses any other pin. A module-only release (a fix in the module
alone) ships with the optimizer release that is already out instead, and its
packages depend on that version. The tagged tree opts in explicitly, in the
pin:

    "pairing": "unpaired"

so that the choice is recorded with the release commit and a stale pin cannot
ship unpaired by accident. An absent field, or "paired", is the normal case.

For a final version (X.Y.Z) and "unpaired", the pin must name a final
optimizer release `optimizer-vA.B.C` older than X.Y.Z. A pin that names this
version's own pair, a newer one or a pre-release is refused: the field is
stale or wrong, and the cut pull request fixes it. For a pre-release version
(an rc, a rehearsal, a -pre dry run) the field does not apply and the lane's
pre-release rules decide; a notice says so.

The rest of the checks do not change in a module-only release: the pinned
release's SHA256SUMS hash, the cache library commit, and the optimizer's
VERSION.txt, which must equal the pinned version
(check_module_optimizer_version.py --pinned-version).

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
FINAL_PIN_RE = re.compile(
    r'^optimizer-v((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))$')
VALUES = ("paired", "unpaired")


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
    pairing = pin.get("pairing", "paired")
    if pairing not in VALUES:
        raise PinRefused(
            "optimizer-pin.json: pairing is %r; it is \"paired\" (the default) "
            "or \"unpaired\" (a module-only release)" % (pairing,))
    if pairing == "paired":
        return False, None
    tag = pin.get("tag", "")
    if m.group(4):
        return False, (
            "optimizer-pin.json says pairing \"unpaired\", which applies to a "
            "final version only; %s is a pre-release, so the pre-release rules "
            "of the lane decide the pair" % version)
    p = FINAL_PIN_RE.match(tag)
    if not p:
        raise PinRefused(
            "optimizer-pin.json says pairing \"unpaired\" but its tag %r is not "
            "a final optimizer release optimizer-vA.B.C" % tag)
    pinned = tuple(int(p.group(i)) for i in (2, 3, 4))
    if pinned == core:
        raise PinRefused(
            "optimizer-pin.json says pairing \"unpaired\" but %s is the pair "
            "of %s: drop the pairing field" % (tag, version))
    if pinned > core:
        raise PinRefused(
            "optimizer-pin.json says pairing \"unpaired\" but %s is newer than "
            "the module release %s" % (tag, version))
    return True, None


CASES = [
    # pin, version, expected: True/False (unpaired) or "refused"
    ({"tag": "optimizer-v2.8.2"}, "2.8.2", False),
    ({"tag": "optimizer-v2.8.1"}, "2.8.2", False),  # the lane then refuses the pin
    ({"tag": "optimizer-v2.8.2", "pairing": "paired"}, "2.8.2", False),
    ({"tag": "optimizer-v2.8.1", "pairing": "unpaired"}, "2.8.2", True),
    ({"tag": "optimizer-v2.8.1", "pairing": "unpaired"}, "2.9.0", True),
    ({"tag": "optimizer-v2.9.0", "pairing": "unpaired"}, "2.10.0", True),
    # stale or wrong field: refused, never silently paired or unpaired
    ({"tag": "optimizer-v2.8.2", "pairing": "unpaired"}, "2.8.2", "refused"),
    ({"tag": "optimizer-v2.9.0", "pairing": "unpaired"}, "2.8.2", "refused"),
    ({"tag": "optimizer-v2.8.2-rc.1", "pairing": "unpaired"}, "2.8.3", "refused"),
    ({"tag": "PLACEHOLDER", "pairing": "unpaired"}, "2.8.2", "refused"),
    ({"tag": "optimizer-v2.8.1", "pairing": "Unpaired"}, "2.8.2", "refused"),
    ({"tag": "optimizer-v2.8.1", "pairing": True}, "2.8.2", "refused"),
    ({"tag": "optimizer-v2.8.1", "pairing": ""}, "2.8.2", "refused"),
    # pre-releases: the field does not apply (rehearsals, rc, -pre dry runs)
    ({"tag": "optimizer-v2.8.1", "pairing": "unpaired"}, "2.8.1-rehearsal.7", False),
    ({"tag": "optimizer-v2.8.1", "pairing": "unpaired"}, "2.8.2-rc.1", False),
    ({"tag": "optimizer-v2.8.1", "pairing": "unpaired"}, "2.8.2-pre.1", False),
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
            ("exit 0, unpaired", {"tag": "optimizer-v2.8.1", "pairing": "unpaired"}, "2.8.2", 0, "pair_unpaired=true"),
            ("exit 0, paired", {"tag": "optimizer-v2.8.2"}, "2.8.2", 0, "pair_unpaired=false"),
            ("exit 1, stale field", {"tag": "optimizer-v2.8.2", "pairing": "unpaired"}, "2.8.2", 1, ""),
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
