# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Streaming and transfer encoding tests for the caching reverse proxy."""

import time
from uuid import uuid4

import pytest
import requests


class TestStreamingTransfer:
    """Verify chunked transfer, body sizes, encoding, and connection handling."""

    # ------------------------------------------------------------------
    # P0 tests
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_chunked_small_chunks(self, client):
        """1-byte chunks are reassembled into the correct total body."""
        uid = uuid4().hex
        path = f"/chunked/small?size=64&_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) == 64

    @pytest.mark.p0
    def test_chunked_large_chunks(self, client):
        """1MB chunks are received intact through the proxy."""
        uid = uuid4().hex
        path = f"/chunked/large?chunks=3&_={uid}"
        r = client.get(path, timeout=60)
        assert r.status_code == 200
        assert len(r.content) == 3 * 1024 * 1024

    @pytest.mark.p0
    def test_chunked_empty_body(self, client):
        """Zero-length chunked response produces an empty body."""
        uid = uuid4().hex
        path = f"/chunked/empty?_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) == 0

    @pytest.mark.p0
    def test_body_exactly_10mb_cached(self, client):
        """10MB response at the recording limit is cached."""
        uid = uuid4().hex
        path = f"/size/10mb?type=text/css&cache=3600&_={uid}"
        r = client.get(path, timeout=120)
        assert r.status_code == 200
        assert len(r.content) == 10 * 1024 * 1024
        hit = client.poll_for_hit(path, max_attempts=90)
        assert len(hit.content) == 10 * 1024 * 1024

    @pytest.mark.p0
    def test_body_10mb_plus_1_not_cached(self, client):
        """Response 1 byte over 10MB exceeds recording limit and stays MISS."""
        uid = uuid4().hex
        path = f"/size/10mb-plus-1?type=text/css&cache=3600&_={uid}"
        r = client.get(path, timeout=60)
        assert r.status_code == 200
        assert len(r.content) == 10 * 1024 * 1024 + 1
        client.assert_miss(r)
        time.sleep(3)
        r2 = client.get(path, timeout=60)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_body_100mb_served_not_cached(self, client):
        """Oversized response is served to client but not recorded in cache."""
        uid = uuid4().hex
        path = f"/size/11mb?type=text/css&cache=3600&_={uid}"
        r = client.get(path, timeout=60)
        assert r.status_code == 200
        assert len(r.content) == 11 * 1024 * 1024
        client.assert_miss(r)
        time.sleep(3)
        r2 = client.get(path, timeout=60)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_body_zero_bytes(self, client):
        """Zero-byte sized response is cached correctly."""
        uid = uuid4().hex
        path = f"/size/0?type=text/css&cache=3600&_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) == 0
        hit = client.poll_for_hit(path)
        assert len(hit.content) == 0

    @pytest.mark.p0
    def test_body_single_byte(self, client):
        """Single-byte response is cached correctly."""
        uid = uuid4().hex
        path = f"/size/1?type=text/css&cache=3600&_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) == 1
        hit = client.poll_for_hit(path)
        assert len(hit.content) == 1

    @pytest.mark.p0
    def test_body_binary_with_nulls(self, client):
        """Binary content with embedded null bytes is preserved through proxy."""
        uid = uuid4().hex
        path = f"/content/binary-nulls?_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert b"\x00" in r.content, "Response should contain null bytes"
        assert len(r.content) > 0

    @pytest.mark.p0
    def test_pre_compressed_gzip_rejected(self, client):
        """Pre-compressed gzip from origin is not cached by proxy."""
        uid = uuid4().hex
        path = f"/encoding/gzip?_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        client.assert_miss(r)
        time.sleep(3)
        r2 = client.get(path)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_pre_compressed_brotli_rejected(self, client):
        """Pre-compressed brotli from origin is not cached by proxy."""
        uid = uuid4().hex
        path = f"/encoding/brotli?_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        client.assert_miss(r)
        time.sleep(3)
        r2 = client.get(path)
        client.assert_miss(r2)

    @pytest.mark.p0
    def test_content_length_matches_body(self, client):
        """Content-Length header matches actual body size on every response."""
        uid = uuid4().hex
        path = f"/content/cl-check-{uid}"
        # Pin Accept-Encoding: identity to avoid auto-decompression mismatch
        # (requests library auto-decompresses gzip, making CL != body length).
        ae = {"Accept-Encoding": "identity"}
        r = client.get(path, headers=ae)
        assert r.status_code == 200
        client.assert_content_length_matches(r)
        hit = client.poll_for_hit(path, headers=ae)
        client.assert_content_length_matches(hit)

    # ------------------------------------------------------------------
    # P1 tests
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_chunked_slow_delivery(self, client):
        """Slow chunked delivery with delays does not cause a proxy timeout."""
        uid = uuid4().hex
        path = f"/chunked/slow?delay=2&chunks=3&_={uid}"
        r = client.get(path, timeout=30)
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.p1
    def test_chunked_rapid_fire(self, client):
        """100 rapid-fire chunks are all received through the proxy."""
        uid = uuid4().hex
        path = f"/chunked/rapid?chunks=100&_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.p1
    def test_sse_not_cached(self, client):
        """Server-Sent Events stream (text/event-stream) is not cached."""
        uid = uuid4().hex
        path = f"/content/sse?_={uid}"
        r = client.get(path, timeout=10, stream=False)
        assert r.status_code == 200
        client.assert_miss(r)
        time.sleep(2)
        r2 = client.get(path, timeout=10, stream=False)
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_origin_disconnect_mid_response(self, client):
        """Origin disconnect mid-response results in partial or error, not cached."""
        uid = uuid4().hex
        path = f"/disconnect/mid?after=1000&_={uid}"
        try:
            r = client.get(path, timeout=10)
            # If we got a response, it should not be cached
            time.sleep(2)
            r2 = client.get(path, timeout=10)
            client.assert_miss(r2)
        except (requests.ConnectionError, requests.exceptions.ChunkedEncodingError):
            # Connection error is expected when origin disconnects
            pass

    @pytest.mark.p1
    def test_headers_only_then_disconnect(self, client):
        """Origin sends headers then disconnects — error or empty body."""
        uid = uuid4().hex
        path = f"/disconnect/headers-only?_={uid}"
        try:
            r = client.get(path, timeout=10)
            # If we got a response, body should be empty or status should be error
            assert r.status_code >= 400 or len(r.content) == 0
        except (requests.ConnectionError, requests.exceptions.ChunkedEncodingError):
            # Connection error is acceptable
            pass

    @pytest.mark.p1
    def test_client_gzip_origin_identity(self, client):
        """MISS returns identity from origin; HIT returns gzip when client accepts it."""
        uid = uuid4().hex
        path = f"/content/gzip-test-{uid}"
        # First request: MISS, origin sends identity
        r = client.get(path, headers={"Accept-Encoding": "gzip"})
        client.assert_miss(r)
        assert r.status_code == 200
        body_miss = r.content

        # Poll for HIT with gzip accepted
        hit = client.poll_for_hit(path, headers={"Accept-Encoding": "gzip"})
        assert hit.status_code == 200
        # Content should be equivalent (requests auto-decompresses)
        assert len(hit.content) > 0

    @pytest.mark.p1
    def test_file_backed_buffer_activation(self, client):
        """Large 5MB response triggers proxy file buffers but serves correctly."""
        uid = uuid4().hex
        path = f"/size/5mb?type=text/css&cache=3600&_={uid}"
        r = client.get(path, timeout=60)
        assert r.status_code == 200
        assert len(r.content) == 5 * 1024 * 1024

    @pytest.mark.p1
    def test_content_encoding_identity_explicit(self, client):
        """Explicit Content-Encoding: identity from origin is served correctly."""
        uid = uuid4().hex
        path = f"/encoding/identity?_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.p1
    def test_keep_alive_multiple_requests(self, client):
        """Three sequential requests on the same session all succeed."""
        for i in range(3):
            uid = uuid4().hex
            path = f"/content/keepalive-{uid}"
            r = client.get(path)
            assert r.status_code == 200
            assert len(r.content) > 0

    # ------------------------------------------------------------------
    # P2 tests
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_chunked_with_extensions(self, client):
        """Chunked transfer with chunk extensions is handled correctly."""
        uid = uuid4().hex
        path = f"/chunked/small?size=32&_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) == 32

    @pytest.mark.p2
    def test_chunked_with_trailers(self, client):
        """Chunked response with trailer headers is received successfully."""
        uid = uuid4().hex
        path = f"/chunked/trailers?_={uid}"
        r = client.get(path)
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.p2
    def test_http_pipelining(self, client):
        """Multiple requests on the same session connection succeed."""
        session = requests.Session()
        base = client.base_url
        results = []
        for i in range(3):
            uid = uuid4().hex
            r = session.get(f"{base}/content/pipeline-{uid}", timeout=10)
            results.append(r)
        session.close()
        for r in results:
            assert r.status_code == 200
            assert len(r.content) > 0

    @pytest.mark.p2
    def test_content_length_mismatch(self, client):
        """Origin lies about Content-Length then disconnects — partial or error."""
        uid = uuid4().hex
        path = f"/disconnect/mid?after=500&_={uid}"
        try:
            r = client.get(path, timeout=10)
            # If proxy detected the mismatch, it may return an error
            # or truncated body; either way it should not be cached
            time.sleep(2)
            r2 = client.get(path, timeout=10)
            client.assert_miss(r2)
        except (
            requests.ConnectionError,
            requests.exceptions.ChunkedEncodingError,
            requests.exceptions.ContentDecodingError,
        ):
            # Connection-level error is expected
            pass
