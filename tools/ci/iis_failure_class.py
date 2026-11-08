#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Classify a failed IIS system-test run: the host, or the change under test.

The IIS job (pipeline-pr.yml, `IIS System Tests`) runs this after its test
step failed. The setup script (test/system/setup_iis_full.ps1) writes
`infra-class.txt` into the evidence directory when the test site cannot bind
its ports: HTTP.sys refuses the binding (appcmd hresult 0x80070020), or the
HTTP.sys IPv6 transport is wedged and only a reboot clears it. Those are
host failures; the same commit passes on a healthy runner.

Output, one line on stdout:

    infra <reason>   the marker names a known reason
    test             anything else: no marker, an empty one, an unknown
                     reason (fail closed: a test failure is never retried
                     away by a marker this script does not know)

The workflow retries an `infra` failure once on another runner; the retry
runs every test again, and its own failure, of either class, fails the job.
So a marker can cost one retry but cannot turn a failing test green.

Usage:

    tools/ci/iis_failure_class.py <evidence dir>
    tools/ci/iis_failure_class.py --self-test

Exit 0 with the line above; 2 on a usage error.
"""
import os
import sys
import tempfile

MARKER = "infra-class.txt"
# reason token -> what it means (docs/ci/pipelines.md, "IIS infrastructure
# failures")
KNOWN_REASONS = {
    "iis-site-start": "the test site did not start: HTTP.sys refused a binding",
    "httpsys-ipv6-wedge": "HTTP.sys cannot bind its IPv6 transport; the runner needs a reboot",
}


def classify(evidence_dir):
    path = os.path.join(evidence_dir, MARKER)
    try:
        with open(path, "r", encoding="ascii", errors="replace") as f:
            first = f.readline().strip()
    except OSError:
        return "test"
    if first in KNOWN_REASONS:
        return "infra " + first
    return "test"


def self_test():
    cases = [
        ("no marker", None, "test"),
        ("site start", "iis-site-start\nsite PageSpeedTestSite did not reach Started\n", "infra iis-site-start"),
        ("ipv6 wedge", "httpsys-ipv6-wedge\n", "infra httpsys-ipv6-wedge"),
        ("windows line ends", "iis-site-start\r\ndetail\r\n", "infra iis-site-start"),
        ("empty marker", "", "test"),
        ("unknown reason", "disk-full\n", "test"),
        ("reason not on the first line", "\niis-site-start\n", "test"),
        ("prefix of a reason", "iis-site\n", "test"),
    ]
    failed = 0
    for name, content, want in cases:
        with tempfile.TemporaryDirectory() as d:
            if content is not None:
                with open(os.path.join(d, MARKER), "w", encoding="ascii", newline="") as f:
                    f.write(content)
            got = classify(d)
        if got == want:
            print("PASS: %s" % name)
        else:
            print("FAIL: %s: got %r, want %r" % (name, got, want))
            failed += 1
    with tempfile.TemporaryDirectory() as d:
        got = classify(os.path.join(d, "missing"))
    if got == "test":
        print("PASS: missing evidence directory")
    else:
        print("FAIL: missing evidence directory: got %r" % got)
        failed += 1
    total = len(cases) + 1
    if failed:
        print("FAIL: %d of %d self-test cases" % (failed, total))
        return 1
    print("OK: all %d self-test cases behave as specified." % total)
    return 0


def main(args):
    if args == ["--self-test"]:
        return self_test()
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    print(classify(args[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
