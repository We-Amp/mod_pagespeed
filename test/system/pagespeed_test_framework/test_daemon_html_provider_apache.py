#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Unit tests for the Apache provider's settle(), without a server.

settle() must not return while a request the provider has an answer for has
no access log line yet: httpd writes that line after the request's HTML
cache commit, so returning earlier would let a row read the volume before
the commit.

Run with: python -m pytest -v test_daemon_html_provider_apache.py
"""

import os
import tempfile
import threading
import time

import pytest

from pagespeed_test_framework.daemon_html_provider_apache import ApacheProvider


def _provider(log_path: str) -> ApacheProvider:
    p = ApacheProvider.__new__(ApacheProvider)
    p.access_log = log_path
    p.answered = {}
    p.answered_lock = threading.Lock()
    p.log_offset = 0
    p.settle_timeout_s = 0.3
    return p


@pytest.fixture
def log_path():
    fd, path = tempfile.mkstemp()
    os.close(fd)
    yield path
    os.unlink(path)


def test_an_answered_request_without_a_line_holds_settle(log_path):
    p = _provider(log_path)
    p.answered = {40001: 1}
    with pytest.raises(AssertionError, match="never finished"):
        p.settle()


def test_settle_returns_once_the_line_is_there(log_path):
    p = _provider(log_path)
    p.answered = {40001: 1}
    p.settle_timeout_s = 5

    def write_later():
        time.sleep(0.2)
        with open(log_path, "a") as f:
            f.write("40001 200 GET /page\n")

    t = threading.Thread(target=write_later)
    t.start()
    start = time.monotonic()
    p.settle()
    t.join()
    assert time.monotonic() - start >= 0.15


def test_a_second_request_on_the_same_port_needs_its_own_line(log_path):
    p = _provider(log_path)
    with open(log_path, "a") as f:
        f.write("40001 200 GET /page\n")
    p.answered = {40001: 2}
    with pytest.raises(AssertionError):
        p.settle()


def test_lines_before_the_window_do_not_count(log_path):
    with open(log_path, "a") as f:
        f.write("40001 410 GET /__dhc_generation_1\n")
    p = _provider(log_path)
    p.log_offset = os.path.getsize(log_path)
    p.answered = {40001: 1}
    with pytest.raises(AssertionError):
        p.settle()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
