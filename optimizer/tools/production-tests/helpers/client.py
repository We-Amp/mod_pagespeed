# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Production test HTTP client with polling and assertion helpers."""

import hashlib
import time

import pytest
import requests


class ProductionTestClient:
    """HTTP client for testing the PageSpeed proxy."""

    def __init__(self, base_url, origin_url=None):
        from requests.adapters import HTTPAdapter
        from urllib3.util.retry import Retry

        self.base_url = base_url
        self.origin_url = origin_url
        self.session = requests.Session()

        # Mount a retry adapter to handle stale keepalive connections.
        # Nginx closes idle connections (keepalive_timeout 65s), and the
        # client may try to reuse a just-closed socket, causing
        # RemoteDisconnected.  urllib3's Retry handles this transparently
        # by re-opening the connection on connection-level errors only
        # (never on HTTP error status codes).
        retry = Retry(
            total=3,
            connect=3,
            read=3,
            status=0,
            backoff_factor=0.1,
            raise_on_status=False,
        )
        adapter = HTTPAdapter(
            max_retries=retry,
            pool_connections=20,
            pool_maxsize=20,
        )
        self.session.mount("http://", adapter)
        self.session.mount("https://", adapter)

    def get(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.get(f"{self.base_url}{path}", **kwargs)

    def head(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.head(f"{self.base_url}{path}", **kwargs)

    def post(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.post(f"{self.base_url}{path}", **kwargs)

    def options(self, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.options(f"{self.base_url}{path}", **kwargs)

    def request(self, method, path, **kwargs):
        kwargs.setdefault("timeout", 30)
        return self.session.request(method, f"{self.base_url}{path}", **kwargs)

    def poll_for_hit(self, path, max_attempts=60, interval=1.0, **kwargs):
        """Poll until X-PageSpeed: HIT is received."""
        last = None
        for _ in range(max_attempts):
            last = self.get(path, **kwargs)
            if last.headers.get("X-PageSpeed") == "HIT":
                return last
            time.sleep(interval)
        pytest.fail(
            f"Never got HIT for {path} after {max_attempts} attempts. "
            f"Last status: {last.status_code if last else 'N/A'}, "
            f"Last X-PageSpeed: {last.headers.get('X-PageSpeed') if last else 'N/A'}"
        )

    def poll_for_header(
        self, path, header, value, max_attempts=60, interval=1.0, **kwargs
    ):
        """Poll until a HIT with the expected header value is received.

        Unlike poll_for_hit, this waits for a specific variant (e.g.,
        Content-Encoding: gzip) rather than any HIT.
        """
        last = None
        for _ in range(max_attempts):
            last = self.get(path, **kwargs)
            if (
                last.headers.get("X-PageSpeed") == "HIT"
                and last.headers.get(header) == value
            ):
                return last
            time.sleep(interval)
        pytest.fail(
            f"Never got HIT with {header}={value} for {path} after "
            f"{max_attempts} attempts. "
            f"Last X-PageSpeed: {last.headers.get('X-PageSpeed') if last else 'N/A'}, "
            f"Last {header}: {last.headers.get(header) if last else 'N/A'}"
        )

    def assert_miss(self, response):
        """Assert response is a cache MISS (or passthrough with no X-PageSpeed)."""
        xps = response.headers.get("X-PageSpeed")
        assert xps in ("MISS", None), (
            f"Expected MISS or passthrough, got X-PageSpeed: {xps} "
            f"(status={response.status_code})"
        )

    def assert_hit(self, response):
        """Assert response is a cache HIT."""
        xps = response.headers.get("X-PageSpeed")
        assert xps == "HIT", (
            f"Expected HIT, got X-PageSpeed: {xps} (status={response.status_code})"
        )

    def assert_not_cached(self, path, attempts=3, interval=2):
        """Verify path stays MISS across multiple requests."""
        for i in range(attempts):
            r = self.get(path)
            self.assert_miss(r)
            if i < attempts - 1:
                time.sleep(interval)

    def assert_content_length_matches(self, response):
        """Assert Content-Length matches actual body size."""
        cl = response.headers.get("Content-Length")
        if cl is not None:
            assert int(cl) == len(response.content), (
                f"Content-Length {cl} != actual {len(response.content)}"
            )

    def verify_body_hash(self, path, expected_hash, **kwargs):
        """GET path and verify SHA-256 hash of body."""
        r = self.get(path, **kwargs)
        actual = hashlib.sha256(r.content).hexdigest()
        assert actual == expected_hash, (
            f"Body hash mismatch for {path}: "
            f"expected {expected_hash[:16]}..., got {actual[:16]}..."
        )
        return r

    def get_origin_counter(self, name):
        """Get the current origin request counter value."""
        assert self.origin_url, "origin_url not configured"
        r = requests.get(f"{self.origin_url}/counter/{name}", timeout=5)
        return r.json()["count"]

    def reset_origin_counters(self):
        """Reset all origin request counters."""
        assert self.origin_url, "origin_url not configured"
        requests.post(f"{self.origin_url}/reset-counters", timeout=5)
