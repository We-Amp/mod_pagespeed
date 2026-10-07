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

import itertools
import re
import socketserver
import sys
import threading

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
    count_matching_lines,
    settled_stats,
)
from pagespeed_test_framework.require import (
    require_match,
    require_status_ok,
    require_no_auth_gate,
)
from pagespeed_test_framework.pytest_main import run_pytest
from pagespeed_test_framework.client import VhostClient
from pagespeed_test_framework import flush_origin
from pagespeed_test_framework.streaming import fetch_chunks


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
                      headers=None, use_gzip=False, detail_fn=None):
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
                      headers=None, use_gzip=False, detail_fn=None):
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
                      headers=None, use_gzip=False, detail_fn=None):
            calls["n"] += 1
            raise TimeoutError("boom")

        monkeypatch.setattr(client_mod.PageSpeedClient, "_fetch_until_once", fake_once)
        with pytest.raises(TimeoutError):
            self._client().fetch_until("/x", lambda r: False)
        assert calls["n"] == 1  # no extra attempts


class TestFetchUntilTimeoutDiagnostics:
    """Failure-window diagnostics for fetch_until timeouts.

    A fetch_until timeout on the AppVerifier lane is currently undiagnosable:
    the server's message buffer rotates past the failure window before the
    suite-end evidence capture runs, and the TimeoutError discards how close
    the condition was (0-of-2 vs 1-of-2 matches). These tests pin the
    at-the-moment-of-failure capture: match-count detail in the error, and
    last-response-body + message_history snapshots written to
    PAGESPEED_EVIDENCE_DIR (the directory CI already uploads).
    """

    MESSAGES_JSON = b'{"messages":[{"severity":"info","message":"hello"}]}'

    def _patch_get(self, monkeypatch, body=b"one match here",
                   admin_error=False):
        """Serve a canned page for every path; message_history JSON for the
        admin path (or a connection error when admin_error is set)."""
        from pagespeed_test_framework import client as client_mod
        from pagespeed_test_framework.client import Response

        monkeypatch.setattr(client_mod, "_TIMEOUT_MULTIPLIER", 1.0)
        monkeypatch.setattr(client_mod, "_FETCH_UNTIL_RETRIES", 0)

        messages_json = self.MESSAGES_JSON

        def fake_get(self, path, headers=None, allow_redirects=False):
            if "message_history" in path:
                if admin_error:
                    raise OSError("admin endpoint unreachable")
                return Response(status=200, headers={}, body=messages_json,
                                url=path)
            return Response(status=200, headers={}, body=body, url=path)

        monkeypatch.setattr(client_mod.PageSpeedClient, "get", fake_get)
        return client_mod.PageSpeedClient("localhost", 80)

    def _fetch_count_timeout(self, client, expected_count=2):
        with pytest.raises(TimeoutError) as err:
            client.fetch_until_count(
                "/mod_pagespeed_example/inline_css.html?PageSpeedFilters=inline_css",
                pattern=r"match",
                expected_count=expected_count,
                timeout=0.05,
            )
        return err.value

    def test_count_timeout_reports_match_count(self, monkeypatch):
        """The error must state how close the condition was: 1-of-2 matches
        (rewrite partially converged) reads completely differently from
        0-of-2 (rewrite never started)."""
        client = self._patch_get(monkeypatch)
        err = self._fetch_count_timeout(client, expected_count=2)
        assert "matches=1" in str(err)
        assert "expected=2" in str(err)

    def test_contains_timeout_reports_pattern(self, monkeypatch):
        """fetch_until_contains names the pattern that never appeared."""
        client = self._patch_get(monkeypatch)
        with pytest.raises(TimeoutError) as err:
            client.fetch_until_contains("/page.html", "never-there",
                                        timeout=0.05)
        assert "never-there" in str(err.value)

    def test_timeout_saves_body_and_message_history(self, monkeypatch, tmp_path):
        """With PAGESPEED_EVIDENCE_DIR set, a timeout writes the last
        response body and a moment-of-failure message_history snapshot."""
        monkeypatch.setenv("PAGESPEED_EVIDENCE_DIR", str(tmp_path))
        monkeypatch.setenv("PAGESPEED_ADMIN_PATH", "/pagespeed_admin")
        client = self._patch_get(monkeypatch, body=b"<html>one match</html>")
        self._fetch_count_timeout(client)

        bodies = list(tmp_path.glob("fetch-until-timeout-*-body.html"))
        messages = list(tmp_path.glob("fetch-until-timeout-*-messages.json"))
        infos = list(tmp_path.glob("fetch-until-timeout-*-info.txt"))
        assert len(bodies) == 1, f"files: {[p.name for p in tmp_path.iterdir()]}"
        assert bodies[0].read_bytes() == b"<html>one match</html>"
        assert len(messages) == 1
        assert messages[0].read_bytes() == self.MESSAGES_JSON
        assert len(infos) == 1
        info = infos[0].read_text()
        assert "inline_css.html" in info
        assert "status=200" in info
        assert "matches=1" in info

    def test_timeout_message_names_evidence(self, monkeypatch, tmp_path):
        """The TimeoutError points triage at the evidence files."""
        monkeypatch.setenv("PAGESPEED_EVIDENCE_DIR", str(tmp_path))
        client = self._patch_get(monkeypatch)
        err = self._fetch_count_timeout(client)
        assert "fetch-until-timeout-" in str(err)

    def test_admin_snapshot_failure_does_not_mask_timeout(self, monkeypatch,
                                                          tmp_path):
        """If the message_history fetch itself fails, the body is still
        saved and the original TimeoutError still propagates."""
        monkeypatch.setenv("PAGESPEED_EVIDENCE_DIR", str(tmp_path))
        client = self._patch_get(monkeypatch, admin_error=True)
        err = self._fetch_count_timeout(client)
        assert "Condition not met" in str(err)
        assert len(list(tmp_path.glob("*-body.html"))) == 1
        assert list(tmp_path.glob("*-messages.json")) == []

    def test_no_evidence_dir_writes_nothing(self, monkeypatch, tmp_path):
        """Without PAGESPEED_EVIDENCE_DIR (Linux lanes, local runs) the
        timeout path stays exactly as before: no files, plain error."""
        monkeypatch.delenv("PAGESPEED_EVIDENCE_DIR", raising=False)
        monkeypatch.chdir(tmp_path)
        client = self._patch_get(monkeypatch)
        err = self._fetch_count_timeout(client)
        assert "Condition not met" in str(err)
        assert list(tmp_path.iterdir()) == []

    def test_repeated_timeouts_get_unique_files(self, monkeypatch, tmp_path):
        """Two timeouts in one run (e.g. the IIS retry budget) must not
        overwrite each other's evidence."""
        monkeypatch.setenv("PAGESPEED_EVIDENCE_DIR", str(tmp_path))
        client = self._patch_get(monkeypatch)
        self._fetch_count_timeout(client)
        self._fetch_count_timeout(client)
        assert len(list(tmp_path.glob("*-body.html"))) == 2

    def test_evidence_filenames_are_windows_safe(self, monkeypatch, tmp_path):
        """Query strings and slashes in the polled path must not produce
        invalid filenames (this all runs on Windows CI)."""
        monkeypatch.setenv("PAGESPEED_EVIDENCE_DIR", str(tmp_path))
        client = self._patch_get(monkeypatch)
        with pytest.raises(TimeoutError):
            client.fetch_until("/a/b.html?x=1&y=2*<>|", lambda r: False,
                               timeout=0.05)
        names = [p.name for p in tmp_path.iterdir()]
        assert names, "expected evidence files"
        for name in names:
            assert re.fullmatch(r"[A-Za-z0-9._-]+", name), name


class TestFetchUntilTimeoutBodyExcerpt:
    """Last-body excerpt in the fetch_until TimeoutError.

    A convergence-window miss that reports only status and pattern cannot
    be triaged after the fact: a 200 whose HTML simply never converged
    reads exactly like a 200 whose body carried nothing useful. The
    TimeoutError therefore carries a truncated excerpt of the last body
    the poll saw -- unsprited-but-served HTML vs an empty or non-text
    body are then distinguishable from the log line alone.
    """

    def _patch_get(self, monkeypatch, body=b"one match here"):
        """Serve a canned body for every path, with a 0.05s poll budget."""
        from pagespeed_test_framework import client as client_mod
        from pagespeed_test_framework.client import Response

        monkeypatch.setattr(client_mod, "_TIMEOUT_MULTIPLIER", 1.0)
        monkeypatch.setattr(client_mod, "_FETCH_UNTIL_RETRIES", 0)
        monkeypatch.delenv("PAGESPEED_EVIDENCE_DIR", raising=False)

        def fake_get(self, path, headers=None, allow_redirects=False):
            return Response(status=200, headers={}, body=body, url=path)

        monkeypatch.setattr(client_mod.PageSpeedClient, "get", fake_get)
        return client_mod.PageSpeedClient("localhost", 80)

    def _timeout_error(self, client):
        with pytest.raises(TimeoutError) as err:
            client.fetch_until("/mod_pagespeed_example/sprite_images.html",
                               lambda r: False, timeout=0.05)
        return str(err.value)

    def test_timeout_carries_body_excerpt(self, monkeypatch):
        """The error names what the last response actually served."""
        client = self._patch_get(monkeypatch, body=b"<html>not converged</html>")
        message = self._timeout_error(client)
        assert "body=" in message
        assert "not converged" in message
        assert "status=200" in message

    def test_long_body_is_truncated_and_marked(self, monkeypatch):
        """Bodies past the cap are cut, the cut is marked, and the tail is
        absent -- the excerpt must quote, not paraphrase."""
        client = self._patch_get(
            monkeypatch,
            body=b"<head-start>" + b"x" * 700 + b"<tail-marker>",
        )
        message = self._timeout_error(client)
        assert "<head-start>" in message
        assert "<tail-marker>" not in message
        # 725 bytes served (12 + 700 + 13), 512 shown -> 213 cut.
        assert "(+213 more bytes)" in message

    def test_non_text_body_is_summarized_not_dumped(self, monkeypatch):
        """A body that is not decodable text (e.g. a binary/image response)
        gets a by-length summary instead of mojibake."""
        png_like = b"\x89PNG\r\n\x1a\n" + b"\x00\x01\x02" * 9
        client = self._patch_get(monkeypatch, body=png_like)
        message = self._timeout_error(client)
        assert "<non-text body, 35 bytes>" in message
        assert "Condition not met" in message  # timeout itself still reported

    def test_multibyte_cut_is_not_mislabeled_non_text(self, monkeypatch):
        """Cutting the excerpt at a byte boundary can split a multi-byte
        character; that must not misreport a text body as non-text."""
        from pagespeed_test_framework.client import _format_last_body_excerpt
        # 'z' + 300 two-byte chars = 601 bytes; the 512-byte cut lands inside
        # the 256th two-byte char.
        body = "z".encode() + "ä".encode() * 300
        excerpt = _format_last_body_excerpt(body)
        assert "non-text" not in excerpt
        assert "(+90 more bytes)" in excerpt  # 601 - 511 kept bytes

    def test_excerpt_keeps_the_error_on_one_line(self, monkeypatch):
        """A multi-line HTML body must not blow the failure report into a
        wall of lines; newlines are repr-escaped."""
        client = self._patch_get(
            monkeypatch, body=b"<html>\n<head>\n<title>x</title>"
        )
        message = self._timeout_error(client)
        assert "\\n" in message  # escaped, not literal
        assert "\n" not in message

    def test_empty_body_omits_the_excerpt(self, monkeypatch):
        """Nothing to quote: no body= segment at all."""
        client = self._patch_get(monkeypatch, body=b"")
        message = self._timeout_error(client)
        assert "body=" not in message


class TestRequireMatch:
    """Tests for require_match.

    The "could not find X in the response" shape must FAIL with the body
    attached for diagnosis, never skip: the missing artifact is the
    product's job, so its absence is a regression, not an environment
    condition.
    """

    def test_returns_match_on_hit(self):
        match = require_match(r'src="([^"]+)"', 'a src="x.jpg" b', "image URL")
        assert match.group(1) == "x.jpg"

    def test_supports_flags(self):
        match = require_match(r"PAGEHIDE", "a pagehide b", "trigger",
                              flags=re.IGNORECASE)
        assert match.group(0) == "pagehide"

    def test_accepts_response(self):
        response = Response(status=200, headers={}, body=b'src="y.jpg"')
        match = require_match(r'src="([^"]+)"', response, "image URL")
        assert match.group(1) == "y.jpg"

    def test_fails_not_skips_on_miss(self):
        with pytest.raises(pytest.fail.Exception):
            require_match(r"\.pagespeed\.ic", "<html>plain</html>",
                          "rewritten image URL")

    def test_failure_attaches_body_and_pattern(self):
        body = "<html>" + "x" * 100 + "</html>"
        with pytest.raises(pytest.fail.Exception) as err:
            require_match(r"\.pagespeed\.ic", body, "rewritten image URL")
        message = str(err.value)
        assert "rewritten image URL" in message
        assert r"\.pagespeed\.ic" in message
        assert body in message

    def test_failure_truncates_long_body(self):
        body = "y" * 5000
        with pytest.raises(pytest.fail.Exception) as err:
            require_match(r"absent", body, "artifact")
        message = str(err.value)
        assert "truncated" in message
        assert len(message) < 3000  # ~2KB cap + scaffolding, not the full 5KB

    def test_failure_reports_body_length(self):
        with pytest.raises(pytest.fail.Exception) as err:
            require_match(r"absent", "z" * 5000, "artifact")
        assert "5000 chars" in str(err.value)

    def test_failure_includes_url_when_response_has_one(self):
        """Passing the Response (not response.text) names the URL."""
        response = Response(
            status=200, headers={}, body=b"<html>plain</html>",
            url="/rewrite_images.html?PageSpeedFilters=rewrite_images",
        )
        with pytest.raises(pytest.fail.Exception) as err:
            require_match(r"\.pagespeed\.ic", response, "rewritten image URL")
        message = str(err.value)
        assert "/rewrite_images.html?PageSpeedFilters=rewrite_images" in message
        assert "rewritten image URL" in message

    def test_failure_omits_url_note_for_plain_text(self):
        """The text-based signature keeps working, without a URL note."""
        with pytest.raises(pytest.fail.Exception) as err:
            require_match(r"absent", "<html>plain</html>", "artifact")
        assert " for /" not in str(err.value)

    def test_failure_omits_url_note_when_response_has_no_url(self):
        """Response without a URL (default) produces no dangling 'for'."""
        response = Response(status=200, headers={}, body=b"plain")
        with pytest.raises(pytest.fail.Exception) as err:
            require_match(r"absent", response, "artifact")
        assert " for " not in str(err.value)


class TestRequireStatusOk:
    """Tests for require_status_ok.

    For endpoints that are unambiguously configured on the lane, a non-200
    is a regression signal and must fail with context, not skip.
    """

    def test_passes_on_200(self):
        response = Response(status=200, headers={}, body=b"ok", url="/x")
        assert require_status_ok(response, "Admin page") is response

    def test_fails_not_skips_on_404(self):
        response = Response(status=404, headers={}, body=b"nope", url="/admin")
        with pytest.raises(pytest.fail.Exception):
            require_status_ok(response, "Admin page")

    def test_failure_carries_status_url_and_body(self):
        response = Response(status=503, headers={}, body=b"backend down",
                            url="/pagespeed_admin/cache")
        with pytest.raises(pytest.fail.Exception) as err:
            require_status_ok(response, "Admin cache page")
        message = str(err.value)
        assert "Admin cache page" in message
        assert "503" in message
        assert "/pagespeed_admin/cache" in message
        assert "backend down" in message


class TestRequireNoAuthGate:
    """Tests for require_no_auth_gate.

    A 401/403 from the admin handler is a plausible regression, not an
    environment condition: the lanes run these endpoints without auth.
    """

    def test_passes_on_200(self):
        response = Response(status=200, headers={}, body=b"ok")
        require_no_auth_gate(response, "Admin endpoint")

    def test_passes_on_other_non_auth_status(self):
        # e.g. a redirect or even a 500 is not an auth gate; other
        # assertions cover those.
        response = Response(status=301, headers={}, body=b"")
        require_no_auth_gate(response, "Admin endpoint")

    def test_fails_not_skips_on_403(self):
        response = Response(status=403, headers={}, body=b"forbidden")
        with pytest.raises(pytest.fail.Exception):
            require_no_auth_gate(response, "Admin endpoint")

    def test_fails_not_skips_on_401(self):
        response = Response(status=401, headers={}, body=b"unauthorized")
        with pytest.raises(pytest.fail.Exception):
            require_no_auth_gate(response, "Admin endpoint")

    def test_failure_explains_and_attaches_body(self):
        response = Response(status=403, headers={}, body=b"go away",
                            url="/pagespeed_admin")
        with pytest.raises(pytest.fail.Exception) as err:
            require_no_auth_gate(response, "Admin endpoint /pagespeed_admin")
        message = str(err.value)
        assert "Admin endpoint /pagespeed_admin" in message
        assert "403" in message
        assert "go away" in message

    def test_allow_marker_permits_expected_rejection(self):
        # The CSRF gate's own rejection is the behavior under test; only an
        # upstream auth gate's 403 is suspicious.
        response = Response(status=403, headers={},
                            body=b"Missing or invalid CSRF headers")
        require_no_auth_gate(
            response, "JSON mutation endpoint",
            allow_marker="Missing or invalid CSRF headers",
        )

    def test_allow_marker_mismatch_still_fails(self):
        response = Response(status=403, headers={}, body=b"token required")
        with pytest.raises(pytest.fail.Exception):
            require_no_auth_gate(
                response, "JSON mutation endpoint",
                allow_marker="Missing or invalid CSRF headers",
            )

class TestPytestTimeoutScaling:
    """conftest.pytest_configure must cap pytest's per-test timeout ABOVE
    fetch_until's worst case: ini timeout x multiplier x (1 + retries).

    At the AppVerif matrix's values (multiplier 6, retries 1) the old
    multiplier-only cap (720s) could fire at the exact instant the retry
    budget ended -- killing a poll seconds before convergence and
    pre-empting fetch_until's own, better-instrumented TimeoutError."""

    class _StubConfig:
        """Minimal stand-in for pytest's Config: pytest_configure reads
        getini("timeout"), writes option.timeout, and registers markers."""

        def __init__(self, ini_timeout=120):
            from types import SimpleNamespace
            self.option = SimpleNamespace(timeout=None)
            self._ini_timeout = ini_timeout

        def getini(self, name):
            return self._ini_timeout if name == "timeout" else None

        def addinivalue_line(self, *_args):
            pass

    def _configure(self, monkeypatch, multiplier=None, retries=None):
        import conftest
        if multiplier is None:
            monkeypatch.delenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", raising=False)
        else:
            monkeypatch.setenv("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", multiplier)
        if retries is None:
            monkeypatch.delenv("PAGESPEED_TEST_FETCH_RETRIES", raising=False)
        else:
            monkeypatch.setenv("PAGESPEED_TEST_FETCH_RETRIES", retries)
        config = self._StubConfig()
        conftest.pytest_configure(config)
        return config.option.timeout

    def test_untouched_by_default(self, monkeypatch):
        """No envs => no override; pytest.ini's own value stands."""
        assert self._configure(monkeypatch) is None

    def test_multiplier_only(self, monkeypatch):
        assert self._configure(monkeypatch, multiplier="4") == 480

    def test_retries_only(self, monkeypatch):
        """Retries raise the worst case even at multiplier 1."""
        assert self._configure(monkeypatch, retries="1") == 240

    def test_multiplier_and_retries(self, monkeypatch):
        """The nightly AppVerif matrix combination:
        120 x 6 x (1 + 1) = 1440, covering a 120s-base test's full
        two-attempt budget."""
        assert self._configure(monkeypatch, multiplier="6", retries="1") == 1440

    def test_garbage_envs_leave_timeout_untouched(self, monkeypatch):
        """client.py's parsing semantics (invalid/<=0 -> default) apply."""
        assert self._configure(monkeypatch, multiplier="junk", retries="junk") is None


class _CaptureHandler(socketserver.StreamRequestHandler):
    """Records one raw request (request line + header lines) and answers 200."""

    def handle(self):
        lines = []
        while True:
            line = self.rfile.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            lines.append(line.decode("latin-1").rstrip("\r\n"))
        self.server.captured.append(lines)
        self.wfile.write(
            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Probe: 1\r\n"
            b"Connection: close\r\n\r\nok"
        )


@pytest.fixture
def capture_server():
    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), _CaptureHandler)
    server.daemon_threads = True
    server.captured = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()


def _header_lines(request_lines, name):
    prefix = name.lower() + ":"
    return [line for line in request_lines[1:] if line.lower().startswith(prefix)]


class TestVhostClient:
    """VhostClient reproduces `http_proxy=$SECONDARY_HOSTNAME wget http://<vhost>/...`."""

    def _client(self, server, vhost="signed-urls.example.com"):
        return VhostClient(vhost, "127.0.0.1", server.server_address[1])

    def test_request_line_and_host_name_the_bare_vhost(self, capture_server):
        response = self._client(capture_server).get("/mod_pagespeed_example/index.html")
        assert response.status == 200 and response.text == "ok"
        assert response.header("X-Probe") == "1"
        assert response.url == "http://signed-urls.example.com/mod_pagespeed_example/index.html"
        request = capture_server.captured[0]
        assert request[0] == (
            "GET http://signed-urls.example.com/mod_pagespeed_example/index.html HTTP/1.1"
        )
        # No ":80": PageSpeed builds absolute URLs from this Host.
        assert _header_lines(request, "Host") == ["Host: signed-urls.example.com"]

    def test_purge_uses_the_purge_verb(self, capture_server):
        self._client(capture_server, "purge.example.com").purge("/*")
        assert capture_server.captured[0][0] == "PURGE http://purge.example.com/* HTTP/1.1"

    def test_extra_headers_are_sent(self, capture_server):
        self._client(capture_server).get("/x", headers={"Cookie": "PageSpeedFilters=+debug"})
        assert _header_lines(capture_server.captured[0], "Cookie") == [
            "Cookie: PageSpeedFilters=+debug"
        ]

    def test_with_user_agent_keeps_the_vhost(self, capture_server):
        client = self._client(capture_server, "ipro-for-browser.example.com")
        client.with_user_agent("webp").get("/images/Puzzle.jpg")
        request = capture_server.captured[0]
        assert request[0] == "GET http://ipro-for-browser.example.com/images/Puzzle.jpg HTTP/1.1"
        assert _header_lines(request, "User-Agent") == ["User-Agent: webp"]

    def test_base_url(self):
        assert VhostClient("cdn.pm.example.com", "localhost", 8081).base_url == (
            "http://cdn.pm.example.com"
        )

    def test_relative_path_is_rejected(self):
        with pytest.raises(ValueError):
            VhostClient("a.example.com", "localhost", 8081).get("mod_pagespeed_example/")


class TestLaneFixtureGate:
    """A lane fixture name is either known or a hard error -- never a silent skip."""

    def test_known_names_are_pinned(self):
        import conftest
        assert conftest.KNOWN_LANE_FIXTURES == frozenset({
            "debug_conf_dirs", "admin_handlers", "stats_log",
            "compressed_metadata_cache", "cache_flush", "doc_root_scratch",
            "secondary_vhosts", "external_origin", "flush_origin",
            "experiment_framework", "remote_config", "option_response_headers",
        })

    def test_parse_ignores_blanks_and_spaces(self):
        import conftest
        assert conftest.parse_lane_fixtures(" stats_log, ,cache_flush,") == frozenset(
            {"stats_log", "cache_flush"}
        )

    def test_unknown_names_are_reported(self):
        import conftest
        assert conftest.unknown_lane_fixtures(["secondary_vhosts", "secondary_vhost"]) == [
            "secondary_vhost"
        ]

    def test_missing_names_are_reported_in_order(self):
        import conftest
        assert conftest.missing_lane_fixtures(
            ("secondary_vhosts", "flush_origin", "stats_log"),
            frozenset({"stats_log"}),
        ) == ["secondary_vhosts", "flush_origin"]

    def test_runner_advertising_a_typo_is_a_usage_error(self):
        import conftest
        with pytest.raises(pytest.UsageError):
            conftest.validate_advertised_lane_fixtures("stats_log,flush_orgin")

    def test_runner_advertising_known_names_is_accepted(self):
        import conftest
        conftest.validate_advertised_lane_fixtures("stats_log,flush_origin")
        conftest.validate_advertised_lane_fixtures("")


class _FakeClock:
    """A wall clock that advances only by what is slept on it."""

    def __init__(self, now: float, advances: bool = True):
        self.now_value = now
        self.advances = advances
        self.slept = 0.0

    def now(self) -> float:
        return self.now_value

    def sleep(self, seconds: float) -> None:
        self.slept += seconds
        if self.advances:
            self.now_value += seconds


class TestFlushLeavesTheFlushSecond:
    """flush_cache must not return inside the second its stamp names.

    The server treats a cache entry whose whole-second Date is <= the flush
    stamp as stale, so a test rewriting in that second mints .pagespeed.
    URLs that are never served from cache (test_source_maps right after
    test_show_cache's flush).
    """

    def test_returns_at_once_when_the_second_is_over(self):
        import conftest
        clock = _FakeClock(1001.2)
        assert conftest.wait_until_second_after(1000, clock.now, clock.sleep)
        assert clock.slept == 0.0

    def test_waits_for_the_next_second(self):
        import conftest
        clock = _FakeClock(1000.56)
        assert conftest.wait_until_second_after(1000, clock.now, clock.sleep)
        assert int(clock.now()) == 1001
        assert 0.4 <= clock.slept <= 0.5

    def test_waits_out_a_stamp_ahead_of_the_clock(self):
        # flush_cache waits for the clock to reach its stamp before touching
        # the file, so at the call site the stamp is never ahead of the
        # clock; the helper still has to cope with one that is.
        import conftest
        clock = _FakeClock(1000.9)
        assert conftest.wait_until_second_after(1001, clock.now, clock.sleep)
        assert int(clock.now()) == 1002
        assert 1.1 <= clock.slept <= 1.2

    def test_gives_up_when_the_clock_does_not_advance(self):
        import conftest
        clock = _FakeClock(1000.5, advances=False)
        assert not conftest.wait_until_second_after(
            1000, clock.now, clock.sleep, deadline_s=1.0
        )
        assert clock.slept >= 1.0


@pytest.fixture
def origin_server(tmp_path):
    (tmp_path / "a.jpg").write_bytes(b"\xff\xd8\xff\xe0fake-jpeg")
    server = flush_origin.make_server(0, str(tmp_path))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()


class TestFlushOriginAndFetchChunks:
    """The PHP-free origin reproduces the PHP pages; fetch_chunks sees its flushes."""

    def _fetch(self, server, route):
        port = server.server_address[1]
        return fetch_chunks("127.0.0.1", port, f"/{route}", f"127.0.0.1:{port}", timeout=5.0)

    def test_slow_flushing_response_arrives_in_timed_chunks(self, origin_server):
        response = self._fetch(origin_server, "slow_flushing_html_response?delay=0.2")
        assert response.status == 200 and response.chunked
        assert response.header("Content-Type").startswith("text/html")
        # head, five <p>foo</p><div>bar:N</div> chunks, tail
        assert len(response.chunks) == 7
        assert [c.data for c in response.chunks[1:6]] == [
            f"<p>foo</p><div>bar:{i}</div>\n".encode() for i in range(1, 6)
        ]
        # four 0.2 s pauses separate the first and the fifth paragraph chunk
        assert response.chunks[5].elapsed - response.chunks[1].elapsed >= 0.75
        assert response.body.startswith(b"<html><head></head><body>\n")
        assert response.body.endswith(b"</body></html>\n")

    def test_withoutflush_is_one_content_length_response(self, origin_server):
        response = self._fetch(origin_server, "withoutflush")
        assert response.status == 200 and not response.chunked
        assert response.header("X-My-PHP-Header") == "without_flush"
        assert int(response.header("Content-Length")) == len(response.body)
        assert b"Content generated by PHP without a call to flush()!" in response.body

    def test_withflush_flushes_the_head_first(self, origin_server):
        response = self._fetch(origin_server, "withflush")
        assert response.chunked and len(response.chunks) == 2
        assert response.header("X-My-PHP-Header") == "with_flush"
        assert response.chunks[0].data.rstrip().endswith(b"<body>")
        assert b"Content generated by PHP with a call to flush()!" in response.chunks[1].data

    def test_image_rewrite_with_flush_splits_inside_the_list_item(self, origin_server):
        response = self._fetch(origin_server, "image_rewrite_with_flush")
        assert response.chunked and len(response.chunks) == 2
        assert b'<li data-thumb="a.jpg">' in response.chunks[0].data
        assert b'<img src="a.jpg"/>' in response.chunks[1].data

    def test_image_is_served_from_the_image_dir(self, origin_server):
        response = self._fetch(origin_server, "a.jpg")
        assert response.status == 200
        assert response.header("Content-Type") == "image/jpeg"
        assert response.body == b"\xff\xd8\xff\xe0fake-jpeg"

    def test_gzip_css_is_pre_gzipped(self, origin_server):
        import gzip
        response = self._fetch(origin_server, "gzip.css")
        assert response.header("Content-Encoding") == "gzip"
        assert int(response.header("Content-Length")) == len(response.body)
        css = gzip.decompress(response.body)
        assert css.startswith(b".peachpuff {background-color: peachpuff;}\n.")
        assert css.endswith(b" {background-color: antiquewhite;}\n")
        assert len(css) > 10000

    def test_unknown_route_is_404(self, origin_server):
        assert self._fetch(origin_server, "nope").status == 404

    def test_content_type_routes(self, origin_server):
        present = self._fetch(origin_server, "content_type_present/")
        assert present.status == 200
        assert present.header("Content-Type") == "text/plain"
        assert b"This file should be proxied" in present.body
        absent = self._fetch(origin_server, "content_type_absent/")
        assert absent.status == 200
        assert absent.header("Content-Type") == ""
        assert b"This file should not be proxied" in absent.body


class TestResponseRawHeaders:
    """Response keeps every header line so duplicates stay countable."""

    def test_header_values_are_case_insensitive_and_keep_duplicates(self):
        response = Response(
            status=200,
            headers={"Cache-Control": "max-age=1, private"},
            body=b"",
            raw_headers=[
                ("Cache-Control", "max-age=1"),
                ("cache-control", "private"),
                ("X-Other", "1"),
            ],
        )
        assert response.header_values("Cache-Control") == ["max-age=1", "private"]
        assert response.header_values("CACHE-CONTROL") == ["max-age=1", "private"]
        assert response.header_values("Vary") == []

    def test_hand_built_response_has_no_raw_headers(self):
        response = Response(status=200, headers={"A": "1"}, body=b"")
        assert response.raw_headers == []
        assert response.header_values("A") == []


class TestCountMatchingLines:
    """count_matching_lines is grep -c: lines, not occurrences."""

    def test_counts_lines_not_occurrences(self):
        content = "a style here\nnothing\nstyle and style\n"
        assert count_matching_lines(content, "style") == 2
        assert count_matching_lines(content, r"^nothing$") == 1
        assert count_matching_lines(content, "absent") == 0


class TestSettledStats:
    """settled_stats returns once the named counters stop moving."""

    def test_waits_until_counters_stop_moving(self):
        snapshots = iter([
            {"a": 1, "b": 5, "curl_fetch_active_count": 0},
            {"a": 2, "b": 6, "curl_fetch_active_count": 0},
            {"a": 2, "b": 7, "curl_fetch_active_count": 0},
        ])
        assert settled_stats(
            lambda: next(snapshots), ["a"], interval=0, quiet_samples=2
        ) == {
            "a": 2,
            "b": 7,
            "curl_fetch_active_count": 0,
        }

    def test_needs_the_whole_quiet_run(self):
        # A single repeat is not enough when three agreeing samples are asked
        # for: the counter moving again restarts the run.
        snapshots = iter([
            {"a": 1, "n": 0}, {"a": 1, "n": 1}, {"a": 2, "n": 2},
            {"a": 2, "n": 3}, {"a": 2, "n": 4},
        ])
        idle = {"curl_fetch_active_count": 0}
        assert settled_stats(
            lambda: dict(next(snapshots), **idle), ["a"], interval=0,
            quiet_samples=3,
        ) == {"a": 2, "n": 4, "curl_fetch_active_count": 0}

    def test_a_fetch_in_flight_is_not_settled(self):
        snapshots = iter([
            {"a": 1, "curl_fetch_active_count": 1, "n": 0},
            {"a": 1, "curl_fetch_active_count": 1, "n": 1},
            {"a": 1, "curl_fetch_active_count": 0, "n": 2},
            {"a": 1, "curl_fetch_active_count": 0, "n": 3},
        ])
        assert settled_stats(
            lambda: next(snapshots), ["a"], interval=0, quiet_samples=2
        )["n"] == 3

    def test_fails_when_counters_keep_moving(self):
        counter = itertools.count()
        with pytest.raises(AssertionError, match="never settled"):
            settled_stats(
                lambda: {"a": next(counter), "curl_fetch_active_count": 0},
                ["a"], interval=0, timeout=0,
            )

    def test_a_snapshot_with_the_keys_settles(self):
        snapshots = iter([
            {"a": 3, "curl_fetch_active_count": 0, "n": 0},
            {"a": 3, "curl_fetch_active_count": 0, "n": 1},
        ])
        assert settled_stats(
            lambda: next(snapshots), ["a"], interval=0, quiet_samples=2
        )["n"] == 1

    def test_missing_requested_counter_never_settles(self):
        with pytest.raises(
            AssertionError, match=r"never settled.*2 statistics.*lacks \['a'\]"
        ):
            settled_stats(
                lambda: {"b": 1, "curl_fetch_active_count": 0},
                ["a"], interval=0, timeout=0.05,
            )

    def test_a_server_without_the_gauge_settles_on_counters(self):
        snapshots = iter([
            {"a": 1, "n": 0}, {"a": 1, "n": 1}, {"a": 1, "n": 2},
        ])
        assert settled_stats(
            lambda: next(snapshots), ["a"], interval=0, quiet_samples=3
        )["n"] == 2

    def test_counters_only_failure_says_so(self):
        counter = itertools.count()
        with pytest.raises(
            AssertionError, match=r"never settled.*counters only.*exposes none"
        ):
            settled_stats(
                lambda: {"a": next(counter)}, ["a"], interval=0, timeout=0.05
            )

    def test_a_present_nonzero_gauge_never_settles(self):
        with pytest.raises(
            AssertionError,
            match=r"never settled.*in flight \{'curl_fetch_active_count': 2\}",
        ):
            settled_stats(
                lambda: {"a": 1, "curl_fetch_active_count": 2},
                ["a"], interval=0, timeout=0.05,
            )

    def test_a_gauge_appearing_later_must_read_zero(self):
        snapshots = iter([
            {"a": 1, "n": 0},
            {"a": 1, "curl_fetch_active_count": 1, "n": 1},
            {"a": 1, "n": 2},
            {"a": 1, "n": 3},
        ])
        assert settled_stats(
            lambda: next(snapshots), ["a"], interval=0, quiet_samples=2
        )["n"] == 3

    def test_an_exposed_gauge_that_vanishes_is_not_quiet(self):
        with pytest.raises(
            AssertionError,
            match=r"never settled.*lacks \['curl_fetch_active_count'\]",
        ):
            first = [True]

            def capture():
                if first[0]:
                    first[0] = False
                    return {"a": 1, "curl_fetch_active_count": 0}
                return {"a": 1}

            settled_stats(capture, ["a"], interval=0, timeout=0.05)

    def test_empty_snapshots_never_settle(self):
        with pytest.raises(
            AssertionError,
            match=r"never settled.*no complete snapshot.*0 statistics.*lacks \['a'\]",
        ):
            settled_stats(lambda: {}, ["a"], interval=0, timeout=0.05)

    def test_an_empty_snapshot_restarts_the_run(self):
        snapshots = iter([
            {"a": 1, "curl_fetch_active_count": 0, "n": 0},
            {},
            {"a": 1, "curl_fetch_active_count": 0, "n": 2},
            {"a": 1, "curl_fetch_active_count": 0, "n": 3},
            {"a": 1, "curl_fetch_active_count": 0, "n": 4},
        ])
        assert settled_stats(
            lambda: next(snapshots), ["a"], interval=0, quiet_samples=3
        )["n"] == 4

    def test_fails_when_a_fetch_stays_in_flight(self):
        with pytest.raises(AssertionError, match="never settled.*in flight"):
            settled_stats(
                lambda: {"a": 0, "curl_fetch_active_count": 1},
                ["a"], interval=0, timeout=0,
            )


class _DuplicateHeaderCaptureHandler(socketserver.StreamRequestHandler):
    """Like _CaptureHandler, but answers with two Cache-Control header lines
    so a VhostClient response can prove it keeps duplicates too."""

    def handle(self):
        lines = []
        while True:
            line = self.rfile.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            lines.append(line.decode("latin-1").rstrip("\r\n"))
        self.server.captured.append(lines)
        self.wfile.write(
            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
            b"Cache-Control: max-age=1\r\ncache-control: private\r\n"
            b"Connection: close\r\n\r\nok"
        )


@pytest.fixture
def duplicate_header_capture_server():
    server = socketserver.ThreadingTCPServer(
        ("127.0.0.1", 0), _DuplicateHeaderCaptureHandler
    )
    server.daemon_threads = True
    server.captured = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()


class TestVhostClientRawHeaders:
    """VhostClient._request populates raw_headers too, duplicates preserved.

    A named vhost's Response is the one every negative header check
    (Vary == [], Set-Cookie == []) runs against in the later caching and
    proxying ports; if VhostClient dropped raw_headers, those checks would
    pass vacuously instead of actually checking anything.
    """

    def test_duplicate_headers_are_preserved(self, duplicate_header_capture_server):
        client = VhostClient(
            "signed-urls.example.com",
            "127.0.0.1",
            duplicate_header_capture_server.server_address[1],
        )
        response = client.get("/mod_pagespeed_example/index.html")
        assert response.header_values("Cache-Control") == ["max-age=1", "private"]
        assert response.header_values("CACHE-CONTROL") == ["max-age=1", "private"]
        assert response.header_values("Vary") == []


if __name__ == "__main__":
    # run_pytest sys.exit()s pytest's return code; a bare pytest.main(...)
    # would drop it, and the bazel py_test would report PASS on a red suite.
    # argv passthrough: bazel invokes this main with no args,
    # so the default runs this file; the exit-status regression guard passes
    # a synthetic case file, driving this exact shipped entry point.
    run_pytest(sys.argv[1:] or [__file__, "-v"])
