#!/usr/bin/env python3
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Unit tests for the pagespeed_test_framework.

These tests verify the framework itself works correctly, independent of
any PageSpeed server. They can be run without any server setup.

Run with: python -m pytest -v test_framework.py
"""

import pytest

from pagespeed_test_framework.client import Response
from pagespeed_test_framework.assertions import (
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_equals,
    assert_header_contains,
    assert_stat_delta,
    assert_stat_increased,
    assert_stat_unchanged,
    assert_file_size,
)
from pagespeed_test_framework.stats import (
    parse_statistics,
    get_stat,
    scrape_header,
    extract_headers,
    scrape_content_length,
    extract_beacon_params,
    count_pattern_matches,
)


class TestResponse:
    """Tests for the Response class."""

    def test_response_text(self):
        """Response.text decodes body as UTF-8."""
        response = Response(
            status=200,
            headers={"Content-Type": "text/html"},
            body=b"Hello World",
        )
        assert response.text == "Hello World"

    def test_response_header_case_insensitive(self):
        """Headers are looked up case-insensitively."""
        response = Response(
            status=200,
            headers={"Content-Type": "text/html", "X-Custom-Header": "value"},
            body=b"",
        )
        assert response.header("content-type") == "text/html"
        assert response.header("CONTENT-TYPE") == "text/html"
        assert response.header("Content-Type") == "text/html"
        assert response.header("x-custom-header") == "value"

    def test_response_header_default(self):
        """header() returns default when header not found."""
        response = Response(status=200, headers={}, body=b"")
        assert response.header("Missing") == ""
        assert response.header("Missing", "default") == "default"

    def test_response_content_length(self):
        """content_length property parses Content-Length header."""
        response = Response(
            status=200,
            headers={"Content-Length": "1234"},
            body=b"x" * 1234,
        )
        assert response.content_length == 1234

    def test_response_is_ok(self):
        """is_ok() returns True for 2xx status codes."""
        assert Response(status=200, headers={}, body=b"").is_ok()
        assert Response(status=204, headers={}, body=b"").is_ok()
        assert not Response(status=404, headers={}, body=b"").is_ok()
        assert not Response(status=500, headers={}, body=b"").is_ok()


class TestAssertContains:
    """Tests for assert_contains and assert_not_contains."""

    def test_assert_contains_string(self):
        """assert_contains finds pattern in string."""
        assert_contains("hello world", "world")
        assert_contains("hello world", r"hel+o")

    def test_assert_contains_bytes(self):
        """assert_contains works with bytes."""
        assert_contains(b"hello world", "world")

    def test_assert_contains_response(self):
        """assert_contains works with Response."""
        response = Response(status=200, headers={}, body=b"hello world")
        assert_contains(response, "world")

    def test_assert_contains_fails(self):
        """assert_contains raises AssertionError when not found."""
        with pytest.raises(AssertionError, match="not found"):
            assert_contains("hello", "world")

    def test_assert_not_contains(self):
        """assert_not_contains passes when pattern absent."""
        assert_not_contains("hello", "world")

    def test_assert_not_contains_fails(self):
        """assert_not_contains raises when pattern found."""
        with pytest.raises(AssertionError, match="unexpectedly found"):
            assert_not_contains("hello world", "world")


class TestAssertHttpStatus:
    """Tests for assert_http_status."""

    def test_assert_http_status_ok(self):
        """assert_http_status passes for matching status."""
        response = Response(status=200, headers={}, body=b"")
        assert_http_status(response, 200)

    def test_assert_http_status_default(self):
        """assert_http_status defaults to expecting 200."""
        response = Response(status=200, headers={}, body=b"")
        assert_http_status(response)

    def test_assert_http_status_fails(self):
        """assert_http_status raises for wrong status."""
        response = Response(status=404, headers={}, body=b"", url="/test")
        with pytest.raises(AssertionError, match="Expected HTTP 200, got 404"):
            assert_http_status(response, 200)


class TestAssertHeaders:
    """Tests for header assertions."""

    def test_assert_header_equals(self):
        """assert_header_equals passes for exact match."""
        response = Response(
            status=200,
            headers={"Content-Type": "text/html"},
            body=b"",
        )
        assert_header_equals(response, "Content-Type", "text/html")

    def test_assert_header_equals_fails(self):
        """assert_header_equals fails for mismatch."""
        response = Response(
            status=200,
            headers={"Content-Type": "text/html"},
            body=b"",
        )
        with pytest.raises(AssertionError, match="expected 'text/plain'"):
            assert_header_equals(response, "Content-Type", "text/plain")

    def test_assert_header_contains(self):
        """assert_header_contains finds pattern in header."""
        response = Response(
            status=200,
            headers={"Cache-Control": "max-age=31536000, public"},
            body=b"",
        )
        assert_header_contains(response, "Cache-Control", "max-age")
        assert_header_contains(response, "Cache-Control", r"max-age=\d+")


class TestAssertStats:
    """Tests for statistics assertions."""

    def test_assert_stat_delta(self):
        """assert_stat_delta checks exact change."""
        old = {"counter": 10}
        new = {"counter": 15}
        assert_stat_delta(old, new, "counter", 5)

    def test_assert_stat_delta_fails(self):
        """assert_stat_delta fails for wrong delta."""
        old = {"counter": 10}
        new = {"counter": 15}
        with pytest.raises(AssertionError, match="expected 3, got 5"):
            assert_stat_delta(old, new, "counter", 3)

    def test_assert_stat_increased(self):
        """assert_stat_increased checks minimum increase."""
        old = {"counter": 10}
        new = {"counter": 15}
        assert_stat_increased(old, new, "counter", min_increase=1)
        assert_stat_increased(old, new, "counter", min_increase=5)

    def test_assert_stat_increased_fails(self):
        """assert_stat_increased fails when increase too small."""
        old = {"counter": 10}
        new = {"counter": 12}
        with pytest.raises(AssertionError, match="expected increase >= 5, got 2"):
            assert_stat_increased(old, new, "counter", min_increase=5)

    def test_assert_stat_unchanged(self):
        """assert_stat_unchanged passes when stat unchanged."""
        stats = {"counter": 10}
        assert_stat_unchanged(stats, stats, "counter")

    def test_assert_stat_missing_defaults_to_zero(self):
        """Missing stats default to 0."""
        old = {}
        new = {"counter": 5}
        assert_stat_delta(old, new, "counter", 5)


class TestAssertFileSize:
    """Tests for file size assertions."""

    def test_assert_file_size_operators(self):
        """assert_file_size works with various operators."""
        content = b"x" * 100
        assert_file_size(content, "-lt", 200)
        assert_file_size(content, "-le", 100)
        assert_file_size(content, "-eq", 100)
        assert_file_size(content, "-ge", 100)
        assert_file_size(content, "-gt", 50)
        # Also test symbol operators
        assert_file_size(content, "<", 200)
        assert_file_size(content, "<=", 100)
        assert_file_size(content, "==", 100)
        assert_file_size(content, ">=", 100)
        assert_file_size(content, ">", 50)

    def test_assert_file_size_response(self):
        """assert_file_size works with Response."""
        response = Response(status=200, headers={}, body=b"x" * 100)
        assert_file_size(response, "-eq", 100)

    def test_assert_file_size_fails(self):
        """assert_file_size fails when constraint not met."""
        with pytest.raises(AssertionError, match="not < 50"):
            assert_file_size(b"x" * 100, "<", 50)


class TestParseStatistics:
    """Tests for statistics parsing."""

    def test_parse_statistics_colon_format(self):
        """parse_statistics handles 'name: value' format."""
        content = """
cache_hits: 100
cache_misses: 50
image_rewrites: 25
"""
        stats = parse_statistics(content)
        assert stats["cache_hits"] == 100
        assert stats["cache_misses"] == 50
        assert stats["image_rewrites"] == 25

    def test_parse_statistics_space_format(self):
        """parse_statistics handles 'name value' format."""
        content = """
cache_hits 100
cache_misses 50
"""
        stats = parse_statistics(content)
        assert stats["cache_hits"] == 100
        assert stats["cache_misses"] == 50

    def test_parse_statistics_ignores_non_numeric(self):
        """parse_statistics ignores non-numeric lines."""
        content = """
cache_hits: 100
some_text: not_a_number
cache_misses: 50
"""
        stats = parse_statistics(content)
        assert "cache_hits" in stats
        assert "cache_misses" in stats
        assert "some_text" not in stats

    def test_get_stat(self):
        """get_stat retrieves value or default."""
        stats = {"counter": 42}
        assert get_stat(stats, "counter") == 42
        assert get_stat(stats, "missing") == 0
        assert get_stat(stats, "missing", 99) == 99


class TestScrapeHeader:
    """Tests for header scraping."""

    def test_scrape_header(self):
        """scrape_header extracts header value."""
        headers = "Content-Type: text/html\r\nContent-Length: 1234\r\n"
        assert scrape_header(headers, "Content-Type") == "text/html"
        assert scrape_header(headers, "Content-Length") == "1234"

    def test_scrape_header_case_insensitive(self):
        """scrape_header is case-insensitive."""
        headers = "Content-Type: text/html\r\n"
        assert scrape_header(headers, "content-type") == "text/html"
        assert scrape_header(headers, "CONTENT-TYPE") == "text/html"

    def test_scrape_header_not_found(self):
        """scrape_header returns empty string when not found."""
        headers = "Content-Type: text/html\r\n"
        assert scrape_header(headers, "Missing") == ""


class TestExtractHeaders:
    """Tests for header extraction."""

    def test_extract_headers_crlf(self):
        """extract_headers handles CRLF line endings."""
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>"
        headers = extract_headers(response)
        assert "Content-Type" in headers
        assert "<html>" not in headers

    def test_extract_headers_lf(self):
        """extract_headers handles LF line endings."""
        response = "HTTP/1.1 200 OK\nContent-Type: text/html\n\n<html>"
        headers = extract_headers(response)
        assert "Content-Type" in headers
        assert "<html>" not in headers


class TestScrapeContentLength:
    """Tests for Content-Length scraping."""

    def test_scrape_content_length(self):
        """scrape_content_length extracts integer value."""
        headers = "Content-Type: text/html\r\nContent-Length: 1234\r\n"
        assert scrape_content_length(headers) == 1234

    def test_scrape_content_length_not_found(self):
        """scrape_content_length returns None when not found."""
        headers = "Content-Type: text/html\r\n"
        assert scrape_content_length(headers) is None


class TestExtractBeaconParams:
    """Tests for beacon parameter extraction."""

    def test_extract_beacon_params(self):
        """extract_beacon_params parses beacon init call."""
        html = """
<script>
pagespeed.criticalCssBeaconInit('/beacon', 'http%3A%2F%2Fexample.com', 'abc123', 'nonce456');
</script>
"""
        params = extract_beacon_params(html)
        assert params is not None
        assert params["path"] == "/beacon"
        assert params["url"] == "http%3A%2F%2Fexample.com"
        assert params["hash"] == "abc123"
        assert params["nonce"] == "nonce456"

    def test_extract_beacon_params_with_selector_list(self):
        """extract_beacon_params handles the live 5-arg snippet.

        The server-injected call passes the candidate selector array as a
        fifth argument after the nonce.
        """
        html = ("pagespeed.criticalCssBeaconInit('/mod_pagespeed_beacon',"
                "'http://example.com/a.html?q=1','oh1','n2',"
                "['.big','.blue']);")
        params = extract_beacon_params(html)
        assert params is not None
        assert params["path"] == "/mod_pagespeed_beacon"
        assert params["url"] == "http://example.com/a.html?q=1"
        assert params["hash"] == "oh1"
        assert params["nonce"] == "n2"

    def test_extract_beacon_params_not_found(self):
        """extract_beacon_params returns None when no beacon."""
        html = "<html><body>No beacon here</body></html>"
        assert extract_beacon_params(html) is None


class TestCountPatternMatches:
    """Tests for pattern counting."""

    def test_count_pattern_matches(self):
        """count_pattern_matches counts regex matches."""
        content = "foo bar foo baz foo"
        assert count_pattern_matches(content, "foo") == 3
        assert count_pattern_matches(content, "bar") == 1
        assert count_pattern_matches(content, "qux") == 0

    def test_count_pattern_matches_regex(self):
        """count_pattern_matches works with regex patterns."""
        content = "cat bat rat mat"
        assert count_pattern_matches(content, r"\wat") == 4


class TestTimeoutMultiplier:
    """Tests for PAGESPEED_TEST_TIMEOUT_MULTIPLIER env var parsing."""

    def test_default_when_unset(self, monkeypatch):
        monkeypatch.delenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", raising=False)
        from pagespeed_test_framework.client import _read_timeout_multiplier
        assert _read_timeout_multiplier() == 1.0

    def test_parses_valid_float(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "3")
        from pagespeed_test_framework.client import _read_timeout_multiplier
        assert _read_timeout_multiplier() == 3.0

    def test_parses_fractional(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "2.5")
        from pagespeed_test_framework.client import _read_timeout_multiplier
        assert _read_timeout_multiplier() == 2.5

    def test_falls_back_on_garbage(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "not-a-number")
        from pagespeed_test_framework.client import _read_timeout_multiplier
        assert _read_timeout_multiplier() == 1.0

    def test_falls_back_on_zero(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "0")
        from pagespeed_test_framework.client import _read_timeout_multiplier
        assert _read_timeout_multiplier() == 1.0

    def test_falls_back_on_negative(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "-5")
        from pagespeed_test_framework.client import _read_timeout_multiplier
        assert _read_timeout_multiplier() == 1.0


class TestFetchUntilRetries:
    """Tests for PAGESPEED_TEST_FETCH_RETRIES env var parsing."""

    def test_default_when_unset(self, monkeypatch):
        monkeypatch.delenv("PAGESPEED_TEST_FETCH_RETRIES", raising=False)
        from pagespeed_test_framework.client import _read_fetch_until_retries
        assert _read_fetch_until_retries() == 0

    def test_parses_valid_int(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_FETCH_RETRIES", "2")
        from pagespeed_test_framework.client import _read_fetch_until_retries
        assert _read_fetch_until_retries() == 2

    def test_falls_back_on_garbage(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_FETCH_RETRIES", "not-a-number")
        from pagespeed_test_framework.client import _read_fetch_until_retries
        assert _read_fetch_until_retries() == 0

    def test_falls_back_on_negative(self, monkeypatch):
        monkeypatch.setenv("PAGESPEED_TEST_FETCH_RETRIES", "-3")
        from pagespeed_test_framework.client import _read_fetch_until_retries
        assert _read_fetch_until_retries() == 0


class TestFetchUntilRetryBehavior:
    """Behavioral tests for the bounded fetch_until retry-on-timeout.

    These prove the retry buys budget WITHOUT masking real failures: a
    transient timeout that later converges succeeds, but a condition that
    never converges still raises TimeoutError after the retries are spent.
    """

    def _client(self):
        from pagespeed_test_framework.client import PageSpeedClient
        return PageSpeedClient("localhost", 80)

    def test_retry_recovers_after_transient_timeout(self, monkeypatch):
        # 1 retry => 2 budgeted attempts total. First attempt "times out",
        # second succeeds. Net effect: the call returns instead of raising.
        from pagespeed_test_framework import client as client_mod
        from pagespeed_test_framework.client import Response
        monkeypatch.setattr(client_mod, "_FETCH_UNTIL_RETRIES", 1)

        calls = {"n": 0}
        ok = Response(status=200, headers={}, body=b"done")

        def fake_once(self, path, condition, timeout=100.0, interval=0.5,
                      headers=None, use_gzip=False):
            calls["n"] += 1
            if calls["n"] == 1:
                raise TimeoutError("transient")
            return ok

        monkeypatch.setattr(client_mod.PageSpeedClient, "_fetch_until_once", fake_once)
        result = self._client().fetch_until("/x", lambda r: True)
        assert result is ok
        assert calls["n"] == 2  # used the retry

    def test_never_converging_still_raises(self, monkeypatch):
        # With retries enabled, a condition that NEVER holds must still raise
        # TimeoutError once the budgeted attempts are exhausted -- the retry
        # must not silently swallow a genuine failure.
        from pagespeed_test_framework import client as client_mod
        monkeypatch.setattr(client_mod, "_FETCH_UNTIL_RETRIES", 2)

        calls = {"n": 0}

        def fake_once(self, path, condition, timeout=100.0, interval=0.5,
                      headers=None, use_gzip=False):
            calls["n"] += 1
            raise TimeoutError("never converges")

        monkeypatch.setattr(client_mod.PageSpeedClient, "_fetch_until_once", fake_once)
        with pytest.raises(TimeoutError):
            self._client().fetch_until("/x", lambda r: False)
        assert calls["n"] == 3  # 1 initial + 2 retries, then propagates

    def test_no_retry_by_default(self, monkeypatch):
        # Default (0 retries) => exactly one budgeted attempt, raises on timeout.
        from pagespeed_test_framework import client as client_mod
        monkeypatch.setattr(client_mod, "_FETCH_UNTIL_RETRIES", 0)

        calls = {"n": 0}

        def fake_once(self, path, condition, timeout=100.0, interval=0.5,
                      headers=None, use_gzip=False):
            calls["n"] += 1
            raise TimeoutError("boom")

        monkeypatch.setattr(client_mod.PageSpeedClient, "_fetch_until_once", fake_once)
        with pytest.raises(TimeoutError):
            self._client().fetch_until("/x", lambda r: False)
        assert calls["n"] == 1  # no extra attempts


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
