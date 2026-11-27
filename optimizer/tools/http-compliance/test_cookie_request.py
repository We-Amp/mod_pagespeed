# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cookie-bearing requests and the shared cache.

A page fetched for a request that carried a Cookie header is stored only
when the origin marked it explicitly shareable (`public`, or an `s-maxage`
above zero), and a stored page is served to a cookie-bearing request only
when the stored page carries the same mark.  "Page" is every response that
is not a stylesheet, script or image: XHTML, JSON and unknown types
included.  A stylesheet, script or image fetched with a cookie is stored
only when the origin stated its lifetime.  Responses fetched without a
cookie are stored and served as before.  The rule is the HTML cache
policy's (lib/cache/html_cache_policy.h, THE COOKIE RULE), applied in the
nginx port through src/nginx/cookie_cache_gate.h.

The origin routes live in compliance_origin.py under /cookie/<kind>/<name>:
the page names the visitor from the request's `visitor` cookie and carries a
per-fetch counter, so a page stored for one visitor and replayed to another
is visible in the body.

The Markdown copy is covered by t/111-cookie-request.t (the compliance stack
does not run agent optimization).
"""

import threading
import time
import uuid

import requests

ALICE = {"Cookie": "visitor=alice"}
BOB = {"Cookie": "visitor=bob"}
AUTH = {"Authorization": "Basic dXNlcjpwYXNz"}  # user:pass


def _fresh(kind: str) -> str:
    """A cache key no earlier test or run has touched."""
    return f"/cookie/{kind}/{uuid.uuid4().hex}"


def _requests_over(client, path, headers=None, seconds=3.0, interval=0.25):
    """Every response to repeated requests for `path` over `seconds`."""
    out = []
    deadline = time.time() + seconds
    while True:
        out.append(client.get(path, headers=headers))
        if time.time() >= deadline:
            return out
        time.sleep(interval)


class TestCookieRequestStore:
    """What is stored when the request carried a cookie."""

    def test_cookie_request_without_public_is_not_shared(self, client):
        path = _fresh("plain")
        first = client.get(path, headers=ALICE)
        assert first.status_code == 200
        assert "visitor:alice" in first.text

        for r in _requests_over(client, path):
            assert r.status_code == 200
            assert "visitor:alice" not in r.text, (
                "a page fetched for a cookie-bearing request without "
                "public or s-maxage was served to a visitor without that "
                f"cookie (X-PageSpeed: {r.headers.get('X-PageSpeed')})"
            )

    def test_cookie_request_with_public_is_shared(self, client):
        path = _fresh("public")
        first = client.get(path, headers=ALICE)
        assert "visitor:alice" in first.text

        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        assert "visitor:alice" in hit.text, (
            "a public page fetched with a cookie should be the stored copy"
        )

    def test_cookie_request_with_s_maxage_is_shared(self, client):
        path = _fresh("s-maxage")
        first = client.get(path, headers=ALICE)
        assert "visitor:alice" in first.text

        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        assert "visitor:alice" in hit.text

    def test_must_revalidate_does_not_make_a_cookie_page_shared(self, client):
        path = _fresh("must-revalidate")
        client.get(path, headers=ALICE)
        for r in _requests_over(client, path):
            assert "visitor:alice" not in r.text


class TestCookieRequestStoreByType:
    """The page rule covers every type that is not a static asset."""

    def _not_shared(self, client, kind):
        path = _fresh(kind)
        first = client.get(path, headers=ALICE)
        assert first.status_code == 200
        assert "visitor:alice" in first.text

        responses = _requests_over(client, path, seconds=4.0)
        for r in responses:
            assert "visitor:alice" not in r.text, (
                f"a {kind} response fetched with a cookie was served to a "
                f"visitor without it (X-PageSpeed: {r.headers.get('X-PageSpeed')})"
            )
        return responses

    # The nginx port records only the types it optimizes (detected from a
    # lower-case Content-Type), so these three are not stored today with or
    # without a cookie; the tests pin that a cookie-bearing response of
    # these types is never shared if that changes.
    def test_xhtml_cookie_request_is_not_shared(self, client):
        self._not_shared(client, "xhtml")

    def test_upper_case_html_cookie_request_is_not_shared(self, client):
        self._not_shared(client, "upper")

    def test_json_cookie_request_is_not_shared(self, client):
        self._not_shared(client, "json")

    def test_script_without_cache_control_from_a_cookie_is_not_shared(
        self, client
    ):
        responses = self._not_shared(client, "js-no-cc")
        # Scripts are cached: without a cookie the same URL becomes a hit.
        assert any(r.headers.get("X-PageSpeed") == "HIT" for r in responses)

    def test_script_with_max_age_from_a_cookie_is_shared(self, client):
        path = _fresh("js-max-age")
        first = client.get(path, headers=ALICE)
        assert "visitor:alice" in first.text
        hit = client.poll_for_hit(path)
        client.assert_hit(hit)
        assert "visitor:alice" in hit.text


class TestSMaxageMustBeAValidPositiveValue:
    """s-maxage=0, a malformed s-maxage and a repeated s-maxage are not a
    sharing mark.

    These routes keep one ETag per path, so a stored copy is revalidated
    with a 304 and its body served again: a copy stored for one visitor
    would reach another.
    """

    def _not_shared(self, client, kind):
        path = _fresh(kind)
        first = client.get(path, headers=ALICE)
        assert "visitor:alice" in first.text
        for r in _requests_over(client, path, seconds=3.0):
            assert "visitor:alice" not in r.text, (
                f"{kind}: a page fetched with a cookie was served to a visitor "
                f"without it (X-PageSpeed: {r.headers.get('X-PageSpeed')})"
            )

    def test_s_maxage_zero_is_not_an_opt_in(self, client):
        self._not_shared(client, "s-maxage-zero")

    def test_malformed_s_maxage_is_not_an_opt_in(self, client):
        self._not_shared(client, "s-maxage-bad")

    def _not_served(self, client, kind):
        path = _fresh(kind)
        stored = client.get(path)
        assert "visitor:nobody" in stored.text
        client.get(path)  # recorded by now; revalidated from here on
        r = client.get(path, headers=BOB)
        assert "visitor:bob" in r.text, (
            f"{kind}: a stored page was served to a cookie-bearing request "
            f"(X-PageSpeed: {r.headers.get('X-PageSpeed')})"
        )

    def test_repeated_s_maxage_is_not_an_opt_in(self, client):
        self._not_shared(client, "s-maxage-dup")

    def test_repeated_s_maxage_page_is_not_served_to_a_cookie(self, client):
        self._not_served(client, "s-maxage-dup")

    def test_s_maxage_zero_page_is_not_served_to_a_cookie(self, client):
        self._not_served(client, "s-maxage-zero")

    def test_malformed_s_maxage_page_is_not_served_to_a_cookie(self, client):
        self._not_served(client, "s-maxage-bad")


class TestCookieRequestServe:
    """What a cookie-bearing request is served from the cache."""

    def test_stored_page_without_public_is_not_served_to_a_cookie(self, client):
        path = _fresh("plain")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        assert "visitor:nobody" in stored.text

        r = client.get(path, headers=BOB)
        assert r.status_code == 200
        assert r.headers.get("X-PageSpeed") != "HIT", (
            "a page stored without public or s-maxage was served to a "
            "cookie-bearing request"
        )
        assert "visitor:bob" in r.text

        # The cookie-bearing response did not replace the stored page.
        after = client.get(path)
        client.assert_hit(after)
        assert "visitor:bob" not in after.text

    def test_stored_public_page_is_served_to_a_cookie(self, client):
        path = _fresh("public")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)

        r = client.get(path, headers=BOB)
        client.assert_hit(r)
        assert r.content == stored.content

    def test_stored_s_maxage_page_is_served_to_a_cookie(self, client):
        path = _fresh("s-maxage")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)

        r = client.get(path, headers=BOB)
        client.assert_hit(r)
        assert r.content == stored.content


class TestCookieRequestStaleServes:
    """Revalidated, stale-if-error and coalesced stale serves of a stored
    page take the same rule as a fresh hit."""

    def test_revalidated_page_is_not_served_to_a_cookie(self, client):
        path = _fresh("reval")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        assert "visitor:nobody" in stored.text
        time.sleep(2.5)  # past max-age=1; ages are whole seconds

        r = client.get(path, headers=BOB)
        assert r.status_code == 200
        assert "visitor:bob" in r.text, (
            "the stored page was revalidated and served to a cookie-bearing "
            f"request (X-PageSpeed: {r.headers.get('X-PageSpeed')})"
        )

        # Control: without a cookie the same entry is revalidated as before.
        anon = client.get(path)
        assert anon.headers.get("X-PageSpeed") in ("REVALIDATED", "HIT"), (
            anon.headers
        )
        assert "visitor:nobody" in anon.text

    def test_stale_if_error_page_is_not_served_to_a_cookie(
        self, client, origin_client
    ):
        path = _fresh("flaky")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        try:
            origin_client.get(f"/cookie-control?fail={path}")
            time.sleep(2.5)
            r = client.get(path, headers=BOB)
            assert "visitor:nobody" not in r.text, (
                "the stored page was served stale to a cookie-bearing request "
                f"(X-PageSpeed: {r.headers.get('X-PageSpeed')})"
            )
            assert r.status_code == 500

            # Control: without a cookie the stale copy stands in.
            anon = client.get(path)
            assert anon.headers.get("X-PageSpeed") == "STALE", anon.headers
            assert anon.content == stored.content
        finally:
            origin_client.get(f"/cookie-control?unfail={path}")

    def test_coalesced_stale_page_is_not_served_to_a_cookie(
        self, client, origin_client
    ):
        path = _fresh("slow")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        try:
            origin_client.get(f"/cookie-control?slow={path}")
            time.sleep(3.5)  # past max-age=2
            refresh = {}

            def refetch():
                refresh["r"] = requests.get(client.base_url + path, timeout=30)

            t = threading.Thread(target=refetch)
            t.start()
            time.sleep(0.5)
            r = client.get(path, headers=BOB, timeout=30)
            t.join(timeout=30)
            assert "visitor:bob" in r.text, (
                "the stale page was served to a cookie-bearing request while "
                f"another request refreshed it (X-PageSpeed: "
                f"{r.headers.get('X-PageSpeed')})"
            )
            assert "r" in refresh and refresh["r"].status_code == 200
        finally:
            origin_client.get(f"/cookie-control?unslow={path}")

    def test_coalesced_stale_page_is_served_without_a_cookie(
        self, client, origin_client
    ):
        """Control for the test above: the coalesced stale serve happens."""
        path = _fresh("slow")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        try:
            origin_client.get(f"/cookie-control?slow={path}")
            time.sleep(3.5)
            refresh = {}

            def refetch():
                refresh["r"] = requests.get(client.base_url + path, timeout=30)

            t = threading.Thread(target=refetch)
            t.start()
            time.sleep(0.5)
            r = client.get(path, timeout=30)
            t.join(timeout=30)
            assert r.headers.get("X-PageSpeed") == "HIT", r.headers
            assert r.content == stored.content
        finally:
            origin_client.get(f"/cookie-control?unslow={path}")


class TestCookieAndAuthorization:
    """The Authorization rule is unchanged; a request with both passes both."""

    def test_authorization_without_cookie_keeps_its_permits(self, client):
        # must-revalidate is an Authorization permit (RFC 9111 §3.5).
        path = _fresh("must-revalidate")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        r = client.get(path, headers=AUTH)
        client.assert_hit(r)

    def test_authorization_and_cookie_need_a_sharing_mark(self, client):
        path = _fresh("must-revalidate")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        r = client.get(path, headers={**AUTH, **BOB})
        assert r.headers.get("X-PageSpeed") != "HIT"
        assert "visitor:bob" in r.text

    def test_authorization_and_cookie_on_a_public_page(self, client):
        path = _fresh("public")
        stored = client.poll_for_hit(path)
        client.assert_hit(stored)
        r = client.get(path, headers={**AUTH, **BOB})
        client.assert_hit(r)
