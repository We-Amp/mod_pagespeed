# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Authorization compliance tests (RFC 9111 §3.5).

A shared cache MUST NOT use a stored response to satisfy a request with an
Authorization header, and MUST NOT store the response to such a request,
unless the response contains a cache directive that explicitly allows it.
§3.5 names exactly three permitting directives: public, s-maxage,
must-revalidate.  Bare max-age is NOT a permit.

End-to-end coverage for the module's §3.5 gate (AuthzCacheGateAllows /
AuthzCacheGateAllowsStale in src/nginx/authz_cache_gate.h, wired at every
serve/store site of src/nginx/ngx_pagespeed_module.cc, incl. the /llms.txt
alternate serve — issue #1017).

The origin routes live in compliance_origin.py under /authz/* — each sends
an ETag so the module records the entry, with Cache-Control varying which
permit (if any) the stored response carries.  The origin itself ignores the
Authorization header, so a pass-through always answers 200 from origin.

Not covered here (by design):
- The RSL-CAP License-scheme exemption: the harness provisions no RSL-CAP
  license tokens, so the exemption path cannot be exercised end-to-end.
  It is unit-covered at test/src/nginx/authz_cache_gate_test.cc.
"""

import re
import time

import pytest

# The origin ignores credentials; only header PRESENCE matters for §3.5.
AUTH = {"Authorization": "Basic dXNlcjpwYXNz"}  # user:pass


def _counter(body: str) -> int:
    """Extract the monotonic origin counter from /authz/stale-must-revalidate."""
    m = re.search(r"authz-counter:(\d+)", body)
    assert m, f"authz-counter marker missing from body: {body[:200]!r}"
    return int(m.group(1))


class TestAuthorizationPassThrough:
    """No permitting directive → authenticated requests bypass the cache."""

    def test_no_permit_entry_not_served_to_authenticated(self, client):
        """Entry cached without a §3.5 permit is not served to Authorization."""
        # Populate the cache anonymously (Cache-Control: max-age=3600 — no
        # public / s-maxage / must-revalidate).
        r = client.poll_for_hit("/authz/no-permit")
        client.assert_hit(r)

        # The same URL with Authorization must pass through to origin.
        r2 = client.get("/authz/no-permit", headers=AUTH)
        assert r2.status_code == 200
        assert r2.headers.get("X-PageSpeed") != "HIT", (
            "§3.5: stored response without a permitting directive must not "
            "satisfy a request with Authorization, got: "
            f"{r2.headers.get('X-PageSpeed', '<absent>')}"
        )


class TestAuthorizationPermits:
    """public / s-maxage / must-revalidate each permit a fresh cache hit."""

    def test_public_permits_authenticated_hit(self, client):
        """Cache-Control: public permits serving the entry to Authorization."""
        r = client.poll_for_hit("/authz/public")
        client.assert_hit(r)

        r2 = client.get("/authz/public", headers=AUTH)
        assert r2.status_code == 200
        client.assert_hit(r2)

    def test_s_maxage_permits_fresh_authenticated_hit(self, client):
        """Cache-Control: s-maxage (without public) permits a fresh hit."""
        r = client.poll_for_hit("/authz/s-maxage")
        client.assert_hit(r)

        r2 = client.get("/authz/s-maxage", headers=AUTH)
        assert r2.status_code == 200
        client.assert_hit(r2)

    def test_must_revalidate_permits_fresh_authenticated_hit(self, client):
        """Cache-Control: must-revalidate (without public) permits a fresh hit."""
        r = client.poll_for_hit("/authz/must-revalidate")
        client.assert_hit(r)

        r2 = client.get("/authz/must-revalidate", headers=AUTH)
        assert r2.status_code == 200
        client.assert_hit(r2)


class TestAuthorizationStaleTightening:
    """Once STALE, must-revalidate no longer permits an authenticated serve.

    RFC 9111 §4.2.4 / §5.2.2.10: must-revalidate and s-maxage require
    revalidation once stale — the very premise under which §3.5 permits
    them — so a stale entry additionally requires `public` for an
    Authorization-bearing request (AuthzCacheGateAllowsStale).
    """

    def test_stale_must_revalidate_not_served_to_authenticated(self, client):
        # Populate: max-age=5, must-revalidate, body carries a monotonic
        # origin counter (only origin fetches advance it).
        hit = client.poll_for_hit("/authz/stale-must-revalidate")
        client.assert_hit(hit)
        cached_counter = _counter(hit.text)

        # Let the entry go stale (max-age=5).
        time.sleep(6)

        r = client.get("/authz/stale-must-revalidate", headers=AUTH)
        assert r.status_code == 200
        assert r.headers.get("X-PageSpeed") != "HIT", (
            "stale + must-revalidate must not be served to Authorization, "
            f"got: {r.headers.get('X-PageSpeed', '<absent>')}"
        )
        # The answer came from origin (counter advanced), not the stale entry.
        assert _counter(r.text) > cached_counter, (
            "response repeated the stale cached counter "
            f"({cached_counter}) — stale entry was served"
        )


class TestAnonymousUnaffected:
    """Anonymous traffic is unchanged by an interleaved authenticated request."""

    def test_anonymous_behavior_unchanged_around_authenticated_request(self, client):
        # Anonymous: entry cached and served (no permit needed without
        # Authorization).
        before = client.poll_for_hit("/authz/anon-unchanged")
        client.assert_hit(before)

        # Authenticated request passes through (no permit) — and its
        # response must not be recorded over the existing entry (§3.5
        # store side).  The origin serves a per-fetch counter body, so a
        # store-side regression (the authenticated response overwriting
        # the entry) surfaces as a changed body on the next anonymous HIT.
        ra = client.get("/authz/anon-unchanged", headers=AUTH)
        assert ra.status_code == 200
        assert ra.headers.get("X-PageSpeed") != "HIT"
        assert ra.content != before.content, (
            "authenticated pass-through returned the cached body — "
            "expected a fresh origin fetch with an advanced counter"
        )

        # Anonymous again: still a HIT, and NOT the authenticated
        # exchange's body — that response must never be recorded over the
        # entry.  Deliberately not `after == before`: the worker may
        # benignly re-record from its own (unauthenticated) origin
        # re-fetch, which also advances the counter; only ra's exact body
        # appearing in the cache would prove the store-side gate regressed.
        after = client.get("/authz/anon-unchanged")
        client.assert_hit(after)
        assert after.content != ra.content, (
            "the authenticated request's response was recorded over the "
            "anonymously cached entry (§3.5 store side)"
        )


class TestLlmsTxtAuthorizationGate:
    """/llms.txt alternate serve takes the same §3.5 gate (issue #1017)."""

    @pytest.mark.skip(
        reason="harness cannot enable the /llms.txt serve path: "
        "agent_optimize_llms_txt_enabled requires two worker opt-in flags "
        "(worker.cc: browser_analysis.agent_optimize AND "
        "agent_optimize_llms_txt — operator flags, no token involved) and a "
        "worker-built sitemap-backed kLlmsTxt sentinel; the compliance stack "
        "provisions none of these (the entrypoint sets no opt-in flags). "
        "Enable once the harness grows llms.txt provisioning."
    )
    def test_llms_txt_not_served_to_authenticated(self, client):
        """Authorization request for /llms.txt must not be served the sentinel.

        Sentinels store no Cache-Control metadata, so there is no §3.5
        permit signal — the gate is consulted with flags 0 and fails
        closed: the request must pass through to origin (which serves its
        own /llms.txt or 404s).
        """
        r = client.get("/llms.txt", headers=AUTH)
        assert r.headers.get("X-PageSpeed") != "HIT", (
            "§3.5: /llms.txt sentinel must not be served to Authorization"
        )
