#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Entity tags the module mints never reach the origin.

The module issues weak validators of its own (W/"psh-..." for an HTML variant,
W/"psm-..." for a page's Markdown copy). When a request's If-None-Match names
one, the module removes If-None-Match and If-Modified-Since before any handler
evaluates them or any proxy forwards them, on every request, whatever is
configured. Every other validator passes untouched.

Lane fixture "strip_echo": name-based hosts on PAGESPEED_STRIP_TAGS_PORT, each
proxying /strip_echo/ to the origin this module starts on
PAGESPEED_STRIP_ECHO_PORT, which records the conditionals it receives:

  strip-off.example.com    PageSpeed on, nothing else (the feature off)
  strip-agent.example.com  PageSpeed on with AgentOptimize
  strip-chain.example.com  proxies /strip_echo/ to strip-agent.example.com on
                           the same server: a module in front of a module

Each host also serves the static file /strip_tags/plain.txt from its document
root, where the server's own handler answers conditionals.
"""

import itertools
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, List, Sequence, Tuple
from urllib.parse import parse_qs, urlsplit

import pytest

pytestmark = pytest.mark.requires_fixture("strip_echo")

OFF_HOST = "strip-off.example.com"
AGENT_HOST = "strip-agent.example.com"
CHAIN_HOST = "strip-chain.example.com"

MARKDOWN_TAG = 'W/"psm-0123456789abcdef"'
HTML_TAG = 'W/"psh-0123456789abcdef"'
PAST = "Sun, 15 Nov 2026 11:00:00 GMT"

ECHO_BODY = b"<!doctype html><html><head><title>echo</title></head><body><p>echo</p></body></html>\n"

# What the origin received, by request id.
_SEEN: Dict[str, Dict[str, List[str]]] = {}
_SEEN_LOCK = threading.Lock()
_IDS = itertools.count(1)


class _EchoHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):  # noqa: N802 (http.server naming)
        rid = parse_qs(urlsplit(self.path).query).get("rid", [""])[0]
        with _SEEN_LOCK:
            _SEEN[rid] = {
                "if-none-match": self.headers.get_all("If-None-Match") or [],
                "if-modified-since": self.headers.get_all("If-Modified-Since") or [],
            }
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "max-age=300")
        self.send_header("ETag", '"v1"')
        self.send_header("Content-Length", str(len(ECHO_BODY)))
        self.end_headers()
        self.wfile.write(ECHO_BODY)

    def log_message(self, *args):  # quiet
        pass


@pytest.fixture(scope="module", autouse=True)
def echo_origin():
    port = int(os.environ["PAGESPEED_STRIP_ECHO_PORT"])
    server = ThreadingHTTPServer(("127.0.0.1", port), _EchoHandler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()


def _request(
    host: str, path: str, headers: Sequence[Tuple[str, str]]
) -> Tuple[int, List[Tuple[str, str]]]:
    """One GET with exactly these header lines (repeats kept, in order)."""
    port = int(os.environ["PAGESPEED_STRIP_TAGS_PORT"])
    lines = [f"GET {path} HTTP/1.1", f"Host: {host}", "Connection: close", "Accept-Encoding: identity"]
    lines += [f"{name}: {value}" for name, value in headers]
    with socket.create_connection(("localhost", port), timeout=30) as conn:
        conn.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        data = b""
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
    head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1").split("\r\n")
    status = int(head[0].split(" ", 2)[1])
    response_headers = []
    for line in head[1:]:
        name, _, value = line.partition(":")
        response_headers.append((name.strip(), value.strip()))
    return status, response_headers


def _through_origin(host: str, headers: Sequence[Tuple[str, str]]) -> Tuple[int, Dict[str, List[str]]]:
    """The status a client gets, and what the origin received."""
    rid = f"r{next(_IDS)}-{time.monotonic_ns()}"
    status, _ = _request(host, f"/strip_echo/page.html?rid={rid}", headers)
    with _SEEN_LOCK:
        seen = _SEEN.get(rid)
    assert seen is not None, f"the origin never saw request {rid} (status {status})"
    return status, seen


def _assert_nothing_forwarded(seen: Dict[str, List[str]]) -> None:
    assert seen["if-none-match"] == [], seen
    assert seen["if-modified-since"] == [], seen


@pytest.mark.parametrize("host", [AGENT_HOST, OFF_HOST])
def test_markdown_tag_on_fall_through_is_not_forwarded(host):
    status, seen = _through_origin(host, [("If-None-Match", MARKDOWN_TAG)])
    assert status == 200
    _assert_nothing_forwarded(seen)


def test_feature_off_drops_tag_and_if_modified_since():
    status, seen = _through_origin(OFF_HOST, [("If-None-Match", MARKDOWN_TAG), ("If-Modified-Since", PAST)])
    assert status == 200
    _assert_nothing_forwarded(seen)


def test_html_tag_on_markdown_request_is_not_forwarded():
    status, seen = _through_origin(
        AGENT_HOST,
        [("Accept", "text/markdown"), ("If-None-Match", HTML_TAG), ("If-Modified-Since", PAST)],
    )
    assert status == 200
    _assert_nothing_forwarded(seen)


@pytest.mark.parametrize(
    "inm",
    [
        '"v1", ' + MARKDOWN_TAG,  # a module tag in a list
        '"psh-0123456789abcdef"',  # strong spelling
        "psm-0123456789abcdef",  # unquoted
        'w/"psm-1"',  # lower-case weakness prefix
        '"x, W/"psm-1"',  # a stray quote
    ],
)
def test_module_tag_in_any_spelling_drops_both_fields(inm):
    status, seen = _through_origin(AGENT_HOST, [("If-None-Match", inm), ("If-Modified-Since", PAST)])
    assert status == 200
    _assert_nothing_forwarded(seen)


def test_module_tag_on_a_second_header_line_drops_every_line():
    headers = [("If-None-Match", '"v1"'), ("If-None-Match", MARKDOWN_TAG), ("If-Modified-Since", PAST)]
    if os.environ.get("PAGESPEED_SERVER_TYPE") == "nginx":
        # nginx refuses a repeated If-None-Match line itself, before any
        # module runs: nothing reaches the origin.
        rid = f"r{next(_IDS)}-{time.monotonic_ns()}"
        status, _ = _request(AGENT_HOST, f"/strip_echo/page.html?rid={rid}", headers)
        assert status == 400
        with _SEEN_LOCK:
            assert rid not in _SEEN
        return
    status, seen = _through_origin(AGENT_HOST, headers)
    assert status == 200
    _assert_nothing_forwarded(seen)


@pytest.mark.parametrize("inm", ['W/"v1"', '"v1", W/"v2"', '"xpsm-1"', "*"])
def test_foreign_validators_are_forwarded_untouched(inm):
    _, seen = _through_origin(AGENT_HOST, [("If-None-Match", inm), ("If-Modified-Since", PAST)])
    assert seen["if-none-match"] == [inm], seen
    assert seen["if-modified-since"] == [PAST], seen


def test_if_modified_since_alone_is_forwarded():
    _, seen = _through_origin(AGENT_HOST, [("If-Modified-Since", PAST)])
    assert seen["if-none-match"] == [], seen
    assert seen["if-modified-since"] == [PAST], seen


def test_chained_module_origin_never_sees_an_outer_tag():
    status, seen = _through_origin(CHAIN_HOST, [("If-None-Match", MARKDOWN_TAG), ("If-Modified-Since", PAST)])
    assert status == 200
    _assert_nothing_forwarded(seen)
    # And a foreign tag still crosses both servers.
    _, seen = _through_origin(CHAIN_HOST, [("If-None-Match", 'W/"v1"')])
    assert seen["if-none-match"] == ['W/"v1"'], seen


# The server's own static handler answers conditionals: a module tag must not
# let the If-Modified-Since beside it turn a 200 into a 304.
STATIC = "/strip_tags/plain.txt"


@pytest.mark.parametrize("host", [OFF_HOST, AGENT_HOST])
def test_static_handler_never_sees_the_tag(host):
    status, headers = _request(host, STATIC, [])
    assert status == 200
    last_modified = dict((n.lower(), v) for n, v in headers).get("last-modified")
    assert last_modified, headers
    status, _ = _request(host, STATIC, [("If-Modified-Since", last_modified)])
    assert status == 304, "control: the static handler answers If-Modified-Since"
    status, _ = _request(host, STATIC, [("If-None-Match", MARKDOWN_TAG), ("If-Modified-Since", last_modified)])
    assert status == 200
