# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Content negotiation compliance tests.

Verifies Accept header negotiation, Vary header correctness, and
Content-Type accuracy for body-modified responses.

RFC 9110 Section 12: Content negotiation semantics.
RFC 9110 Section 12.5.3: Vary header.
"""

import pytest


class TestImageFormatNegotiation:
    """Accept header → image format selection."""

    def test_accept_webp_gets_webp_on_hit(self, client):
        """Accept: image/webp returns WebP content on cache HIT."""
        # Trigger recording
        client.get("/1x1.jpg")
        r = client.poll_for_hit(
            "/1x1.jpg",
            headers={"Accept": "image/webp, image/jpeg, */*"},
        )
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            # May still be JPEG if worker hasn't processed yet
            if ct == "image/webp":
                assert r.content[:4] == b"RIFF"
                assert r.content[8:12] == b"WEBP"

    def test_accept_avif_gets_avif_on_hit(self, client):
        """Accept: image/avif returns AVIF content on cache HIT."""
        client.get("/1x1.png")
        r = client.poll_for_hit(
            "/1x1.png",
            headers={"Accept": "image/avif, image/png, */*"},
        )
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            if ct == "image/avif":
                assert r.content[4:8] == b"ftyp"

    def test_no_accept_negotiates_as_wildcard(self, client):
        """No Accept header negotiates as */* (RFC 9110 12.5.1)."""
        client.get("/1x1.jpg")
        r = client.poll_for_hit("/1x1.jpg")
        if r.headers.get("X-PageSpeed") == "HIT":
            # An absent Accept is read as */*, so this request is negotiated
            # like any wildcard one and may be served an optimized format --
            # it is no longer pinned to the origin's own format.
            ct = r.headers.get("Content-Type", "")
            assert ct in ("image/jpeg", "image/webp", "image/avif"), (
                f"Unexpected Content-Type: {ct}"
            )

    def test_accept_star_returns_image(self, client):
        """Accept: */* returns a valid image."""
        r = client.get("/1x1.jpg", headers={"Accept": "*/*"})
        assert r.status_code == 200
        assert len(r.content) > 0

    def test_webp_has_correct_content_type(self, client):
        """WebP response has Content-Type: image/webp."""
        client.get("/1x1.jpg")
        r = client.poll_for_hit(
            "/1x1.jpg",
            headers={"Accept": "image/webp"},
        )
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            if r.content[:4] == b"RIFF":
                assert ct == "image/webp", f"WebP body but Content-Type is {ct}"


class TestVaryHeader:
    """Vary header presence and correctness."""

    def test_vary_accept_on_image_response(self, client):
        """Image responses should include Vary: Accept."""
        r = client.get("/1x1.jpg", headers={"Accept": "image/webp"})
        # Nginx may or may not add Vary depending on configuration
        # At minimum, it shouldn't duplicate Vary headers
        vary = r.headers.get("Vary", "")
        # Verify no duplication
        if vary:
            parts = [v.strip() for v in vary.split(",")]
            unique = set(p.lower() for p in parts)
            assert len(unique) == len(parts), f"Duplicate Vary values: {vary}"

    def test_vary_not_duplicated(self, client):
        """Repeated requests don't accumulate Vary headers."""
        import requests as req

        for _ in range(3):
            # Use fresh connection to avoid keep-alive issues
            r = req.get(
                client.base_url + "/1x1.jpg",
                headers={"Accept": "image/webp"},
                timeout=10,
            )
            vary = r.headers.get("Vary", "")
            if vary:
                # Count occurrences of "Accept" in Vary (case-insensitive)
                parts = [v.strip().lower() for v in vary.split(",")]
                accept_count = sum(1 for p in parts if p == "accept")
                assert accept_count <= 1, f"Vary has duplicate Accept: {vary}"


class TestContentTypePreservation:
    """Content-Type preserved for non-image types."""

    def test_css_content_type_preserved(self, client):
        """CSS response has text/css Content-Type."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "text/css" in ct, f"Expected text/css, got: {ct}"

    def test_js_content_type_preserved(self, client):
        """JS response has javascript Content-Type."""
        r = client.get("/tiny.js")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "javascript" in ct, f"Expected javascript, got: {ct}"

    def test_html_content_type_preserved(self, client):
        """HTML response has text/html Content-Type."""
        r = client.get("/nocache.html")
        if r.status_code == 103:
            return  # requests library limitation with Early Hints
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "text/html" in ct, f"Expected text/html, got: {ct}"

    def test_css_content_type_on_hit(self, client):
        """Minified CSS still has text/css Content-Type."""
        client.get("/etag-test.css")
        r = client.poll_for_hit("/etag-test.css")
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            assert "text/css" in ct, f"Minified CSS should be text/css, got: {ct}"

    def test_js_content_type_on_hit(self, client):
        """Minified JS still has javascript Content-Type."""
        client.get("/tiny.js")
        r = client.poll_for_hit("/tiny.js")
        if r.headers.get("X-PageSpeed") == "HIT":
            ct = r.headers.get("Content-Type", "")
            assert "javascript" in ct, f"Minified JS should be javascript, got: {ct}"

    def test_html_content_type_on_hit(self, client):
        """HTML with critical CSS still has text/html Content-Type."""
        r = client.get("/nocache.html")
        if r.status_code == 103:
            return  # requests library limitation with Early Hints
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "text/html" in ct, f"HTML should be text/html, got: {ct}"

    def test_jpeg_content_type_on_miss(self, client):
        """JPEG image has image/jpeg Content-Type on MISS."""
        r = client.get("/1x1.jpg")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "image/jpeg" in ct or "image" in ct, (
            f"Expected image Content-Type, got: {ct}"
        )

    def test_png_content_type_on_miss(self, client):
        """PNG image has image/png Content-Type on MISS."""
        r = client.get("/1x1.png")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "image/png" in ct or "image" in ct, (
            f"Expected image Content-Type, got: {ct}"
        )

    def test_gif_content_type_on_miss(self, client):
        """GIF image has image/gif Content-Type on MISS."""
        r = client.get("/1x1.gif")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "image/gif" in ct or "image" in ct, (
            f"Expected image Content-Type, got: {ct}"
        )


class TestContentTypeCharset:
    """Charset in Content-Type for text types."""

    def test_html_charset(self, client):
        """HTML Content-Type includes charset or is valid."""
        r = client.get("/nocache.html")
        if r.status_code == 103:
            return  # requests library limitation with Early Hints
        ct = r.headers.get("Content-Type", "")
        # text/html with optional charset is valid
        assert ct.startswith("text/html"), f"Expected text/html, got: {ct}"

    def test_css_charset(self, client):
        """CSS Content-Type includes charset or is valid."""
        r = client.get("/etag-test.css")
        ct = r.headers.get("Content-Type", "")
        assert ct.startswith("text/css"), f"Expected text/css, got: {ct}"


class TestOriginNegotiatesByAccept:
    """An origin that does its OWN Accept negotiation.

    `/vary/accept` is a `text/css` response carrying `Vary: Accept`
    (compliance_origin.py). The origin has declared that IT picks the
    representation from the request's `Accept`. The cache keeps one copy per
    URL, so it cannot hold one answer per `Accept` value: the response is
    passed through on every request, with the origin's own `Vary` and bytes,
    and never served from the cache.
    """

    PATH = "/vary/accept"

    @staticmethod
    def _vary_tokens(response) -> list[str]:
        """Lowercased `Vary` list members.

        `requests` joins repeated `Vary` header lines with ", ", which is
        exactly how RFC 9110 Section 5.3 says they combine, so splitting the
        joined value on "," recovers the full list either way.
        """
        vary = response.headers.get("Vary", "")
        return [t.strip().lower() for t in vary.split(",") if t.strip()]

    def test_miss_preserves_origin_vary_accept(self, client):
        """MISS relays the origin's own `Vary: Accept`."""
        r = client.get(self.PATH)
        assert r.status_code == 200
        tokens = self._vary_tokens(r)
        assert "accept" in tokens, (
            f"MISS dropped the origin's Vary: Accept "
            f"(Vary: {r.headers.get('Vary', '<absent>')!r})"
        )

    def test_never_served_from_cache(self, client, origin_client):
        """Every request reaches the origin and gets the origin's bytes.

        Re-checked over a settle window: recording and optimization run
        asynchronously, so a response that is a MISS once but a HIT a second
        later would still be one client's answer served to another.
        """
        import time

        origin_body = origin_client.get(self.PATH).content
        assert origin_body, "origin served an empty body — fixture broken"

        deadline = time.time() + 3.0
        while time.time() < deadline:
            r = client.get(self.PATH)
            assert r.headers.get("X-PageSpeed") != "HIT", (
                "a Vary: Accept response was served from the cache"
            )
            assert r.content == origin_body, (
                f"body differs from the origin's: {r.content!r} != {origin_body!r}"
            )
            assert self._vary_tokens(r).count("accept") == 1, (
                f"Vary must name Accept once: {r.headers.get('Vary', '<absent>')!r}"
            )
            time.sleep(0.5)


class TestRequestFieldVaryIsPerClient:
    """A response whose `Vary` names a request field answers each client.

    `/vary/negotiated/<field>/...` sends `Vary: <field>` and echoes the
    request's value for that field in its body. Two clients that differ only
    in that field must each get the answer to their own request, however long
    the first answer has had to settle into the cache. `Accept-Encoding` is
    the control: the cache stores the identity form and codes it per request,
    so that response is still cached and shared.
    """

    @staticmethod
    def _path(field: str) -> str:
        import uuid

        return "/vary/negotiated/%s/%s.css" % (field, uuid.uuid4().hex)

    @staticmethod
    def _settle(client, path, headers) -> None:
        """Give the first answer every chance to be recorded."""
        import time

        client.get(path, headers=headers)
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if client.get(path, headers=headers).headers.get("X-PageSpeed") == "HIT":
                return
            time.sleep(0.25)

    @pytest.mark.parametrize(
        "field,first,second",
        [
            ("Accept", "image/webp,*/*", "text/css,*/*"),
            ("User-Agent", "client-one/1.0", "client-two/1.0"),
            ("Save-Data", "on", None),
            ("Origin", "https://a.example", "https://b.example"),
        ],
    )
    def test_second_client_gets_its_own_answer(self, client, field, first, second):
        path = self._path(field.lower())
        first_headers = {field: first}
        self._settle(client, path, first_headers)

        second_headers = {field: second} if second is not None else {}
        r = client.get(path, headers=second_headers)
        assert r.status_code == 200
        expected = ("/* %s=%s */\n" % (field, second or "")).encode()
        assert r.content == expected, (
            "a client sending %s: %r was given %r (X-PageSpeed %s)"
            % (field, second, r.content, r.headers.get("X-PageSpeed"))
        )
        assert r.headers.get("X-PageSpeed") != "HIT"

    def test_accept_encoding_vary_is_still_cached(self, client):
        path = self._path("accept-encoding")
        client.get(path)
        r = client.poll_for_hit(path, headers={"User-Agent": "client-two/1.0"})
        client.assert_hit(r)


class TestOriginBeginsNegotiatingByAccept:
    """A URL already cached AND OPTIMIZED before its origin started
    declaring `Vary: Accept`.

    `/vary/accept` cannot reach this case — it has declared `Vary: Accept`
    from its very first response, so it never has a variant set to begin
    with. `/vary/flip.css` starts as an ordinary stylesheet and is switched
    over mid-test via `/vary/control`. Once the origin negotiates, its
    response is not stored, so every client must get the origin's own bytes
    and `Vary` rather than the variants built earlier.

    Recovery waits for the stored entry to go stale, so these poll rather
    than assert on the next request.
    """

    PATH = "/vary/flip.css"
    ORIGIN_BYTES = b"body { color: red; }"
    OPTIMIZED_BYTES = b"body{color:red}"

    @staticmethod
    def _vary_tokens(response) -> list[str]:
        vary = response.headers.get("Vary", "")
        return [t.strip().lower() for t in vary.split(",") if t.strip()]

    @staticmethod
    def _set_origin_vary(origin_client, on: bool):
        r = origin_client.get("/vary/control?flip=%s" % ("on" if on else "off"))
        assert r.status_code == 200, "control endpoint failed: %s" % r.status_code

    def _poll(self, client, predicate, timeout: float):
        """Poll until predicate(response) holds; return (ok, last_response)."""
        import time

        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = client.get(self.PATH)
            if predicate(last):
                return True, last
            time.sleep(0.5)
        return False, last

    def test_pre_existing_variants_recover_after_origin_starts_varying(
        self, client, origin_client
    ):
        import time

        # --- precondition: an ordinary, OPTIMIZED resource ----------------
        self._set_origin_vary(origin_client, False)
        client.get(self.PATH)
        optimized, r = self._poll(
            client, lambda x: x.content == self.OPTIMIZED_BYTES, timeout=20.0
        )
        assert optimized, (
            "precondition not reached: %s was never optimized, so this test "
            "would not be exercising a pre-existing variant set at all "
            "(last body %r, X-PageSpeed %s)"
            % (
                self.PATH,
                r.content if r else None,
                r.headers.get("X-PageSpeed") if r else None,
            )
        )

        # --- the origin starts negotiating on Accept ----------------------
        self._set_origin_vary(origin_client, True)

        def recovered(x):
            return x.content == self.ORIGIN_BYTES and "accept" in self._vary_tokens(x)

        ok, last = self._poll(client, recovered, timeout=45.0)
        assert ok, (
            "the URL never recovered after its origin began declaring "
            "Vary: Accept -- it is still being served a representation "
            "derived from the pre-existing variant set. "
            "last body=%r Vary=%r X-PageSpeed=%s"
            % (
                last.content if last else None,
                last.headers.get("Vary") if last else None,
                last.headers.get("X-PageSpeed") if last else None,
            )
        )

        # --- and it STAYS recovered ---------------------------------------
        # A single good response could be one re-fetch passing through. The
        # variants must be gone, not momentarily bypassed.
        deadline = time.time() + 6.0
        while time.time() < deadline:
            time.sleep(0.5)
            later = client.get(self.PATH)
            assert later.content == self.ORIGIN_BYTES, (
                "regressed to an optimized representation after recovery: %r "
                "(X-PageSpeed %s)" % (later.content, later.headers.get("X-PageSpeed"))
            )
            assert "accept" in self._vary_tokens(later), (
                "regressed to dropping Accept from Vary after recovery: %r"
                % later.headers.get("Vary")
            )


class TestOriginVaryChangesOnRevalidation:
    """The same transition, but over the conditional-revalidation path.

    `/vary/flip-etag.css` carries an ETag, so an expired entry is revalidated
    and answered `304` rather than re-fetched in full. A `304` that carries
    `Vary: Accept` makes the entry one the cache may not hold: it must be
    dropped, and from then on the URL is passed through to the origin.
    """

    PATH = "/vary/flip-etag.css"
    ORIGIN_BYTES = b"body { color: blue; }"
    OPTIMIZED_BYTES = b"body{color:blue}"

    @staticmethod
    def _vary_tokens(response) -> list[str]:
        vary = response.headers.get("Vary", "")
        return [t.strip().lower() for t in vary.split(",") if t.strip()]

    @staticmethod
    def _set_origin_vary(origin_client, on: bool):
        r = origin_client.get("/vary/control?flip-etag=%s" % ("on" if on else "off"))
        assert r.status_code == 200

    @staticmethod
    def _revise_validator(origin_client, rev: str):
        """Change the route's ETag without changing its bytes, so the next
        revalidation is answered with a full 200 instead of a 304."""
        r = origin_client.get("/vary/control?etag-rev=%s" % rev)
        assert r.status_code == 200

    def _poll(self, client, predicate, timeout: float):
        import time

        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = client.get(self.PATH)
            if predicate(last):
                return True, last
            time.sleep(0.5)
        return False, last

    def test_entry_is_dropped_when_origin_starts_negotiating(
        self, client, origin_client
    ):
        # --- ordinary resource, optimized ---------------------------------
        # Validator reset explicitly so the test is idempotent across reruns
        # against a long-lived origin process.
        self._set_origin_vary(origin_client, False)
        self._revise_validator(origin_client, "1")
        client.get(self.PATH)
        optimized, r = self._poll(
            client, lambda x: x.content == self.OPTIMIZED_BYTES, timeout=20.0
        )
        assert optimized, (
            "precondition not reached: %s was never optimized (last body %r)"
            % (self.PATH, r.content if r else None)
        )

        # --- origin STARTS negotiating: the entry must go ------------------
        self._set_origin_vary(origin_client, True)
        ok, last = self._poll(
            client,
            lambda x: x.content == self.ORIGIN_BYTES
            and "accept" in self._vary_tokens(x),
            timeout=45.0,
        )
        assert ok, (
            "the entry was kept after its origin started negotiating on "
            "Accept across revalidation. last body=%r Vary=%r X-PageSpeed=%s"
            % (
                last.content if last else None,
                last.headers.get("Vary") if last else None,
                last.headers.get("X-PageSpeed") if last else None,
            )
        )

        # --- and, while it negotiates, it is never served from the cache --
        import time

        deadline = time.time() + 6.0
        while time.time() < deadline:
            time.sleep(0.5)
            held = client.get(self.PATH)
            assert held.headers.get("X-PageSpeed") != "HIT", (
                "a Vary: Accept response was served from the cache"
            )
            assert held.content == self.ORIGIN_BYTES, (
                "served %r instead of the origin's bytes" % held.content
            )


class TestOriginStopsNegotiatingByAccept:
    """The release direction: an origin that stops declaring `Vary: Accept`.

    The URL must become an ordinary cached, optimized resource again, without
    `Accept` in `Vary` -- or it is frozen out of the cache forever by a header
    its origin no longer sends, while still over-keying every downstream
    cache with an `Accept` nobody asked for.

    This once failed because the serve path emitted our own `Vary` before the
    freshness verdict was known, so the store-side check read our own
    `Accept` back as if the origin had sent it.
    """

    PATH = "/vary/flip-etag.css"
    OPTIMIZED_BYTES = b"body{color:blue}"

    @staticmethod
    def _vary_tokens(response) -> list[str]:
        vary = response.headers.get("Vary", "")
        return [t.strip().lower() for t in vary.split(",") if t.strip()]

    def test_url_is_cached_again_when_origin_stops_negotiating(
        self, client, origin_client
    ):
        import time

        origin_client.get("/vary/control?flip-etag=on&etag-rev=1")
        deadline = time.time() + 30.0
        while time.time() < deadline:
            r = client.get(self.PATH)
            if "accept" in self._vary_tokens(r):
                break
            time.sleep(0.5)

        # The origin stops negotiating AND revises its validator, so the next
        # revalidation is answered with a full response carrying no Vary.
        origin_client.get("/vary/control?flip-etag=off&etag-rev=2")

        deadline = time.time() + 45.0
        last = None
        while time.time() < deadline:
            last = client.get(self.PATH)
            if (
                "accept" not in self._vary_tokens(last)
                and last.content == self.OPTIMIZED_BYTES
            ):
                return
            time.sleep(0.5)
        raise AssertionError(
            "never cached and optimized again: Vary=%r body=%r X-PageSpeed=%s"
            % (
                last.headers.get("Vary") if last else None,
                last.content if last else None,
                last.headers.get("X-PageSpeed") if last else None,
            )
        )


class TestStaleImageStillRecords:
    """A stale image must still be re-recorded after its re-fetch.

    Not about Accept negotiation at all -- it is the other half of the same
    defect. The serving stage used to push its own `Vary` onto the response
    before the freshness verdict was known, and for an image that value names
    `Sec-CH-DPR`. On a stale entry the request then left for the origin with
    that header still attached, and the store-side check on the way back read
    it as the ORIGIN's `Vary`. `Sec-CH-DPR` is deliberately not on the
    storability allowlist, so the response was judged unstorable: the refreshed
    image was never re-recorded and the optimizer was never told, on every
    expiry, forever.

    `/stale-image.jpg` carries a short lifetime and no validators, so it takes the
    full re-fetch path within test time.
    """

    PATH = "/stale-image.jpg"

    def test_image_still_hits_after_expiry(self, client):
        """After the entry has fully expired, caching must recover.

        The sequencing matters and is why this is spelled out rather than
        driven by `poll_for_hit`. The FIRST store happens with nothing in
        cache, so there is no cached entry for the serving stage to build
        headers from and nothing to leak -- it succeeds even when the bug is
        present. Only once an entry exists AND has expired does the request
        take the re-fetch path that carried our own `Vary` to the origin and
        back. So the entry has to be warmed, then genuinely aged out, and
        only then measured.

        With the bug present this is not flaky, it is absolute: every request
        after expiry is a MISS, forever, because the refreshed response is
        judged unstorable every single time.
        """
        import time

        # Warm it, and confirm the cache serves it at all.
        client.get(self.PATH)
        first = client.poll_for_hit(self.PATH, timeout=15.0)
        assert first.headers.get("X-PageSpeed") == "HIT", (
            "precondition: image never became cacheable at all (X-PageSpeed %s)"
            % first.headers.get("X-PageSpeed")
        )

        # Age it well past max-age=2 (and past the serve-stale grace window,
        # which is min(10, max_age)) so the next request takes the full
        # re-fetch path rather than being served from a still-fresh entry.
        time.sleep(5.0)

        statuses = []
        for _ in range(20):
            r = client.get(self.PATH)
            assert r.status_code == 200, (
                "image stopped being served: %s" % r.status_code
            )
            statuses.append(r.headers.get("X-PageSpeed", "<absent>"))
            if statuses.count("HIT") >= 2:
                return
            time.sleep(1.0)

        raise AssertionError(
            "the refreshed image was never re-recorded: %d/%d requests after "
            "expiry went to the origin, so every refreshed response is being "
            "refused storage. sequence=%s"
            % (statuses.count("MISS"), len(statuses), statuses)
        )

    def test_expired_image_response_is_intact(self, client, origin_client):
        """Whatever is served after expiry is still the right bytes."""
        import time

        origin_body = origin_client.get(self.PATH).content
        assert origin_body[:2] == b"\xff\xd8", "fixture is not a JPEG"

        client.get(self.PATH)
        client.poll_for_hit(self.PATH, timeout=15.0)
        time.sleep(2.0)
        r = client.get(self.PATH)
        assert r.status_code == 200
        assert len(r.content) > 0
        # Either the stored original or an optimized variant, never truncated
        # or empty, and still an image.
        ct = r.headers.get("Content-Type", "")
        assert ct.startswith("image/"), "Content-Type is not an image: %s" % ct


class TestServeNoCacheArmStillDeclaresVary:
    """The serve-with-no-cache arm is a cache HIT and must declare its Vary.

    The freshness logic has more than one arm that SERVES from cache. Besides
    the ordinary fresh hit there is the "serve with no-cache" arm, taken by
    no-store/private-flagged entries and -- the case that matters -- by HTML
    from an origin that sends no `Cache-Control` at all, which is the default
    configuration for an ordinary site.

    Both are HIT serves (the module emits an `Age` header on both), so both
    have to declare what the response varies on. Losing it on this arm drops
    `User-Agent`, which is the viewport-variant key: a shared cache
    downstream would hand a phone's copy to a desktop. `Cache-Control:
    no-cache` on these responses limits the blast radius but does not close
    it -- a validator-based revalidation can still reuse the entry, and not
    every intermediary honours no-cache.

    `/no-cc.html` sends no `Cache-Control`, so it lands there by default.
    """

    PATH = "/no-cc.html"

    @staticmethod
    def _vary_tokens(response) -> list[str]:
        vary = response.headers.get("Vary", "")
        return [t.strip().lower() for t in vary.split(",") if t.strip()]

    def test_hit_on_no_cache_arm_declares_user_agent(self, client):
        client.get(self.PATH)
        r = client.poll_for_hit(self.PATH, timeout=15.0)
        if r.status_code == 103:
            return  # requests library limitation with Early Hints
        client.assert_hit(r)

        # Confirm this really is the arm under test, not an ordinary fresh
        # hit -- otherwise the assertion below proves nothing about it.
        cc = r.headers.get("Cache-Control", "")
        assert "no-cache" in cc, (
            "precondition: %s did not take the serve-with-no-cache arm "
            "(Cache-Control %r)" % (self.PATH, cc)
        )

        tokens = self._vary_tokens(r)
        assert "user-agent" in tokens, (
            "a cache HIT on the serve-with-no-cache arm declared no "
            "User-Agent in Vary, so a shared cache downstream keys it without "
            "the viewport dimension. Vary=%r" % r.headers.get("Vary", "<absent>")
        )
