# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Response-header sidecar (issue #208).

Store side: the nginx front stage classifies the origin's response headers
before it records a response:

* only reproducible headers (the security set: ACAO, CSP, nosniff,
  Referrer-Policy) -> recorded, with the headers stored beside the entry
  (the stored-block counter moves);
* any header a cache hit cannot reproduce (X-Request-Id, a custom header)
  -> never cached, served from the origin each time, and an entry recorded
  earlier under different headers is removed;
* a Content-Security-Policy carrying a nonce -> never cached;
* Vary: Origin -> never cached (the existing Vary gate).

Serve side: every response served from the cache — a fresh HIT, the
bounded stale serve while a refresh is in flight, the stale-if-error serve,
and a 304-revalidated serve — carries the same stored lines the MISS
relayed; a cached entry whose stored lines have gone is a MISS (and is
recorded again), never a hit without them.

The counters live in the shared serve-statistics file the front stage writes
and the optimizer exports on /v1/metrics.  The compliance stack does not
expose the optimizer API, so the counters are read from the file itself,
inside the nginx container.
"""

import os
import socket
import struct
import subprocess
import threading
import time

import pytest

COMPOSE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "docker-compose.yml")

SERVE_STATS_PATH = "/shared/.pagespeed-serve-stats"
SERVE_STATS_VERSION = 11
# v11 layout (src/worker/serve_stats.h, sizeof == 11496): the six sidecar
# counters sit just before counting_since_unix_day (8 bytes) and boot_id (16).
SIDECAR_COUNTER_NAMES = (
    "fall_through",
    "never_optimized",
    "replayed",
    "missing",
    "write_failed",
    "stored",
)
SIDECAR_COUNTERS_OFFSET = 11496 - 16 - 8 - len(SIDECAR_COUNTER_NAMES) * 8


def read_sidecar_counters() -> dict[str, int]:
    """Read the six v11 sidecar counters from the shared stats file."""
    out = subprocess.run(
        [
            "docker",
            "compose",
            "-f",
            COMPOSE_FILE,
            "exec",
            "-T",
            "nginx",
            "cat",
            SERVE_STATS_PATH,
        ],
        capture_output=True,
        check=False,
    )
    size = len(SIDECAR_COUNTER_NAMES) * 8
    if out.returncode != 0 or len(out.stdout) < SIDECAR_COUNTERS_OFFSET + size:
        pytest.skip(
            "serve statistics file not readable from the nginx container: "
            + out.stderr.decode(errors="replace")[:200]
        )
    _magic, version = struct.unpack_from("<II", out.stdout, 0)
    assert version == SERVE_STATS_VERSION, (
        f"serve statistics file is version {version}, expected "
        f"{SERVE_STATS_VERSION}: the layout offsets here are stale"
    )
    values = struct.unpack_from(
        "<%dQ" % len(SIDECAR_COUNTER_NAMES), out.stdout, SIDECAR_COUNTERS_OFFSET
    )
    return dict(zip(SIDECAR_COUNTER_NAMES, values))


# The names a cached copy carries in its stored header block (the sidecar
# allowlist), lowercased.
CARRIED_NAMES = (
    "content-security-policy",
    "access-control-allow-origin",
    "x-content-type-options",
    "referrer-policy",
    "cross-origin-opener-policy",
    "cross-origin-embedder-policy",
    "cross-origin-resource-policy",
    "permissions-policy",
    "vary",
    "link",
    "strict-transport-security",
    "x-frame-options",
    "x-xss-protection",
    "x-permitted-cross-domain-policies",
    "origin-agent-cluster",
    "x-dns-prefetch-control",
    "timing-allow-origin",
    "access-control-expose-headers",
    "reporting-endpoints",
    "nel",
    "x-powered-by",
    "x-pingback",
    "x-robots-tag",
    "content-language",
    "content-disposition",
    "alt-svc",
    "allow",
)

IDENTITY = {"Accept-Encoding": "identity"}
NGINX_HOST = f"localhost:{os.environ.get('COMPLIANCE_NGINX_PORT', '8180')}"


def carried(response, names=CARRIED_NAMES) -> dict[str, list[str]]:
    """The response's lines of the carried names, one entry per received
    line (repeated lines stay separate), as a per-name multiset."""
    out = {}
    for name in names:
        values = response.raw.headers.getlist(name)
        if values:
            out[name] = sorted(values)
    return out


def without_vary(lines: dict[str, list[str]]) -> dict[str, list[str]]:
    return {k: v for k, v in lines.items() if k != "vary"}


def without_link(lines: dict[str, list[str]]) -> dict[str, list[str]]:
    return {k: v for k, v in lines.items() if k != "link"}


def wait_for_optimized_hit(client, path: str, miss):
    """Poll until the optimizer's (smaller) variant is served from cache, so
    the entry later requests see is the one whose age the test controls."""
    hit = client.poll_for_hit(
        path, headers=IDENTITY, timeout=20, expect_smaller_than=len(miss.content)
    )
    client.assert_hit(hit)
    assert len(hit.content) < len(miss.content), "no optimized variant"
    return hit


def serve_control(origin_client, **params):
    query = "&".join(f"{k}={v}" for k, v in params.items())
    r = origin_client.get("/sidecar/serve-control?" + query)
    assert r.status_code == 200, r.status_code


def remove_stored_headers(path: str):
    """Remove the URL's stored header block (sentinel 0x6C) behind the
    running server's back, leaving its cached entries in place."""
    out = subprocess.run(
        [
            "docker",
            "compose",
            "-f",
            COMPOSE_FILE,
            "exec",
            "-T",
            "nginx",
            "seed_test_cache",
            "--cache",
            "/shared/cache.vol",
            "--url",
            path,
            "--host",
            NGINX_HOST,
            "--scheme",
            "http",
            # The safe-mode server block (nginx-compliance.conf): its
            # listener, and no server_name of its own.
            "--site_listen",
            "0.0.0.0:8080",
            "--site_names",
            "",
            "--remove_alternate",
            "0x6c",
        ],
        capture_output=True,
        check=False,
    )
    assert out.returncode == 0, out.stderr.decode(errors="replace")[:400]


def raw_get(path: str, extra=None):
    """GET on a raw socket: (status line, header lines, body).  The status
    line is asserted verbatim, so a stale line paired with a rewritten code
    cannot hide behind the parsed status."""
    host, _, port = NGINX_HOST.partition(":")
    lines = [f"GET {path} HTTP/1.1", f"Host: {NGINX_HOST}", "Connection: close"]
    lines.append("Accept-Encoding: identity")
    lines += [f"{k}: {v}" for k, v in (extra or {}).items()]
    with socket.create_connection((host, int(port)), timeout=15) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        data = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    status_line, *header_lines = head.decode("latin-1").split("\r\n")
    return status_line, header_lines, body


def raw_header(header_lines, name: str):
    prefix = name.lower() + ":"
    return [l.split(":", 1)[1].strip() for l in header_lines if l.lower().startswith(prefix)]


def x_pagespeed(response) -> str:
    return response.headers.get("X-PageSpeed", "<absent>")


def requests_never_hit(client, path: str, n: int = 6, pause: float = 0.3):
    """Request `path` n times with pauses; return the X-PageSpeed values."""
    seen = []
    for _ in range(n):
        r = client.get(path)
        assert r.status_code == 200, f"{path}: {r.status_code}"
        seen.append(x_pagespeed(r))
        time.sleep(pause)
    return seen


class TestHeadersSidecarRecord:
    def test_reproducible_security_headers_are_cached(self, client):
        """ACAO + CSP + nosniff + Referrer-Policy: recorded WITH a stored
        header block (the stored counter moves; a build that does not store
        the block leaves it alone), then a HIT; no refusal is counted."""
        before = read_sidecar_counters()
        first = client.get("/sidecar/asset.css")
        assert first.status_code == 200
        # The MISS passes every origin header through untouched.
        assert first.headers.get("Access-Control-Allow-Origin") == "*"
        assert first.headers.get("Referrer-Policy") == "no-referrer"
        hit = client.poll_for_hit("/sidecar/asset.css")
        client.assert_hit(hit)
        after = read_sidecar_counters()
        assert after["stored"] > before["stored"], (before, after)
        assert after["fall_through"] == before["fall_through"]
        assert after["never_optimized"] == before["never_optimized"]
        assert after["write_failed"] == before["write_failed"]

    def test_unreproducible_header_is_never_cached(self, client):
        """X-Request-Id cannot be reproduced on a hit: never a HIT."""
        before = read_sidecar_counters()
        seen = requests_never_hit(client, "/sidecar/custom.css")
        assert "HIT" not in seen, seen
        assert all(v == "MISS" for v in seen), seen
        # The origin's header reaches the client on every (origin) response.
        r = client.get("/sidecar/custom.css")
        assert r.headers.get("X-Request-Id") == "req-8f3a"
        after = read_sidecar_counters()
        assert after["fall_through"] >= before["fall_through"] + len(seen), (
            before,
            after,
        )

    def test_nonce_csp_is_never_cached(self, client):
        before = read_sidecar_counters()
        seen = requests_never_hit(client, "/sidecar/nonce.html")
        assert "HIT" not in seen, seen
        after = read_sidecar_counters()
        assert after["never_optimized"] >= before["never_optimized"] + len(seen)
        # Disjoint from the fall-through class.
        assert after["fall_through"] == before["fall_through"]

    def test_vary_origin_is_never_cached(self, client):
        before = read_sidecar_counters()
        seen = requests_never_hit(client, "/sidecar/vary-origin.css")
        assert "HIT" not in seen, seen
        # Refused by the Vary gate before the sidecar gate runs: not counted
        # here a second time.
        after = read_sidecar_counters()
        assert after["fall_through"] == before["fall_through"]

    def test_header_change_evicts_the_cached_entry(self, client, origin_client):
        """A cached URL whose origin starts sending an unreproducible header
        stops being served from cache at the next origin fetch."""
        path = "/sidecar/flip.css"
        origin_client.get("/sidecar/control?custom=off")
        try:
            client.get(path)
            hit = client.poll_for_hit(path)
            client.assert_hit(hit)

            origin_client.get("/sidecar/control?custom=on")
            before = read_sidecar_counters()
            # max-age=1: wait past expiry so the next request goes to origin.
            time.sleep(2.5)
            refetch = client.get(path)
            assert refetch.headers.get("X-Custom-Foo") == "bar"
            assert x_pagespeed(refetch) != "HIT"
            seen = requests_never_hit(client, path, n=4)
            assert "HIT" not in seen, seen
            after = read_sidecar_counters()
            assert after["fall_through"] > before["fall_through"]
        finally:
            origin_client.get("/sidecar/control?custom=off")


class TestHeadersSidecarReplay:
    def test_hit_carries_the_miss_headers(self, client):
        """ACAO + CSP + nosniff + Referrer-Policy + the origin's Vary: the HIT
        sends the same lines as the MISS, and the replayed counter moves."""
        path = "/sidecar/parity.css"
        miss = client.get(path, headers=IDENTITY)
        assert miss.status_code == 200
        assert x_pagespeed(miss) == "MISS"
        expected = carried(miss)
        assert "Accept-Encoding" in expected.get("vary", []), expected
        before = read_sidecar_counters()
        hit = client.poll_for_hit(path, headers=IDENTITY)
        client.assert_hit(hit)
        assert carried(hit) == expected
        after = read_sidecar_counters()
        assert after["replayed"] > before["replayed"], (before, after)
        assert after["missing"] == before["missing"], (before, after)

    def test_gzip_hit_carries_the_miss_headers(self, client):
        """The same on a HIT for a gzip-accepting client.  Vary is compared
        by the origin's line only: nginx's own gzip filter adds a Vary line
        to a response it compresses itself, which a pre-compressed cached
        variant is not."""
        path = "/sidecar/parity.css"
        miss = client.get(path, headers=IDENTITY)
        expected = carried(miss)
        client.poll_for_hit(path, headers=IDENTITY)
        hit = client.poll_for_hit(path, headers={"Accept-Encoding": "gzip"})
        client.assert_hit(hit)
        assert without_vary(carried(hit)) == without_vary(expected)
        assert "Accept-Encoding" in hit.raw.headers.getlist("vary")

    def test_optimized_html_hit_carries_the_miss_headers(self, client):
        """Once the optimizer's variant of the page is served, the HIT still
        sends the origin's lines."""
        path = "/sidecar/page.html"
        miss = client.get(path, headers=IDENTITY)
        assert miss.status_code == 200
        expected = carried(miss)
        assert "default-src 'self'" in expected.get("content-security-policy", [])
        deadline = time.time() + 30
        hit = None
        while time.time() < deadline:
            r = client.get(path, headers=IDENTITY)
            if x_pagespeed(r) == "HIT" and r.content != miss.content:
                hit = r
                break
            time.sleep(0.5)
        assert hit is not None, "the optimized page was never served from cache"
        # An HTML hit also carries the module's own preload hints (the Link
        # lines built from the page's Early Hints record, e.g. the hero image
        # preload), which the origin MISS cannot have.  The origin's lines
        # must all be replayed; Link is compared as "the origin's lines plus
        # exactly the page's known hints" (the page references only
        # /1x1.jpg).
        hit_lines = carried(hit)
        assert without_link(hit_lines) == without_link(expected)
        hit_links = list(hit_lines.get("link", []))
        for line in expected.get("link", []):
            assert line in hit_links, (line, hit_links)
            hit_links.remove(line)
        assert sorted(hit_links) == [
            "</1x1.jpg>; rel=preload; as=image; fetchpriority=high"
        ], hit_links

    def test_missing_stored_headers_is_a_miss_then_rerecorded(self, client):
        """A cached URL whose stored lines are removed behind the server's
        back is served from the origin (counted as missing), not as a hit
        without them, and the next requests are HITs with the lines again."""
        path = "/sidecar/heal.css"
        miss = client.get(path, headers=IDENTITY)
        expected = carried(miss)
        hit = client.poll_for_hit(path, headers=IDENTITY)
        client.assert_hit(hit)

        remove_stored_headers(path)
        before = read_sidecar_counters()
        refused = client.get(path, headers=IDENTITY)
        assert refused.status_code == 200
        assert x_pagespeed(refused) == "MISS"
        assert carried(refused) == expected
        after = read_sidecar_counters()
        assert after["missing"] > before["missing"], (before, after)

        healed = client.poll_for_hit(path, headers=IDENTITY)
        client.assert_hit(healed)
        assert carried(healed) == expected

    def test_coalesced_stale_serve_carries_the_headers(self, client, origin_client):
        """While one request re-fetches an expired URL from a slow origin, a
        concurrent request is served the stale cached copy — with the lines."""
        path = "/sidecar/slow.css"
        miss = client.get(path, headers=IDENTITY)
        expected = carried(miss)
        wait_for_optimized_hit(client, path, miss)
        try:
            serve_control(origin_client, delay="2.5")
            # Past max-age=2 with a margin: ages are whole seconds.
            time.sleep(3.5)
            refresh = {}

            def refetch():
                refresh["r"] = requests_get(client, path)

            t = threading.Thread(target=refetch)
            t.start()
            time.sleep(0.5)
            stale = client.get(path, headers=IDENTITY)
            t.join(timeout=15)
            assert x_pagespeed(stale) == "HIT", stale.headers
            assert int(stale.headers.get("Age", "0")) >= 2, stale.headers
            assert carried(stale) == expected
            assert "r" in refresh and refresh["r"].status_code == 200
        finally:
            serve_control(origin_client, delay="0")

    def test_stale_if_error_serve_carries_the_stored_headers(
        self, client, origin_client
    ):
        """An expired URL whose origin now answers 500 is served stale with
        the cached copy's lines, and none of the error page's."""
        path = "/sidecar/flaky.css"
        miss = client.get(path, headers=IDENTITY)
        expected = carried(miss)
        hit = wait_for_optimized_hit(client, path, miss)
        try:
            serve_control(origin_client, fail="on")
            time.sleep(2.5)  # past max-age=1, ages are whole seconds
            stale = client.get(path, headers=IDENTITY)
            assert x_pagespeed(stale) == "STALE", stale.headers
            assert stale.headers.get("X-PageSpeed-Stale") == "if-error"
            assert stale.content == hit.content
            assert stale.status_code == 200
            assert stale.raw.reason == "OK", stale.raw.reason
            assert carried(stale) == expected
            assert "https://error.example" not in stale.raw.headers.getlist(
                "access-control-allow-origin"
            )
            assert "default-src 'none'" not in stale.raw.headers.getlist(
                "content-security-policy"
            )
        finally:
            serve_control(origin_client, fail="off")

    def test_revalidated_serve_carries_the_headers(self, client, origin_client):
        """An expired URL revalidated with a 304 carries the stored lines the
        304 did not repeat plus the 304's own; when the 304 changes
        Referrer-Policy, later HITs send the new value."""
        path = "/sidecar/reval.css"
        try:
            serve_control(origin_client, rp="no-referrer")
            miss = client.get(path, headers=IDENTITY)
            expected = carried(miss)
            wait_for_optimized_hit(client, path, miss)

            time.sleep(2.5)  # past max-age=1, ages are whole seconds
            revalidated = client.get(path, headers=IDENTITY)
            assert revalidated.status_code == 200
            assert x_pagespeed(revalidated) == "REVALIDATED", revalidated.headers
            assert carried(revalidated) == expected
            hit = client.poll_for_hit(path, headers=IDENTITY)
            client.assert_hit(hit)
            assert carried(hit) == expected

            serve_control(origin_client, rp="same-origin")
            time.sleep(2.5)
            changed = client.get(path, headers=IDENTITY)
            assert x_pagespeed(changed) == "REVALIDATED", changed.headers
            assert changed.raw.headers.getlist("referrer-policy") == ["same-origin"]
            hit = client.poll_for_hit(path, headers=IDENTITY)
            client.assert_hit(hit)
            want = dict(expected)
            want["referrer-policy"] = ["same-origin"]
            assert carried(hit) == want
        finally:
            serve_control(origin_client, rp="no-referrer")


def optimized_hit(client, origin_client, path: str):
    """A cache HIT of the optimizer's smaller variant.  The size to beat is
    taken from the origin (which bypasses the cache), so this works whether
    or not the URL was cached by an earlier test."""
    n = len(origin_client.get(path).content)
    client.get(path, headers=IDENTITY)
    hit = client.poll_for_hit(
        path, headers=IDENTITY, timeout=20, expect_smaller_than=n
    )
    client.assert_hit(hit)
    assert len(hit.content) < n, "no optimized variant"
    return hit


class TestRevalidatedStatusLine:
    """The status line follows the status code on the two serve paths that
    rewrite the origin's answer (issue #216)."""

    def test_stale_if_error_status_line_is_200(self, client, origin_client):
        # No optimized variant is awaited here: this URL has no validator, so
        # every expiry re-records it and the variant is a race.  The cached
        # body itself is asserted by the stale-if-error replay case above.
        path = "/sidecar/flaky.css"
        client.get(path, headers=IDENTITY)
        try:
            serve_control(origin_client, fail="on")
            time.sleep(2.5)
            status_line, header_lines, body = raw_get(path)
            assert status_line == "HTTP/1.1 200 OK", status_line
            assert raw_header(header_lines, "X-PageSpeed-Stale") == ["if-error"]
            assert body
            assert int(raw_header(header_lines, "Content-Length")[0]) == len(body)
        finally:
            serve_control(origin_client, fail="off")

    def test_revalidated_non_conditional_client_gets_200_and_body(
        self, client, origin_client
    ):
        """The conditional request was the module's own: a client that sent
        none gets the stored 200 with its body and the 304's headers
        (RFC 9111 section 4.3.4), never a bare 304."""
        path = "/sidecar/reval.css"
        try:
            serve_control(origin_client, rp="no-referrer")
            hit = optimized_hit(client, origin_client, path)
            serve_control(origin_client, rp="same-origin")
            time.sleep(2.5)
            status_line, header_lines, body = raw_get(path)
            assert status_line == "HTTP/1.1 200 OK", status_line
            assert raw_header(header_lines, "X-PageSpeed") == ["REVALIDATED"]
            assert raw_header(header_lines, "Referrer-Policy") == ["same-origin"]
            assert body == hit.content
            assert int(raw_header(header_lines, "Content-Length")[0]) == len(body)
        finally:
            serve_control(origin_client, rp="no-referrer")

    def test_revalidated_matching_conditional_client_gets_304(
        self, client, origin_client
    ):
        """A client whose own If-None-Match matches still gets a real 304,
        with no body."""
        path = "/sidecar/reval.css"
        serve_control(origin_client, rp="no-referrer")
        hit = optimized_hit(client, origin_client, path)
        time.sleep(2.5)
        # The validator the client holds is the one the revalidated response
        # carried.
        first = client.get(path, headers=IDENTITY)
        assert first.status_code == 200
        assert x_pagespeed(first) == "REVALIDATED", first.headers
        etag = first.headers.get("ETag")
        assert etag, first.headers
        time.sleep(2.5)
        status_line, header_lines, body = raw_get(path, {"If-None-Match": etag})
        assert status_line == "HTTP/1.1 304 Not Modified", status_line
        assert raw_header(header_lines, "X-PageSpeed") == ["REVALIDATED"]
        assert raw_header(header_lines, "ETag") == [etag]
        assert raw_header(header_lines, "Cache-Control")
        assert raw_header(header_lines, "Vary")
        assert raw_header(header_lines, "Referrer-Policy") == ["no-referrer"]
        assert raw_header(header_lines, "Content-Length") == []
        assert body == b""

        # A validator that does not match gets the full 200.
        time.sleep(2.5)
        status_line, header_lines, body = raw_get(
            path, {"If-None-Match": '"nomatch"'}
        )
        assert status_line == "HTTP/1.1 200 OK", status_line
        assert body == hit.content

    def test_revalidated_serve_matches_the_hit_etag(self, client, origin_client):
        """The same bytes carry the same ETag on a HIT and on a revalidated
        serve: a client holding the HIT copy gets a 304 once the origin has
        confirmed the stale entry, instead of a full 200."""
        path = "/sidecar/reval.css"
        serve_control(origin_client, rp="no-referrer")
        hit = optimized_hit(client, origin_client, path)
        hit_etag = hit.headers.get("ETag")
        assert hit_etag and hit_etag.startswith('W/"ps-'), hit.headers

        time.sleep(2.5)  # past max-age=1: the entry is stale
        status_line, header_lines, body = raw_get(path, {"If-None-Match": hit_etag})
        assert status_line == "HTTP/1.1 304 Not Modified", status_line
        assert raw_header(header_lines, "X-PageSpeed") == ["REVALIDATED"]
        assert raw_header(header_lines, "ETag") == [hit_etag]
        assert body == b""

        # Without the conditional header the revalidated 200 carries the
        # same tag.
        time.sleep(2.5)
        revalidated = client.get(path, headers=IDENTITY)
        assert revalidated.status_code == 200
        assert x_pagespeed(revalidated) == "REVALIDATED", revalidated.headers
        assert revalidated.headers.get("ETag") == hit_etag


class TestRevalidatedUnstorable:
    """A 304 to the module's own revalidation that makes the entry no longer
    storable (here `Vary: *`) must not reach a client that sent no
    conditional request (issue #220)."""

    PATH = "/sidecar/reval.css"

    @pytest.mark.parametrize("vary304", ["*", "Cookie"])
    def test_non_conditional_client_gets_200_and_entry_is_dropped(
        self, client, origin_client, vary304
    ):
        path = self.PATH
        try:
            serve_control(origin_client, rp="no-referrer", vary304="")
            # The URL lives one second, so a poll can land on a revalidation
            # as well as a hit.  Any cached copy will do: the worker records
            # the optimized variant of a URL once, so a second run of this
            # test finds the original bytes cached, and the assertion below
            # is that the stored bytes are what gets served.
            client.get(path, headers=IDENTITY)
            hit = None
            for _ in range(60):
                r = client.get(path, headers=IDENTITY)
                if x_pagespeed(r) in ("HIT", "REVALIDATED"):
                    hit = r
                    break
                time.sleep(0.3)
            assert hit is not None, "no cached copy"
            # The stored lines say no-referrer; the 304 says same-origin and
            # must win, while the other stored lines are still replayed.
            serve_control(origin_client, rp="same-origin", vary304=vary304)
            time.sleep(2.5)
            status_line, header_lines, body = raw_get(path)
            assert status_line == "HTTP/1.1 200 OK", status_line
            assert raw_header(header_lines, "X-PageSpeed") == ["REVALIDATED"]
            assert raw_header(header_lines, "Vary")
            assert body == hit.content
            assert int(raw_header(header_lines, "Content-Length")[0]) == len(body)
            assert raw_header(header_lines, "Referrer-Policy") == ["same-origin"]
            # The entry was dropped and not written again: with the origin
            # back to a storable 304, the next request is a miss, not a hit.
            serve_control(origin_client, vary304="")
            again = client.get(path, headers=IDENTITY)
            assert again.status_code == 200
            assert x_pagespeed(again) == "MISS", again.headers
        finally:
            serve_control(origin_client, vary304="")

    def test_matching_conditional_client_still_gets_304(
        self, client, origin_client
    ):
        path = self.PATH
        try:
            serve_control(origin_client, rp="no-referrer", vary304="*")
            client.get(path, headers=IDENTITY)  # records the entry
            time.sleep(2.5)
            # The 200 served from the unstorable revalidation: the validator
            # a client would hold.
            status_line, header_lines, _ = raw_get(path)
            assert status_line == "HTTP/1.1 200 OK", status_line
            etag = raw_header(header_lines, "ETag")
            assert etag, header_lines
            client.get(path, headers=IDENTITY)  # records the entry again
            time.sleep(2.5)
            status_line, header_lines, body = raw_get(
                path, {"If-None-Match": etag[0]}
            )
            assert status_line == "HTTP/1.1 304 Not Modified", status_line
            assert raw_header(header_lines, "ETag") == [etag[0]]
            assert raw_header(header_lines, "X-PageSpeed") == ["REVALIDATED"]
            assert body == b""
        finally:
            serve_control(origin_client, vary304="")


def requests_get(client, path: str):
    """A GET on its own connection (the client's session is not shared
    across threads)."""
    import requests

    return requests.get(client.base_url + path, headers=IDENTITY, timeout=15)
