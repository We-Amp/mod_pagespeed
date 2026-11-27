# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""HTTP method handling tests.

Verifies that pagespeed only processes GET and HEAD requests.
POST, PUT, DELETE, and OPTIONS must be passed through to the origin
without pagespeed interception.

The nginx module checks: if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD)))
  return NGX_DECLINED;
"""


class TestGetAndHead:
    """GET and HEAD processed by pagespeed."""

    def test_get_processed(self, client):
        """GET request processed by pagespeed module."""
        r = client.get("/etag-test.css")
        assert r.status_code == 200
        # Module should add X-PageSpeed on success
        xs = r.headers.get("X-PageSpeed")
        assert xs in ("MISS", "HIT"), f"Expected MISS/HIT, got: {xs}"

    def test_head_processed(self, client):
        """HEAD request processed by pagespeed module."""
        r = client.head("/etag-test.css")
        assert r.status_code == 200
        assert len(r.content) == 0


class TestMethodBypass:
    """Non-GET/HEAD methods bypass pagespeed."""

    def test_post_not_intercepted(self, client):
        """POST request not intercepted by pagespeed."""
        r = client.post("/echo", data=b"test body")
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"POST should not have X-PageSpeed: {xs}"

    def test_put_not_intercepted(self, client):
        """PUT request not intercepted by pagespeed."""
        r = client.put("/echo", data=b"put body")
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"PUT should not have X-PageSpeed: {xs}"

    def test_delete_not_intercepted(self, client):
        """DELETE request not intercepted by pagespeed."""
        r = client.delete("/echo")
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"DELETE should not have X-PageSpeed: {xs}"

    def test_options_not_intercepted(self, client):
        """OPTIONS request not intercepted by pagespeed."""
        r = client.options("/echo")
        assert r.status_code == 200
        xs = r.headers.get("X-PageSpeed")
        assert xs is None, f"OPTIONS should not have X-PageSpeed: {xs}"


class TestMethodPreservation:
    """Methods reach the origin correctly."""

    def test_post_reaches_origin(self, client):
        """POST body reaches origin through proxy."""
        r = client.post("/echo", data=b"hello compliance")
        assert r.status_code == 200
        body = r.text
        assert "POST" in body, f"Expected POST in echo: {body}"

    def test_put_reaches_origin(self, client):
        """PUT body reaches origin through proxy."""
        r = client.put("/echo", data=b"update data")
        assert r.status_code == 200
        body = r.text
        assert "PUT" in body

    def test_delete_reaches_origin(self, client):
        """DELETE reaches origin through proxy."""
        r = client.delete("/echo")
        assert r.status_code == 200
        body = r.text
        assert "DELETE" in body
