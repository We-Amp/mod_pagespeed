# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Programmable HTTP origin for compliance testing.

A full-featured Python HTTP server that supports:
- Conditional requests (If-None-Match → 304, If-Modified-Since → 304)
- HEAD method (same headers as GET, no body)
- Range requests (206 Partial Content)
- Error routes (404, 500, 502, 503)
- Per-route headers (ETag, Last-Modified, Cache-Control, Vary)
- POST/PUT/DELETE echo routes
- Chunked transfer encoding simulation
- Binary data passthrough

Routes are configured via static files in /var/www plus hardcoded
special routes for compliance testing.
"""

import hashlib
import http.server
import os
import sys
import time
from email.utils import formatdate, parsedate_to_datetime

TESTDATA_DIR = os.environ.get("COMPLIANCE_TESTDATA", "/var/www")

# Runtime switches for the /vary/flip* routes, toggled via /vary/control.
# Single-threaded server, so a plain dict is enough.
#
# "etag-rev" revises /vary/flip-etag.css's validator WITHOUT changing its
# bytes, which is how a test forces that route to answer a full 200 instead
# of a 304 -- the distinction matters because a 304 that omits `Vary` means
# "unchanged", not "cleared" (RFC 9111 Section 4.3.4).
VARY_FLIP = {"flip": False, "flip-etag": False, "etag-rev": "1"}

# Response-header sidecar control (issue #208): /sidecar/flip.css starts
# with only reproducible headers and can be switched, at runtime, to also
# send a header a cache hit cannot reproduce.
SIDECAR_FLIP = {"custom": False}

# Cookie-request stale controls (test_cookie_request.py): paths that answer
# 500 ("fail") or wait 2.5 s ("slow"), set through /cookie-control.
COOKIE_CONTROL = {"fail": set(), "slow": set()}

# Serve-side sidecar controls (issue #208): a delay on /sidecar/slow.css
# (holds the refresh in flight so a concurrent request takes the coalesced
# stale serve), a 500 on /sidecar/flaky.css (stale-if-error), and the
# Referrer-Policy /sidecar/reval.css sends on its 200 and 304.
SIDECAR_SERVE = {"delay": 0.0, "fail": False, "rp": "no-referrer", "vary304": ""}

# The reproducible security set the sidecar routes send (every name is in
# the sidecar allowlist).
SIDECAR_SECURITY_HEADERS = (
    ("Access-Control-Allow-Origin", "*"),
    ("Content-Security-Policy", "default-src 'self'"),
    ("X-Content-Type-Options", "nosniff"),
    ("Referrer-Policy", "no-referrer"),
)

# Content type mapping
MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".svg": "image/svg+xml",
    ".woff2": "font/woff2",
    ".pdf": "application/pdf",
    ".dat": "application/octet-stream",
    ".txt": "text/plain; charset=utf-8",
}


def etag_for(data: bytes) -> str:
    """Generate a strong ETag from content bytes."""
    h = hashlib.md5(data).hexdigest()[:16]
    return f'"{h}"'


def weak_etag_for(data: bytes) -> str:
    """Generate a weak ETag from content bytes."""
    h = hashlib.md5(data).hexdigest()[:16]
    return f'W/"{h}"'


class ComplianceHandler(http.server.BaseHTTPRequestHandler):
    """HTTP request handler for compliance testing."""

    server_version = "ComplianceOrigin/1.0"

    # Monotonic per-process counter for /authz/stale-must-revalidate: lets
    # tests tell a stale cache serve apart from a fresh origin fetch.
    authz_stale_counter = 0

    # Same idea for /authz/anon-unchanged: a per-fetch body makes a
    # store-side overwrite of the anonymously cached entry observable.
    authz_anon_counter = 0

    # Per-fetch counter for /cookie/* (test_cookie_request.py).
    cookie_counter = 0

    def _guess_type(self, path: str) -> str:
        _, ext = os.path.splitext(path)
        return MIME_TYPES.get(ext.lower(), "application/octet-stream")

    def _read_file(self, path: str) -> bytes | None:
        filepath = os.path.join(TESTDATA_DIR, path.lstrip("/"))
        if os.path.isfile(filepath):
            with open(filepath, "rb") as f:
                return f.read()
        return None

    def _compute_validators(self, content: bytes, path: str):
        """Compute ETag and Last-Modified for content."""
        tag = etag_for(content)
        mtime = formatdate(timeval=time.time(), localtime=False, usegmt=True)
        filepath = os.path.join(TESTDATA_DIR, path.lstrip("/"))
        if os.path.isfile(filepath):
            mtime = formatdate(
                timeval=os.path.getmtime(filepath),
                localtime=False,
                usegmt=True,
            )
        return tag, mtime

    def _check_conditional(self, tag: str, mtime: str) -> bool:
        """Check If-None-Match and If-Modified-Since. Return True if 304."""
        inm = self.headers.get("If-None-Match")
        if inm:
            etags = [e.strip() for e in inm.split(",")]
            if tag in etags or "*" in etags:
                self.send_response(304)
                self.send_header("ETag", tag)
                self.send_header("Date", formatdate(usegmt=True))
                self.send_header("Cache-Control", "public, max-age=3600")
                self.end_headers()
                return True

        ims = self.headers.get("If-Modified-Since")
        if ims and not inm:
            try:
                ims_dt = parsedate_to_datetime(ims)
                mtime_dt = parsedate_to_datetime(mtime)
                if mtime_dt <= ims_dt:
                    self.send_response(304)
                    self.send_header("Last-Modified", mtime)
                    self.send_header("Date", formatdate(usegmt=True))
                    self.send_header("Cache-Control", "public, max-age=3600")
                    self.end_headers()
                    return True
            except (ValueError, TypeError):
                pass
        return False

    def _send_full_response(
        self,
        content: bytes,
        content_type: str,
        tag: str,
        mtime: str,
        send_body: bool = True,
    ):
        """Send a complete 200 response with standard headers."""
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("ETag", tag)
        self.send_header("Last-Modified", mtime)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Date", formatdate(usegmt=True))
        self.send_header("Cache-Control", "public, max-age=3600")
        self.end_headers()
        if send_body:
            self.wfile.write(content)

    def _handle_range(self, content: bytes, content_type: str) -> bool:
        """Handle Range header. Return True if 206 sent."""
        range_header = self.headers.get("Range")
        if not range_header:
            return False

        if_range = self.headers.get("If-Range")
        if if_range:
            tag = etag_for(content)
            if if_range != tag:
                return False  # Fall through to full 200

        if not range_header.startswith("bytes="):
            self.send_error(416, "Unsupported range unit")
            return True

        range_spec = range_header[6:]
        parts = range_spec.split("-")
        if len(parts) != 2:
            self.send_error(416, "Invalid range")
            return True

        total = len(content)
        try:
            if parts[0] == "":
                # Suffix range: -N (last N bytes)
                suffix_len = int(parts[1])
                start = max(0, total - suffix_len)
                end = total - 1
            elif parts[1] == "":
                # Open-ended range: N-
                start = int(parts[0])
                end = total - 1
            else:
                start = int(parts[0])
                end = int(parts[1])
        except ValueError:
            self.send_error(416, "Invalid range values")
            return True

        if start > end or start >= total:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{total}")
            self.end_headers()
            return True

        end = min(end, total - 1)
        partial = content[start : end + 1]

        self.send_response(206)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(partial)))
        self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()
        self.wfile.write(partial)
        return True

    def _serve_special_route(self, path: str, send_body: bool = True) -> bool:
        """Handle special compliance test routes. Return True if handled."""
        # Error routes
        if path == "/error/404":
            self.send_error(404, "Not Found")
            return True
        if path == "/error/403":
            self.send_error(403, "Forbidden")
            return True
        if path == "/error/500":
            self.send_error(500, "Internal Server Error")
            return True
        if path == "/error/502":
            self.send_error(502, "Bad Gateway")
            return True
        if path == "/error/503":
            self.send_error(503, "Service Unavailable")
            return True

        # Redirect routes
        if path == "/redirect/301":
            self.send_response(301)
            self.send_header("Location", "/small.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        if path == "/redirect/302":
            self.send_response(302)
            self.send_header("Location", "/small.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True

        # Cache-Control test routes
        if path == "/cc/no-store":
            content = b"<html><body>no-store content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/no-cache":
            content = b"<html><body>no-cache content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/private":
            content = b"<html><body>private content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "private")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/max-age":
            content = b"<html><body>max-age content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "public, max-age=3600")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/must-revalidate":
            content = b"<html><body>must-revalidate content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "public, must-revalidate")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/max-age-must-revalidate":
            content = b"<html><body>max-age + must-revalidate content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=60, must-revalidate")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/max-age-no-cache":
            content = b"<html><body>max-age + no-cache content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-cache, max-age=60")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/cc/s-maxage":
            content = b"<html><body>s-maxage content</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "public, s-maxage=7200")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # RFC 9111 §3.5 Authorization-gate routes (test_authorization.py).
        # Each sends an ETag (like /cc/max-age-must-revalidate) so the module
        # records the entry; Cache-Control varies which §3.5 permit (if any)
        # the stored response carries.  §3.5 names exactly three permitting
        # directives: public, s-maxage, must-revalidate — bare max-age is
        # NOT a permit.
        authz_cc = {
            "/authz/no-permit": "max-age=3600",
            "/authz/public": "public, max-age=3600",
            "/authz/s-maxage": "max-age=3600, s-maxage=7200",
            "/authz/must-revalidate": "max-age=3600, must-revalidate",
        }
        if path in authz_cc:
            content = f"<html><body>authz {path}</body></html>".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", authz_cc[path])
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        # Cookie-request routes (test_cookie_request.py):
        # /cookie/<kind>/<anything>, so each test can use a fresh cache key.
        # The response names the visitor from the request's `visitor` cookie
        # (as a personalised page would) and carries a per-fetch counter;
        # <kind> picks the Content-Type and Cache-Control (None: no
        # Cache-Control at all).  Kinds marked `fixed` keep one ETag per path
        # and answer a matching If-None-Match with a 304, so a stored copy can
        # be revalidated; the others change their ETag with every fetch.
        cookie_kinds = {
            "plain": ("text/html", "max-age=3600", False),
            "public": ("text/html", "public, max-age=3600", False),
            "s-maxage": ("text/html", "max-age=3600, s-maxage=3600", False),
            "must-revalidate": ("text/html", "max-age=3600, must-revalidate", False),
            "s-maxage-zero": ("text/html", "max-age=3600, s-maxage=0", True),
            "s-maxage-bad": ("text/html", "max-age=3600, s-maxage=abc", True),
            "s-maxage-dup": (
                "text/html",
                "max-age=3600, s-maxage=600, s-maxage=600",
                True,
            ),
            "xhtml": ("application/xhtml+xml", "max-age=3600", False),
            "upper": ("Text/HTML; charset=UTF-8", "max-age=3600", False),
            "json": ("application/json", "max-age=3600", False),
            "js-no-cc": ("application/javascript", None, False),
            "js-max-age": ("application/javascript", "max-age=3600", False),
            # Stale-serve routes: one second of freshness.  `reval` is
            # revalidated with a 304; `flaky` answers 500 and `slow` waits
            # 2.5 s once the test names the path in /cookie-control.
            "reval": ("text/html", "max-age=1", True),
            "flaky": ("text/html", "max-age=1", False),
            "slow": ("text/html", "max-age=2", False),
        }
        if path == "/cookie-control":
            query = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            for mode in ("fail", "slow"):
                if mode in params:
                    COOKIE_CONTROL[mode].add(params[mode])
                if "un" + mode in params:
                    COOKIE_CONTROL[mode].discard(params["un" + mode])
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(body)
            return True
        cookie_parts = path.split("/")
        if (
            len(cookie_parts) == 4
            and cookie_parts[1] == "cookie"
            and cookie_parts[2] in cookie_kinds
        ):
            ctype, cc, fixed_etag = cookie_kinds[cookie_parts[2]]
            if path in COOKIE_CONTROL["slow"]:
                time.sleep(2.5)
            if path in COOKIE_CONTROL["fail"]:
                err = b"origin down"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(err)))
                self.send_header("Date", formatdate(usegmt=True))
                self.end_headers()
                if send_body:
                    self.wfile.write(err)
                return True
            tag = etag_for(path.encode()) if fixed_etag else None
            if tag is not None:
                inm = self.headers.get("If-None-Match")
                if inm and tag in inm:
                    self.send_response(304)
                    self.send_header("ETag", tag)
                    self.send_header("Cache-Control", cc)
                    self.send_header("Date", formatdate(usegmt=True))
                    self.end_headers()
                    return True
            visitor = "nobody"
            for item in (self.headers.get("Cookie") or "").split(";"):
                name, _, value = item.strip().partition("=")
                if name == "visitor" and value:
                    visitor = value
            ComplianceHandler.cookie_counter += 1
            marker = "visitor:%s fetch:%d" % (visitor, ComplianceHandler.cookie_counter)
            if "javascript" in ctype:
                content = ('var cookieTest = "%s";\n' % marker).encode()
            elif "json" in ctype:
                content = ('{"marker": "%s"}' % marker).encode()
            else:
                content = ("<html><body>%s</body></html>" % marker).encode()
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(content)))
            if cc is not None:
                self.send_header("Cache-Control", cc)
            self.send_header("ETag", tag or etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/authz/anon-unchanged":
            # Per-fetch counter body: every origin fetch yields a UNIQUE
            # body, so the authenticated pass-through's response is
            # distinguishable from every other fetch — if it were recorded
            # over the anonymously stored entry (a store-side §3.5
            # regression), the next anonymous HIT would serve exactly that
            # body and test_authorization.py would catch it.  A constant
            # body could not observe the overwrite.  No permit directive:
            # anonymous storage needs none.
            ComplianceHandler.authz_anon_counter += 1
            content = (
                "<html><body>authz-anon-counter:%d</body></html>"
                % ComplianceHandler.authz_anon_counter
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=3600")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True
        if path == "/authz/stale-must-revalidate":
            # Short freshness so the test can observe staleness; the body
            # carries a monotonic counter so a stale cache serve (old
            # counter) is distinguishable from a genuine origin fetch (the
            # counter only advances when THIS handler runs).  The ETag
            # tracks the content, so a revalidation can never 304-restamp
            # the stale entry back to fresh mid-test.
            ComplianceHandler.authz_stale_counter += 1
            content = (
                "<html><body>authz-counter:%d</body></html>"
                % ComplianceHandler.authz_stale_counter
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=5, must-revalidate")
            self.send_header("ETag", etag_for(content))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Vary-flip control: turn the origin's own Accept negotiation on or
        # off at runtime, so a test can exercise a URL that was already
        # cached (and optimized) BEFORE its origin started declaring
        # `Vary: Accept` -- the upgrade population, which a route that has
        # always declared it cannot reach.
        if path == "/vary/control":
            query = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            for key in ("flip", "flip-etag"):
                if key in params:
                    VARY_FLIP[key] = params[key] in ("1", "on", "true")
            if "etag-rev" in params:
                VARY_FLIP["etag-rev"] = params["etag-rev"]
            body = ("%s\n" % VARY_FLIP).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(body)
            return True

        # Flip route WITHOUT validators: a stale entry here is re-fetched in
        # full (200), which is the path that carries the origin-refresh
        # signal.  Short max-age so the transition happens in test time.
        if path == "/vary/flip.css":
            content = b"body { color: red; }"
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=1")
            if VARY_FLIP["flip"]:
                self.send_header("Vary", "Accept")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Flip route WITH an ETag: a stale entry here is revalidated and
        # answered 304, which is the conditional-revalidation path.  The 304
        # carries the current `Vary` so a cache can re-decide from it
        # (RFC 9111 Section 4.3.4).
        if path == "/vary/flip-etag.css":
            content = b"body { color: blue; }"
            tag = etag_for(content + VARY_FLIP["etag-rev"].encode())
            inm = self.headers.get("If-None-Match")
            if inm and tag in inm:
                self.send_response(304)
                self.send_header("ETag", tag)
                self.send_header("Cache-Control", "max-age=1")
                if VARY_FLIP["flip-etag"]:
                    self.send_header("Vary", "Accept")
                self.send_header("Date", formatdate(usegmt=True))
                self.end_headers()
                return True
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("ETag", tag)
            self.send_header("Cache-Control", "max-age=1")
            if VARY_FLIP["flip-etag"]:
                self.send_header("Vary", "Accept")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Short-lived image WITHOUT validators, so an expired entry is
        # re-fetched in full rather than revalidated. The re-fetch path is
        # where the serving stage used to leave its own `Vary` on the
        # response before handing off to the origin -- and for an image that
        # value names `Sec-CH-DPR`, which the store-side allowlist does not
        # admit, so the response read its own header back and was refused
        # storage. Body is served from testdata so it is a real image.
        if path == "/stale-image.jpg":
            src = os.path.join(TESTDATA_DIR, "1x1.jpg")
            try:
                with open(src, "rb") as fh:
                    content = fh.read()
            except OSError:
                self.send_error(404)
                return True
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(content)))
            # max-age=2, not 1: the serve-stale grace window is
            # min(10, max_age), so a one-second lifetime leaves the test's
            # poll interval sitting on the boundary between "expired" and
            # "inside grace".  Two seconds buys margin without changing what
            # is being tested.
            self.send_header("Cache-Control", "max-age=2")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # HTML with NO Cache-Control at all. With the default html_max_age of
        # 0 this lands in the serve-with-no-cache freshness arm, which is a
        # cache-HIT serve path distinct from the ordinary fresh-hit one -- and
        # is the DEFAULT arm for HTML from an origin that sets no
        # Cache-Control, i.e. a very ordinary site.
        if path == "/no-cc.html":
            content = (
                b"<!DOCTYPE html><html><head><title>no cc</title></head>"
                b"<body><h1>no cache-control</h1></body></html>"
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Request-field negotiation: /vary/negotiated/<field>/<anything>.css
        # answers with `Vary: <field>` and echoes that request field's value
        # in the body, so a test can tell whose answer a client was given.
        # Cacheable for ten minutes, so only the Vary keeps it out of a
        # shared cache.  <field> is one of the names below.
        if path.startswith("/vary/negotiated/"):
            fields = {
                "accept": "Accept",
                "user-agent": "User-Agent",
                "save-data": "Save-Data",
                "origin": "Origin",
                "accept-encoding": "Accept-Encoding",
            }
            parts = path.split("/")
            field = fields.get(parts[3] if len(parts) > 4 else "")
            if field is None:
                return False
            value = self.headers.get(field, "")
            content = ("/* %s=%s */\n" % (field, value)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "max-age=600")
            self.send_header("Vary", field)
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Vary test route
        if path == "/vary/accept":
            content = b"body { color: red; }"
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Vary", "Accept")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Weak ETag route
        if path == "/weak-etag":
            content = b"body { color: blue; }"
            tag = weak_etag_for(content)
            # Check conditional
            inm = self.headers.get("If-None-Match")
            if inm:
                etags = [e.strip() for e in inm.split(",")]
                if tag in etags or "*" in etags:
                    self.send_response(304)
                    self.send_header("ETag", tag)
                    self.send_header("Date", formatdate(usegmt=True))
                    self.end_headers()
                    return True
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("ETag", tag)
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Chunked transfer encoding test
        if path == "/chunked/css":
            content = self._read_file("/large.css")
            if content is None:
                self.send_error(404)
                return True
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                # Send in chunks
                chunk_size = 8192
                for i in range(0, len(content), chunk_size):
                    chunk = content[i : i + chunk_size]
                    self.wfile.write(f"{len(chunk):x}\r\n".encode())
                    self.wfile.write(chunk)
                    self.wfile.write(b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            return True

        # Hop-by-hop header test — origin sends hop-by-hop headers
        # that a proxy should strip
        if path == "/hop-by-hop":
            content = b"<html><body>hop-by-hop test</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Connection", "keep-alive")
            self.send_header("Keep-Alive", "timeout=5")
            self.send_header("Proxy-Authenticate", "Basic")
            self.send_header("Trailer", "X-Checksum")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Pragma: no-cache backward compat test
        if path == "/pragma/no-cache":
            content = b"<html><body>pragma no-cache</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Pragma", "no-cache")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Timeout simulation (slow response)
        if path == "/slow":
            time.sleep(35)  # Longer than typical proxy timeout
            content = b"slow response"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Response-header sidecar routes (issue #208).  Each sends a
        # cacheable stylesheet/page plus a header set chosen to land in one
        # verdict of the store-side gate:
        #   /sidecar/asset.css   only reproducible headers (security set)
        #   /sidecar/custom.css  + X-Request-Id (cannot be reproduced)
        #   /sidecar/nonce.html  CSP with a per-response nonce
        #   /sidecar/vary-origin.css  Vary: Origin (never stored)
        #   /sidecar/flip.css    short-lived; X-Custom-Foo switched on via
        #                        /sidecar/control?custom=on
        # Serve-side sidecar routes (issue #208).  Every one sends the
        # security set plus `Vary: Accept-Encoding`, so a cache hit has
        # stored lines to replay:
        #   /sidecar/parity.css, /sidecar/page.html  long-lived, ETag
        #   /sidecar/heal.css    long-lived; the test removes its stored block
        #   /sidecar/slow.css    max-age=2, waits SIDECAR_SERVE["delay"]
        #   /sidecar/flaky.css   max-age=1, a 500 with its own policy headers
        #                        while SIDECAR_SERVE["fail"] is set
        #   /sidecar/reval.css   max-age=1 + ETag: revalidated with a 304
        #                        that carries the current Referrer-Policy
        # Controlled through /sidecar/serve-control?delay=&fail=&rp=&vary304=
        # (vary304 adds a Vary line to the 304 of /sidecar/reval.css).
        if path == "/sidecar/serve-control":
            query = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            if "delay" in params:
                SIDECAR_SERVE["delay"] = float(params["delay"])
            if "fail" in params:
                SIDECAR_SERVE["fail"] = params["fail"] in ("1", "on", "true")
            if "rp" in params:
                SIDECAR_SERVE["rp"] = params["rp"]
            if "vary304" in params:
                SIDECAR_SERVE["vary304"] = params["vary304"]
            body = f"{SIDECAR_SERVE}\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(body)
            return True
        if path in (
            "/sidecar/parity.css",
            "/sidecar/page.html",
            "/sidecar/heal.css",
            "/sidecar/slow.css",
            "/sidecar/flaky.css",
            "/sidecar/reval.css",
        ):
            is_html = path.endswith(".html")
            if is_html:
                content = (
                    b"<!DOCTYPE html>\n<html>\n  <head>\n"
                    b"    <title>sidecar page</title>\n"
                    b"    <!-- a comment the optimizer removes -->\n"
                    b"    <style>\n      body   {   color :  green ;  }\n"
                    b"      p      {   margin : 0px ;  }\n    </style>\n"
                    b"  </head>\n  <body>\n"
                    + b"    <p>  headers sidecar   parity   </p>\n" * 12
                    + b'    <img src="/1x1.jpg" alt="" width="640" height="480">\n' * 3
                    + b"  </body>\n</html>\n"
                )
            else:
                content = (
                    f"/* {path}: a stylesheet with room to minify */\n"
                    + "body   {   color :  green ;   margin : 0px ;  }\n" * 12
                ).encode()
            if path == "/sidecar/slow.css" and SIDECAR_SERVE["delay"] > 0:
                time.sleep(SIDECAR_SERVE["delay"])
            if path == "/sidecar/flaky.css" and SIDECAR_SERVE["fail"]:
                err = b"origin down"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(err)))
                self.send_header("Content-Security-Policy", "default-src 'none'")
                self.send_header("Access-Control-Allow-Origin", "https://error.example")
                self.send_header("Referrer-Policy", "unsafe-url")
                self.send_header("Date", formatdate(usegmt=True))
                self.end_headers()
                if send_body:
                    self.wfile.write(err)
                return True
            rp = SIDECAR_SERVE["rp"] if path == "/sidecar/reval.css" else None
            tag = etag_for(content)
            if path == "/sidecar/reval.css":
                inm = self.headers.get("If-None-Match")
                if inm and tag in inm:
                    self.send_response(304)
                    self.send_header("ETag", tag)
                    self.send_header("Cache-Control", "public, max-age=1")
                    self.send_header("Referrer-Policy", rp)
                    if SIDECAR_SERVE["vary304"]:
                        self.send_header("Vary", SIDECAR_SERVE["vary304"])
                    self.send_header("Date", formatdate(usegmt=True))
                    self.end_headers()
                    return True
            self.send_response(200)
            self.send_header("Content-Type", "text/html" if is_html else "text/css")
            self.send_header("Content-Length", str(len(content)))
            if path in ("/sidecar/flaky.css", "/sidecar/reval.css"):
                self.send_header("Cache-Control", "public, max-age=1")
            elif path == "/sidecar/slow.css":
                self.send_header("Cache-Control", "public, max-age=2")
            else:
                self.send_header("Cache-Control", "public, max-age=600")
            if path not in ("/sidecar/slow.css", "/sidecar/flaky.css"):
                self.send_header("ETag", tag)
            for name, value in SIDECAR_SECURITY_HEADERS:
                if name == "Referrer-Policy" and rp is not None:
                    value = rp
                self.send_header(name, value)
            self.send_header("Vary", "Accept-Encoding")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        if path == "/sidecar/control":
            query = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            if "custom" in params:
                SIDECAR_FLIP["custom"] = params["custom"] in ("1", "on", "true")
            body = ("%s\n" % SIDECAR_FLIP).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(body)
            return True
        if path in (
            "/sidecar/asset.css",
            "/sidecar/custom.css",
            "/sidecar/nonce.html",
            "/sidecar/vary-origin.css",
            "/sidecar/flip.css",
        ):
            is_html = path.endswith(".html")
            content = (
                b"<html><body><p>sidecar</p></body></html>"
                if is_html
                else ("/* %s */ body { color: green; }" % path).encode()
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html" if is_html else "text/css")
            self.send_header("Content-Length", str(len(content)))
            if path == "/sidecar/flip.css":
                self.send_header("Cache-Control", "max-age=1")
            else:
                self.send_header("Cache-Control", "public, max-age=600")
                self.send_header("ETag", etag_for(content))
            if path == "/sidecar/nonce.html":
                self.send_header("Content-Security-Policy", "script-src 'nonce-abc123'")
            else:
                for name, value in SIDECAR_SECURITY_HEADERS:
                    self.send_header(name, value)
            if path == "/sidecar/custom.css":
                self.send_header("X-Request-Id", "req-8f3a")
            if path == "/sidecar/vary-origin.css":
                self.send_header("Vary", "Origin")
            if path == "/sidecar/flip.css" and SIDECAR_FLIP["custom"]:
                self.send_header("X-Custom-Foo", "bar")
            self.send_header("Date", formatdate(usegmt=True))
            self.end_headers()
            if send_body:
                self.wfile.write(content)
            return True

        # Echo route for POST/PUT/DELETE methods
        if path == "/echo":
            return False  # Handled by do_POST, do_PUT, do_DELETE

        return False

    def do_GET(self):
        path = self.path.split("?")[0]  # Strip query string

        # Check special routes first
        if self._serve_special_route(path, send_body=True):
            return

        # Serve static file
        content = self._read_file(path)
        if content is None:
            self.send_error(404, f"File not found: {path}")
            return

        content_type = self._guess_type(path)
        tag, mtime = self._compute_validators(content, path)

        # Handle conditional requests (before sending any response)
        if self._check_conditional(tag, mtime):
            return

        # Handle range requests
        if self._handle_range(content, content_type):
            return

        # Send full 200 response
        self._send_full_response(content, content_type, tag, mtime)

    def do_HEAD(self):
        path = self.path.split("?")[0]

        # Check special routes (no body)
        if self._serve_special_route(path, send_body=False):
            return

        content = self._read_file(path)
        if content is None:
            self.send_error(404, f"File not found: {path}")
            return

        content_type = self._guess_type(path)
        tag, mtime = self._compute_validators(content, path)

        # Send full response without body
        self._send_full_response(content, content_type, tag, mtime, send_body=False)

    def do_POST(self):
        self._handle_echo("POST")

    def do_PUT(self):
        self._handle_echo("PUT")

    def do_DELETE(self):
        self._handle_echo("DELETE")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS")
        self.send_header("Content-Length", "0")
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()

    def _handle_echo(self, method: str):
        """Echo back the request method and body."""
        content_length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_length) if content_length > 0 else b""

        response = f"Method: {method}\nPath: {self.path}\nBody-Length: {len(body)}\n"
        response_bytes = response.encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(response_bytes)))
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()
        self.wfile.write(response_bytes)

    def log_message(self, format, *args):
        """Log to stderr for Docker container visibility."""
        sys.stderr.write(f"[origin] {self.address_string()} - {format % args}\n")


def main():
    port = int(os.environ.get("PORT", "8081"))
    bind = os.environ.get("BIND", "0.0.0.0")
    server = http.server.HTTPServer((bind, port), ComplianceHandler)
    print(f"Compliance origin listening on {bind}:{port}", flush=True)
    print(f"Serving files from {TESTDATA_DIR}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
