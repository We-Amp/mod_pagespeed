#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""The nginx HTML cache conformance provider's settle() against the real server.

The commit that stores a page runs after the client has the response, so a
read of the volume right after a request can see a page that will be stored
as absent. settle() waits for every posted commit to finish. This test holds
the commit (PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE: while that file exists the
module's commit thread waits), shows that a read made then finds nothing
although the commit is posted, then lets it go and shows that settle()
returns only once the page is stored.
"""

import pytest

from pagespeed_test_framework import daemon_html_conformance as conf
from pagespeed_test_framework import daemon_html_provider_nginx as nginx_provider

pytestmark = pytest.mark.requires_fixture("daemon_html_conformance")

LIVE = {
    "mode": "serve-variants",
    "agent_optimize": True,
    "origin": {
        "status": 200,
        "headers": [
            ["Content-Type", "text/html; charset=utf-8"],
            ["Cache-Control", "max-age=300"],
        ],
        "body_ref": "page",
    },
    "steps": [],
}


@pytest.fixture(scope="module")
def provider(server_config):
    return nginx_provider.make_provider(server_config)


def test_settle_waits_for_a_held_commit(provider):
    bodies = conf.load_cases()["bodies"]
    provider.setup("provider-held-commit", LIVE, bodies)
    try:
        provider.settle()
        provider.hold_commits(True)
        try:
            r = provider.request("GET", "/held", None, [])
            assert r.status == 200
            # A commit is posted and has not finished: settle() would wait.
            posted, finished = provider.commits()["a"]
            assert posted > finished, (posted, finished)
            # The premature read: the page is not there yet.
            assert provider.stored_entry("/held", None) is None
        finally:
            provider.hold_commits(False)
        provider.settle()
        posted, finished = provider.commits()["a"]
        assert posted == finished
        entry = provider.stored_entry("/held", None)
        assert entry is not None
        assert entry.lifetime_s == 300
    finally:
        provider.teardown()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
