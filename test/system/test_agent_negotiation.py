"""the design record P4: agent_optimize negotiation / cloaking-safety parity.

mod_pagespeed 1.1 has NO markdown render moat (no headless browser). Whatever the
agent_optimize configuration, an ``Accept: text/markdown`` request for an HTML
page MUST receive a normal HTML 200 — never a markdown body and never a 4xx
decline. That "never mis-serve, never cloak" invariant is the load-bearing
safety property, and because this file runs under EVERY port's system-test job
(Apache / nginx / Envoy / IIS), a pass on all of them IS the port-parity proof.

The positive negotiation assertion (an entitled ``Accept: text/markdown`` HTML
response advertises ``Vary: Accept``) requires a server configured with
``AgentOptimize on`` AND an ``agent_optimize``-entitled license. It is gated
behind PAGESPEED_AGENT_OPTIMIZE_ENABLED=1 and skips otherwise.

Ported intent: the design record §8.4 port-parity (negotiation/licensing/safety;
render-fidelity rows n/a on 1.1).
"""

import os

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_header_contains,
)

# A plain HTML page present in the example site on every port.
_HTML_PAGE = "extend_cache.html"


class TestAgentNegotiationSafety:
    """Invariants that hold on the DEFAULT server config (agent_optimize off).

    These need no special licensing or configuration, so they run — and must
    pass identically — on every port.
    """

    def test_markdown_accept_serves_html_not_markdown(
        self, client: PageSpeedClient, example_root: str
    ):
        # 1.1 never renders markdown: an Accept: text/markdown request for an
        # HTML page gets a normal HTML 200, never a markdown body.
        response = client.get(
            f"{example_root}/{_HTML_PAGE}",
            headers={"Accept": "text/markdown"},
        )
        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "markdown" not in content_type, (
            f"1.1 must never serve markdown; got Content-Type: {content_type!r}"
        )

    def test_markdown_accept_does_not_decline(
        self, client: PageSpeedClient, example_root: str
    ):
        # The agent request must never be turned into a 4xx (e.g. 406).
        response = client.get(
            f"{example_root}/{_HTML_PAGE}",
            headers={"Accept": "text/markdown"},
        )
        assert_http_status(response, 200)

    def test_wildcard_accept_serves_html(
        self, client: PageSpeedClient, example_root: str
    ):
        # A wildcard accept must behave exactly like a normal browser request
        # (it never opts into markdown).
        response = client.get(
            f"{example_root}/{_HTML_PAGE}",
            headers={"Accept": "*/*"},
        )
        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "markdown" not in content_type


@pytest.mark.skipif(
    os.environ.get("PAGESPEED_AGENT_OPTIMIZE_ENABLED") != "1",
    reason=(
        "needs a server with 'AgentOptimize on' AND an agent_optimize-entitled "
        "license; set PAGESPEED_AGENT_OPTIMIZE_ENABLED=1 on such a rig."
    ),
)
class TestAgentNegotiationEntitled:
    """The positive negotiation path on an entitled, agent_optimize-on server.

    Same URL, no body change: an entitled Accept: text/markdown request gets the
    normal optimized HTML plus ``Vary: Accept``; a browser request is untouched.
    """

    def test_entitled_markdown_request_adds_vary_accept(
        self, client: PageSpeedClient, example_root: str
    ):
        response = client.get(
            f"{example_root}/{_HTML_PAGE}",
            headers={"Accept": "text/markdown"},
        )
        assert_http_status(response, 200)
        assert_header_contains(response, "Vary", "Accept")
        # Still HTML — 1.1 never serves markdown.
        content_type = response.header("Content-Type").lower()
        assert "markdown" not in content_type

    def test_browser_request_unaffected(
        self, client: PageSpeedClient, example_root: str
    ):
        response = client.get(
            f"{example_root}/{_HTML_PAGE}",
            headers={"Accept": "text/html"},
        )
        assert_http_status(response, 200)
        content_type = response.header("Content-Type").lower()
        assert "markdown" not in content_type


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
