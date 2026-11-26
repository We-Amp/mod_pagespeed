# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Runs every `live` row of test/system/daemon_html/conformance_cases.json
against the IIS the rig provisioned, through the IIS provider
(pagespeed_test_framework.daemon_html_provider_iis), inside the IIS host.

First it proves the provider's settle() on the real server: a commit held
by the module (PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE) is not in the volume
yet, settle() does not return while it is held, and the page is there once
it returns. Then each row runs; a row is skipped only when its `ports`
leave IIS out.

Prints "RESULT <id> PASS|FAIL|SKIP <detail>" lines and
"SUMMARY <passed> <failed> <skipped>". Exit status 0 only with no failure.

Then the IIS-only rows of iis_rows.json (beside this file), which use the
rig's own knobs (daemon_html_provider_iis.IIS_VHOST_KNOBS).

Usage: python run_iis_html_conformance.py <test/system dir> [row id ...]
"""

import os
import sys
import threading
import time


def main(argv):
    sys.path.insert(0, argv[1])
    from pagespeed_test_framework import daemon_html_conformance as conf
    from pagespeed_test_framework import daemon_html_provider_iis as iis

    only = set(argv[2:])
    cases = conf.load_cases()
    bodies = cases["bodies"]
    provider = iis.make_provider()
    passed = failed = skipped = 0

    def result(rid, verdict, detail=""):
        print(f"RESULT {rid} {verdict} {detail}".rstrip(), flush=True)

    if not only or "provider-held-commit" in only:
        live = {
            "mode": "serve-variants",
            "agent_optimize": True,
            "origin": {"status": 200,
                       "headers": [["Content-Type", "text/html; charset=utf-8"],
                                   ["Cache-Control", "max-age=300"]],
                       "body_ref": "page"},
            "steps": [],
        }
        problems = []
        provider.setup("provider-held-commit", live, bodies)
        try:
            provider.settle()
            provider.hold_commits(True)
            try:
                r = provider.request("GET", "/held", None, [])
                if r.status != 200:
                    problems.append(f"status {r.status}")
                started, finished = provider.commits()
                if started <= finished:
                    problems.append(f"no commit held ({started} {finished})")
                if provider.stored_entry("/held", None) is not None:
                    problems.append("stored before the held commit ran")
                done = threading.Event()
                threading.Thread(target=lambda: (provider.settle(), done.set()),
                                 daemon=True).start()
                time.sleep(1.0)
                if done.is_set():
                    problems.append("settle returned while the commit was held")
            finally:
                provider.hold_commits(False)
            provider.settle()
            entry = provider.stored_entry("/held", None)
            if entry is None or entry.lifetime_s != 300:
                problems.append(f"after settle: {entry}")
        except Exception as e:  # reported, never hidden
            problems.append(repr(e))
        finally:
            provider.teardown()
        if problems:
            failed += 1
            result("provider-held-commit", "FAIL", "; ".join(problems))
        else:
            passed += 1
            result("provider-held-commit", "PASS")

    # The IIS-only rows, with the rig's own knobs.
    iis_rows = conf.load_cases(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                            "iis_rows.json"))["rows"]
    conf.VHOST_KNOBS = conf.VHOST_KNOBS | iis.IIS_VHOST_KNOBS
    for row in conf.live_rows(cases) + iis_rows:
        if only and row["id"] not in only:
            continue
        reason = conf.skip_reason(row, provider.port)
        if reason is not None:
            skipped += 1
            result(row["id"], "SKIP", reason)
            continue
        try:
            failures = conf.run_row(provider, row, bodies)
        except Exception as e:  # a broken row is a failure, never a skip
            failures = [f"{row['id']}: {e!r}"]
        if failures:
            failed += 1
            result(row["id"], "FAIL", " | ".join(failures)[:2000])
        else:
            passed += 1
            result(row["id"], "PASS")
    print(f"SUMMARY {passed} {failed} {skipped}", flush=True)
    return 0 if failed == 0 and passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
