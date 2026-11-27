#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Adversarial HTTP origin for the HTML cache live conformance lanes.

A single stdlib-only app. Each endpoint returns one fixed caching or
personalisation shape (see ENDPOINTS below); the lanes put it behind each
web server through a reverse proxy, so a probe can compare what the server
replays against what the origin sent.

Run:   python3 adversarial_origin.py --port 8099 [--bind 127.0.0.1]
Embed: server = make_server(0); threading.Thread(
           target=server.serve_forever, daemon=True).start()

Body markers
------------
Every body (HTML and JSON alike; for /gzip, after decompression) carries
exactly one of each of these fields:

    origin-token:<32 uppercase hex>:origin-token
    cred:<16 uppercase hex>:cred

HTML bodies put them in visible elements:

    <p id="origin-token">origin-token:...:origin-token</p>
    <p id="cred">cred:...:cred</p>

The token is fresh random per response, so two bodies with the same token
were minted by the same origin response. Hex is uppercase so that a
lowercase-only substitution filter (s/a/aa/) leaves the markers intact.
parse_markers(body) returns (token, cred_hash), either None when missing.

Credential hash
---------------
For each of the request headers Cookie, Authorization, X-Test-User and
Proxy-Authorization, in that order, one field is appended to a byte string:

    absent header  -> b"-"
    present header -> b"+" + <decimal byte length> + b":" + <value bytes>

The value bytes are the header value as received (latin-1). When a header
arrives more than once, its values are joined with b"\\n" in arrival order
(a newline cannot occur inside a header value). The hash is the first 16
hex digits, uppercase, of SHA-256 over the concatenation. An absent header
and an empty header hash differently. The anonymous request (all three
absent) hashes SHA-256(b"----"), which is ANONYMOUS_CRED_HASH.

/fresh and /counter-json also carry "counter:<n>:counter", the number of
GET/HEAD requests the origin has answered for that path so far.

Control endpoints (never cacheable: Cache-Control: no-store)
------------------------------------------------------------
    GET  /_control/requests           JSON {key: count}; key is the path for
                                      GET/HEAD, "<METHOD> <path>" otherwise
    POST /_control/reset              clears counters, failures, waiters
    POST /_control/fail?path=P&status=S
                                      GET P answers S (status=0 clears)
    POST /_control/release?path=/slow releases every held /slow request;
                                      &sticky=1 also answers later ones at
                                      once, until the next reset
    GET  /_control/waiting?path=/slow JSON {"waiting": n}

Every response, control endpoints and errors included, carries Date and
X-Robots-Tag: noindex. All content is synthetic. The control endpoints are
unauthenticated: a public deployment must not route /_control/ to the app.

The CGI origin kind of the live lanes runs this app's handlers through a
CGI shim the Apache lane provider owns; it forwards to one running server
so the request counts stay in one place.
"""

import argparse
import email.utils
import gzip
import hashlib
import html
import json
import re
import secrets
import string
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_BIG_BYTES = 6 * 1024 * 1024
DEFAULT_SLOW_TIMEOUT = 30.0
# Held /slow requests beyond this many are answered 503 at once, so a burst
# cannot exhaust the threads of a small container.
MAX_SLOW_WAITERS = 8
CSRF_LATE_OFFSET = 64 * 1024
SUBST_A_COUNT = 4096

TOKEN_RE = re.compile(r"origin-token:([0-9A-F]{32}):origin-token")
CRED_RE = re.compile(r"cred:([0-9A-F]{16}):cred")
COUNTER_RE = re.compile(r"counter:([0-9]+):counter")

# Agent product tokens /ua-variant reacts to (a subset of the known-agent
# table), matched case-insensitively as whole tokens.
UA_VARIANT_TOKENS = ("GPTBot", "ClaudeBot", "PerplexityBot", "CCBot",
                     "Bytespider", "OAI-SearchBot", "ChatGPT-User",
                     "Claude-User", "Amazonbot", "Applebot")
_BOUNDARY = " ();/"
_ASCII_LOWER = str.maketrans(string.ascii_uppercase, string.ascii_lowercase)

VALIDATOR_ETAG = 'W/"v1-validators"'
VALIDATOR_LAST_MODIFIED = "Mon, 02 Mar 2026 10:00:00 GMT"
_VALIDATOR_LM_TS = email.utils.parsedate_to_datetime(
    VALIDATOR_LAST_MODIFIED).timestamp()


def credential_hash(cookie, authorization, test_user, proxy_authorization=None):
    """Hash of the credential header values; None means absent."""
    buf = bytearray()
    for value in (cookie, authorization, test_user, proxy_authorization):
        if value is None:
            buf += b"-"
        else:
            raw = value.encode("latin-1") if isinstance(value, str) else value
            buf += b"+" + str(len(raw)).encode("ascii") + b":" + raw
    return hashlib.sha256(bytes(buf)).hexdigest()[:16].upper()


ANONYMOUS_CRED_HASH = credential_hash(None, None, None)


def parse_markers(body):
    """Returns (token, cred_hash) from a body (str or bytes)."""
    if isinstance(body, bytes):
        body = body.decode("utf-8", "replace")
    t = TOKEN_RE.search(body)
    c = CRED_RE.search(body)
    return (t.group(1) if t else None, c.group(1) if c else None)


def parse_counter(body):
    if isinstance(body, bytes):
        body = body.decode("utf-8", "replace")
    m = COUNTER_RE.search(body)
    return int(m.group(1)) if m else None


def token_matches(user_agent, token):
    """Case-insensitive whole-token (or phrase) match on token boundaries."""
    # ASCII-only case folding: str.lower() also folds non-ASCII letters.
    ua = user_agent.translate(_ASCII_LOWER)
    t = token.translate(_ASCII_LOWER)
    start = ua.find(t)
    while start != -1:
        end = start + len(t)
        if ((start == 0 or ua[start - 1] in _BOUNDARY) and
                (end == len(ua) or ua[end] in _BOUNDARY)):
            return True
        start = ua.find(t, start + 1)
    return False


def _new_token():
    return secrets.token_hex(16).upper()


def _cookie_value(cookie_header, name):
    if not cookie_header:
        return None
    for part in cookie_header.split(";"):
        k, sep, v = part.strip().partition("=")
        if sep and k == name:
            return v
    return None


class OriginState:
    def __init__(self, big_bytes, slow_timeout):
        self.big_bytes = big_bytes
        self.slow_timeout = slow_timeout
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.counts = {}
        self.failures = {}
        self.release_gen = {}
        self.waiting = {}
        self.open_paths = set()

    def count(self, key):
        with self.lock:
            self.counts[key] = self.counts.get(key, 0) + 1
            return self.counts[key]

    def reset(self):
        with self.cond:
            self.counts.clear()
            self.failures.clear()
            self.open_paths.clear()
            for path in list(self.release_gen):
                self.release_gen[path] += 1
            self.cond.notify_all()

    def hold(self, path):
        """Blocks until released or timed out; returns True when released."""
        deadline = time.monotonic() + self.slow_timeout
        with self.cond:
            if path in self.open_paths:
                return True
            if self.waiting.get(path, 0) >= MAX_SLOW_WAITERS:
                return None
            gen = self.release_gen.get(path, 0)
            self.waiting[path] = self.waiting.get(path, 0) + 1
            try:
                while self.release_gen.get(path, 0) == gen:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        return False
                    self.cond.wait(left)
                return True
            finally:
                self.waiting[path] -= 1

    def release(self, path, sticky=False):
        with self.cond:
            if sticky:
                self.open_paths.add(path)
            self.release_gen[path] = self.release_gen.get(path, 0) + 1
            self.cond.notify_all()


def _html(title, body_html, token, cred, head_extra=""):
    return ("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
            "<meta charset=\"utf-8\">\n"
            "<title>%s</title>\n%s</head>\n<body>\n<h1>%s</h1>\n"
            "<p id=\"origin-token\">origin-token:%s:origin-token</p>\n"
            "<p id=\"cred\">cred:%s:cred</p>\n%s\n</body>\n</html>\n"
            % (html.escape(title), head_extra, html.escape(title), token,
               cred, body_html)).encode("utf-8")


HTML_TYPE = "text/html; charset=utf-8"

# Endpoints whose GET response is a plain page: path -> (Cache-Control or
# None, extra headers, paragraph).
SIMPLE = {
    "/item": ("max-age=60", [], "Item page, invalidated by POST /item."),
    "/short": ("max-age=8", [], "Short lifetime."),
    "/cap": ("max-age=3600", [], "Long lifetime, above a typical cap."),
    "/smax": ("max-age=5, s-maxage=60", [], "Shared lifetime differs."),
    "/ext": ("max-age=30, stale-while-revalidate=600, immutable", [],
             "Extension directives."),
    "/agecheck": ("max-age=30", [("Age", "20")], "Arrives already aged."),
    "/nocache": ("no-cache", [], "Always revalidate."),
    "/zero": ("max-age=0", [], "Zero lifetime."),
    "/mr": ("max-age=10, must-revalidate", [], "Must revalidate."),
    "/swr": ("max-age=2, stale-while-revalidate=600, stale-if-error=600", [],
             "Stale directives."),
    "/ga-page": ("max-age=60", [], "Greets nobody."),
    "/setcookie": ("max-age=60", None, "Sets a cookie."),
    "/vary-cookie": ("max-age=60", [("Vary", "Cookie")], "Varies on Cookie."),
    "/vary-ua": ("max-age=60", [("Vary", "User-Agent")],
                 "Varies on User-Agent."),
    "/hdr": ("max-age=60", [], "Header multiset page."),
    "/notransform": ("no-transform, max-age=30", [], "Do not transform."),
}

GET_PATHS = sorted(set(SIMPLE) | {
    "/fresh", "/expires", "/me", "/public-me", "/bearer", "/bearer-mr",
    "/echo-ga", "/reflect-host", "/csrf", "/nonce", "/gzip", "/csrf-late",
    "/inline-nonce", "/ua-variant", "/auth", "/subst", "/validators",
    "/slow", "/big", "/residual", "/counter-json"})
UNSAFE_PATHS = ("/fresh", "/item", "/fail", "/slow")
UNSAFE_METHODS = ("POST", "PUT", "DELETE", "PATCH")


class AdversarialHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "AdversarialOrigin/1"
    sys_version = ""

    def log_message(self, fmt, *args):
        if getattr(self.server, "verbose", False):
            sys.stderr.write("%s - %s\n" % (self.address_string(),
                                            fmt % args))

    # -- plumbing ---------------------------------------------------------

    @property
    def state(self):
        return self.server.state

    def _start(self, status, headers, now=None):
        self.send_response_only(status)
        self.send_header("Server", self.version_string())
        self.send_header("Date", email.utils.formatdate(
            time.time() if now is None else now, usegmt=True))
        self.send_header("X-Robots-Tag", "noindex")
        for k, v in headers:
            self.send_header(k, v)

    def _send(self, status, headers, body=b"", now=None,
              content_type=HTML_TYPE):
        headers = list(headers)
        if body is not None and status not in (204, 304):
            headers.append(("Content-Type", content_type))
            headers.append(("Content-Length", str(len(body))))
        self._start(status, headers, now)
        self.end_headers()
        if (body and self.command != "HEAD" and
                status not in (204, 304)):
            self.wfile.write(body)
        self.log_request(status, len(body or b""))

    def _cred(self):
        def get(name):
            vals = self.headers.get_all(name)
            return None if vals is None else "\n".join(vals)
        return credential_hash(get("Cookie"), get("Authorization"),
                               get("X-Test-User"), get("Proxy-Authorization"))

    def _page(self, status, cache_control, title, body_html, extra=(),
              head_extra="", token=None):
        token = token or _new_token()
        headers = list(extra)
        if cache_control is not None:
            headers.insert(0, ("Cache-Control", cache_control))
        self._send(status, headers,
                   _html(title, body_html, token, self._cred(), head_extra))

    def _json(self, status, obj, cache_control="no-store"):
        self._send(status, [("Cache-Control", cache_control)],
                   (json.dumps(obj, sort_keys=True) + "\n").encode("utf-8"),
                   content_type="application/json")

    def _drain(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)

    def _split(self):
        u = urllib.parse.urlsplit(self.path)
        return u.path, urllib.parse.parse_qs(u.query)

    # -- dispatch ---------------------------------------------------------

    def do_GET(self):
        path, query = self._split()
        if path.startswith("/_control/"):
            return self._control_get(path, query)
        self._n = self.state.count(path)
        with self.state.lock:
            fail = self.state.failures.get(path)
        if fail:
            return self._page(fail, None, "Forced failure",
                              "<p>Forced status %d.</p>" % fail)
        handler = getattr(self, "_get_" + path[1:].replace("-", "_"), None) \
            if path in GET_PATHS else None
        if handler is not None:
            return handler(query)
        if path in SIMPLE:
            return self._simple(path)
        self._page(404, "no-store", "Not found", "<p>No such page.</p>")

    do_HEAD = do_GET

    def _unsafe(self):
        path, query = self._split()
        self._drain()
        if path.startswith("/_control/") and self.command == "POST":
            return self._control_post(path, query)
        if path not in UNSAFE_PATHS:
            self.state.count("%s %s" % (self.command, path))
            return self._send(405, [("Allow", "GET, HEAD"),
                                    ("Cache-Control", "no-store")],
                              _html("Method not allowed", "", _new_token(),
                                    self._cred()))
        self.state.count("%s %s" % (self.command, path))
        if path in ("/fresh", "/slow"):
            if self.command == "DELETE":
                return self._send(204, [("Cache-Control", "no-store")], None)
            return self._page(200, "no-store", "Updated",
                              "<p>%s accepted.</p>" % self.command)
        if path == "/item" and self.command == "POST":
            return self._page(303, "no-store", "See other",
                              "<p>Created; see /fresh.</p>",
                              extra=[("Location", "/fresh"),
                                     ("Content-Location", "/item")])
        if path == "/fail" and self.command == "POST":
            return self._page(500, "no-store", "Failure",
                              "<p>The update failed.</p>")
        self._send(405, [("Allow", "POST"), ("Cache-Control", "no-store")],
                   _html("Method not allowed", "", _new_token(),
                         self._cred()))

    do_POST = do_PUT = do_DELETE = do_PATCH = _unsafe

    # -- control ----------------------------------------------------------

    def _control_get(self, path, query):
        if path == "/_control/requests":
            with self.state.lock:
                return self._json(200, dict(self.state.counts))
        if path == "/_control/waiting":
            p = query.get("path", ["/slow"])[0]
            with self.state.lock:
                return self._json(200, {"waiting":
                                        self.state.waiting.get(p, 0)})
        self._json(404, {"error": "unknown control endpoint"})

    def _control_post(self, path, query):
        if path == "/_control/reset":
            self.state.reset()
            return self._json(200, {"reset": True})
        if path == "/_control/release":
            p = query.get("path", ["/slow"])[0]
            sticky = query.get("sticky", ["0"])[0] == "1"
            self.state.release(p, sticky)
            return self._json(200, {"released": p, "sticky": sticky})
        if path == "/_control/fail":
            p = query.get("path", [None])[0]
            try:
                status = int(query.get("status", ["500"])[0])
            except ValueError:
                status = -1
            if not p or not p.startswith("/") or not (
                    status == 0 or 100 <= status <= 599):
                return self._json(400, {"error": "need path and status"})
            with self.state.lock:
                if status:
                    self.state.failures[p] = status
                else:
                    self.state.failures.pop(p, None)
            return self._json(200, {"path": p, "status": status})
        self._json(404, {"error": "unknown control endpoint"})

    # -- endpoints --------------------------------------------------------

    def _simple(self, path):
        cc, extra, text = SIMPLE[path]
        if extra is None:  # /setcookie
            extra = [("Set-Cookie",
                      "session=%s; Path=/; HttpOnly" % secrets.token_hex(8))]
        self._page(200, cc, path[1:], "<p>%s</p>" % html.escape(text),
                   extra=extra)

    def _get_fresh(self, query):
        n = self._n
        self._page(200, "max-age=30", "fresh",
                   "<p id=\"counter\">counter:%d:counter</p>" % n)

    def _get_expires(self, query):
        now = time.time()
        token = _new_token()
        self._send(200, [("Expires",
                          email.utils.formatdate(now + 60, usegmt=True))],
                   _html("expires", "<p>Expires only.</p>", token,
                         self._cred()), now=now)

    def _greet_cookie(self, cc, title):
        cookie = self.headers.get("Cookie")
        who = html.escape(cookie) if cookie else "guest"
        self._page(200, cc, title, "<p id=\"greeting\">Hello, %s.</p>" % who)

    def _get_me(self, query):
        self._greet_cookie("max-age=60", "me")

    def _get_public_me(self, query):
        self._greet_cookie("public, max-age=60", "public-me")

    def _bearer(self, cc, title):
        if self.headers.get("Authorization") is None:
            return self._page(401, "no-store", "Unauthorized",
                              "<p>Credentials required.</p>",
                              extra=[("WWW-Authenticate",
                                      'Bearer realm="example.test"')])
        self._page(200, cc, title, "<p>Signed-in view.</p>")

    def _get_bearer(self, query):
        self._bearer("public, max-age=60", "bearer")

    def _get_bearer_mr(self, query):
        self._bearer("max-age=60, must-revalidate", "bearer-mr")

    def _get_echo_ga(self, query):
        ga = _cookie_value(self.headers.get("Cookie"), "_ga") or ""
        self._page(200, "max-age=60", "echo-ga",
                   "<p id=\"ga\">ga:%s:ga</p>" % html.escape(ga))

    def _get_reflect_host(self, query):
        host = (self.headers.get("X-Forwarded-Host") or
                self.headers.get("Host") or "example.test")
        self._page(200, "max-age=60", "reflect-host",
                   "<p><a id=\"home\" href=\"https://%s/fresh\">home</a></p>"
                   % html.escape(host, quote=True))

    def _get_csrf(self, query):
        self._page(200, "max-age=60", "csrf",
                   "<form method=\"post\" action=\"/item\">"
                   "<input type=\"hidden\" name=\"csrf_token\" value=\"%s\">"
                   "<button>Send</button></form>" % secrets.token_hex(16))

    def _get_nonce(self, query):
        nonce = secrets.token_hex(12)
        self._page(200, "max-age=60", "nonce",
                   "<script nonce=\"%s\">document.title += \"\";</script>"
                   % nonce,
                   extra=[("Content-Security-Policy",
                           "script-src 'nonce-%s'" % nonce)])

    def _get_gzip(self, query):
        body = _html("gzip", "<p>Compressed at the origin.</p>",
                     _new_token(), self._cred())
        self._send(200, [("Cache-Control", "max-age=60"),
                         ("Content-Encoding", "gzip")],
                   gzip.compress(body))

    def _get_csrf_late(self, query):
        filler = "<!-- %s -->\n" % ("x" * CSRF_LATE_OFFSET)
        self._page(200, "max-age=60", "csrf-late",
                   "%s<form method=\"post\" action=\"/item\">"
                   "<input type=\"hidden\" name=\"csrf_token\" value=\"%s\">"
                   "</form>" % (filler, secrets.token_hex(16)))

    def _get_inline_nonce(self, query):
        self._page(200, "max-age=60", "inline-nonce",
                   "<script>var requestNonce = \"%s\";</script>"
                   % secrets.token_hex(12))

    def _get_ua_variant(self, query):
        ua = self.headers.get("User-Agent") or ""
        agent = any(token_matches(ua, t) for t in UA_VARIANT_TOKENS)
        self._page(200, "max-age=60", "ua-variant",
                   "<p id=\"variant\">variant:%s:variant</p>"
                   % ("agent" if agent else "browser"))

    def _get_auth(self, query):
        if self.headers.get("Authorization") is None:
            return self._page(401, "no-store", "Unauthorized",
                              "<p>Credentials required.</p>",
                              extra=[("WWW-Authenticate",
                                      'Basic realm="example.test"')])
        self._page(200, "max-age=60", "auth", "<p>Protected page.</p>")

    def _get_subst(self, query):
        self._page(200, "max-age=60", "subst",
                   "<p id=\"subst\">%s</p>" % ("a" * SUBST_A_COUNT))

    def _get_validators(self, query):
        headers = [("Cache-Control", "max-age=30"),
                   ("ETag", VALIDATOR_ETAG),
                   ("Last-Modified", VALIDATOR_LAST_MODIFIED)]
        inm = self.headers.get("If-None-Match")
        ims = self.headers.get("If-Modified-Since")
        not_modified = False
        if inm is not None:
            tags = [t.strip() for t in inm.split(",")]
            weak = lambda t: t[2:] if t.startswith("W/") else t
            not_modified = "*" in tags or any(
                weak(t) == weak(VALIDATOR_ETAG) for t in tags)
        elif ims is not None:
            try:
                ts = email.utils.parsedate_to_datetime(ims).timestamp()
                not_modified = ts >= _VALIDATOR_LM_TS
            except (TypeError, ValueError):
                not_modified = False
        if not_modified:
            return self._send(304, headers, None)
        self._send(200, headers, _html("validators", "<p>Validated.</p>",
                                       _new_token(), self._cred()))

    def _get_slow(self, query):
        body = _html("slow", "<p>Held until released.</p>", _new_token(),
                     self._cred())
        released = self.state.hold("/slow")
        if released is None:
            return self._send(503, [("Cache-Control", "no-store")],
                              b"too many held requests\n",
                              content_type="text/plain")
        self._send(200, [("Cache-Control", "max-age=30"),
                         ("X-Origin-Slow",
                          "released" if released else "timeout")], body)

    def _get_big(self, query):
        head = _html("big", "<p>Large page follows.</p>", _new_token(),
                     self._cred())
        line = b"<p>" + b"b" * 1020 + b"</p>\n"
        reps = self.state.big_bytes // len(line) + 2
        # Streamed line by line: the page is never held in memory whole.
        self._start(200, [("Cache-Control", "max-age=60"),
                          ("Content-Type", HTML_TYPE),
                          ("Content-Length", str(len(head) + len(line) * reps))])
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(head)
            for _ in range(reps):
                self.wfile.write(line)
        self.log_request(200, len(head) + len(line) * reps)

    def _get_residual(self, query):
        user = self.headers.get("X-Test-User")
        who = html.escape(user) if user else "guest"
        self._page(200, "max-age=60", "residual",
                   "<p id=\"greeting\">Hello, %s.</p>" % who)

    def _get_counter_json(self, query):
        n = self._n
        self._json(200, {
            "counter": "counter:%d:counter" % n,
            "origin": "origin-token:%s:origin-token" % _new_token(),
            "cred": "cred:%s:cred" % self._cred(),
        }, cache_control="max-age=30")


def make_server(port=0, bind="127.0.0.1", big_bytes=DEFAULT_BIG_BYTES,
                slow_timeout=DEFAULT_SLOW_TIMEOUT, verbose=False):
    """Returns a ThreadingHTTPServer (not yet serving) for the origin."""
    server = ThreadingHTTPServer((bind, port), AdversarialHandler)
    server.daemon_threads = True
    server.state = OriginState(big_bytes, slow_timeout)
    server.verbose = verbose
    return server


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--big-bytes", type=int, default=DEFAULT_BIG_BYTES)
    ap.add_argument("--slow-timeout", type=float,
                    default=DEFAULT_SLOW_TIMEOUT)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    server = make_server(args.port, args.bind, args.big_bytes,
                         args.slow_timeout, args.verbose)
    print("adversarial origin on %s:%d" % server.server_address[:2],
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
