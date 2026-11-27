#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Origin-level self-test of the adversarial origin: every endpoint answers
with the status, headers and body markers its row promises."""

import email.utils
import gzip
import hashlib
import http.client
import json
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import adversarial_origin as ao  # noqa: E402

STATIC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

COOKIE_A = "sid=alpha"
COOKIE_B = "sid=bravo"
AUTH_A = "Bearer token-alpha"
USER_A = "user-alpha"


class Origin:
    def __init__(self, **kw):
        self.server = ao.make_server(0, "127.0.0.1", **kw)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def request(self, method, path, headers=None, body=None, timeout=10):
        conn = http.client.HTTPConnection("127.0.0.1", self.port,
                                          timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            data = resp.read()
            return resp.status, resp.headers, data
        finally:
            conn.close()

    def get(self, path, headers=None):
        return self.request("GET", path, headers)

    def requests(self):
        return json.loads(self.get("/_control/requests")[2])


class AdversarialOriginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.o = Origin(big_bytes=256 * 1024, slow_timeout=10.0)

    @classmethod
    def tearDownClass(cls):
        cls.o.close()

    def setUp(self):
        self.o.request("POST", "/_control/reset")

    def assertCommon(self, headers):
        self.assertEqual(headers.get_all("X-Robots-Tag"), ["noindex"])
        self.assertIsNotNone(headers.get("Date"))

    def assertPage(self, path, status=200, cc=None, headers=None,
                   content_type="text/html; charset=utf-8"):
        st, h, body = self.o.get(path, headers)
        self.assertEqual(st, status, path)
        self.assertCommon(h)
        self.assertEqual(h.get("Content-Type"), content_type, path)
        self.assertEqual(h.get("Cache-Control"), cc, path)
        token, cred = ao.parse_markers(body)
        self.assertRegex(token or "", r"^[0-9A-F]{32}$", path)
        self.assertRegex(cred or "", r"^[0-9A-F]{16}$", path)
        return h, body

    # -- markers and credential hash -------------------------------------

    def test_credential_hash_definition(self):
        self.assertEqual(ao.ANONYMOUS_CRED_HASH,
                         hashlib.sha256(b"---").hexdigest()[:16].upper())
        self.assertEqual(
            ao.credential_hash("a=b", None, ""),
            hashlib.sha256(b"+3:a=b-+0:").hexdigest()[:16].upper())
        self.assertNotEqual(ao.credential_hash("", None, None),
                            ao.credential_hash(None, None, None))
        # Length prefixes keep the fields from sliding into each other.
        self.assertNotEqual(ao.credential_hash("ab", "c", None),
                            ao.credential_hash("a", "bc", None))

    def test_token_unique_and_cred_per_identity(self):
        identities = {
            "anon": ({}, ao.ANONYMOUS_CRED_HASH),
            "cookie-a": ({"Cookie": COOKIE_A},
                         ao.credential_hash(COOKIE_A, None, None)),
            "cookie-b": ({"Cookie": COOKIE_B},
                         ao.credential_hash(COOKIE_B, None, None)),
            "auth-a": ({"Authorization": AUTH_A},
                       ao.credential_hash(None, AUTH_A, None)),
            "user-a": ({"X-Test-User": USER_A},
                       ao.credential_hash(None, None, USER_A)),
            "empty-cookie": ({"Cookie": ""},
                             ao.credential_hash("", None, None)),
        }
        self.assertEqual(len({v[1] for v in identities.values()}),
                         len(identities))
        tokens = set()
        for path in ("/residual", "/ga-page", "/me", "/public-me"):
            for name, (hdrs, expected) in identities.items():
                st, h, body = self.o.get(path, hdrs)
                token, cred = ao.parse_markers(body)
                self.assertEqual(cred, expected, (path, name))
                tokens.add(token)
        self.assertEqual(len(tokens), 4 * len(identities))
        # Bearer needs Authorization; its cred is the Authorization one.
        st, h, body = self.o.get("/bearer", {"Authorization": AUTH_A})
        self.assertEqual(ao.parse_markers(body)[1],
                         identities["auth-a"][1])

    def test_markers_parse_bytes_and_missing(self):
        self.assertEqual(ao.parse_markers(b"nothing"), (None, None))
        body = ("origin-token:%s:origin-token cred:%s:cred"
                % ("A" * 32, "B" * 16)).encode()
        self.assertEqual(ao.parse_markers(body), ("A" * 32, "B" * 16))

    # -- caching shapes ---------------------------------------------------

    def test_fresh_counter(self):
        h, body = self.assertPage("/fresh", cc="max-age=30")
        self.assertEqual(ao.parse_counter(body), 1)
        h, body = self.assertPage("/fresh", cc="max-age=30")
        self.assertEqual(ao.parse_counter(body), 2)
        self.assertEqual(self.o.requests().get("/fresh"), 2)

    def test_cache_control_rows(self):
        rows = {
            "/short": "max-age=8",
            "/cap": "max-age=3600",
            "/smax": "max-age=5, s-maxage=60",
            "/ext": "max-age=30, stale-while-revalidate=600, immutable",
            "/agecheck": "max-age=30",
            "/nocache": "no-cache",
            "/zero": "max-age=0",
            "/mr": "max-age=10, must-revalidate",
            "/swr": "max-age=2, stale-while-revalidate=600, "
                    "stale-if-error=600",
            "/me": "max-age=60",
            "/public-me": "public, max-age=60",
            "/ga-page": "max-age=60",
            "/echo-ga": "max-age=60",
            "/reflect-host": "max-age=60",
            "/csrf": "max-age=60",
            "/inline-nonce": "max-age=60",
            "/ua-variant": "max-age=60",
            "/hdr": "max-age=60",
            "/subst": "max-age=60",
            "/notransform": "no-transform, max-age=30",
            "/residual": "max-age=60",
        }
        for path, cc in rows.items():
            h, _ = self.assertPage(path, cc=cc)
            self.assertIsNone(h.get("Vary"), path)
            self.assertIsNone(h.get("Set-Cookie"), path)
            self.assertIsNone(h.get("ETag"), path)
            self.assertIsNone(h.get("Content-Encoding"), path)
            if path != "/agecheck":
                self.assertIsNone(h.get("Age"), path)
        h, _ = self.assertPage("/agecheck", cc="max-age=30")
        self.assertEqual(h.get_all("Age"), ["20"])

    def test_expires(self):
        st, h, body = self.o.get("/expires")
        self.assertEqual(st, 200)
        self.assertCommon(h)
        self.assertIsNone(h.get("Cache-Control"))
        date = email.utils.parsedate_to_datetime(h["Date"])
        exp = email.utils.parsedate_to_datetime(h["Expires"])
        self.assertEqual((exp - date).total_seconds(), 60)

    def test_swr_forced_failure(self):
        st, h, _ = self.o.request("POST",
                                  "/_control/fail?path=/swr&status=500")
        self.assertEqual(st, 200)
        self.assertEqual(h.get("Cache-Control"), "no-store")
        self.assertCommon(h)
        st, h, body = self.o.get("/swr")
        self.assertEqual(st, 500)
        self.assertCommon(h)
        self.assertIsNotNone(ao.parse_markers(body)[0])
        self.o.request("POST", "/_control/fail?path=/swr&status=0")
        self.assertEqual(self.o.get("/swr")[0], 200)
        st, _, _ = self.o.request("POST", "/_control/fail?path=swr")
        self.assertEqual(st, 400)

    def test_personalised_bodies(self):
        _, body = self.assertPage("/me", cc="max-age=60",
                                  headers={"Cookie": COOKIE_A})
        self.assertIn(b"Hello, sid=alpha.", body)
        _, body = self.assertPage("/public-me", cc="public, max-age=60",
                                  headers={"Cookie": COOKIE_B})
        self.assertIn(b"Hello, sid=bravo.", body)
        _, body = self.assertPage("/residual", cc="max-age=60",
                                  headers={"X-Test-User": USER_A})
        self.assertIn(b"Hello, user-alpha.", body)
        _, body = self.assertPage("/ga-page", cc="max-age=60",
                                  headers={"Cookie": "_ga=GA1.1.1.1; "
                                                     "_gid=GA1.1.2.2"})
        self.assertNotIn(b"GA1.", body)

    def test_bearer(self):
        for path, cc in (("/bearer", "public, max-age=60"),
                         ("/bearer-mr", "max-age=60, must-revalidate")):
            st, h, body = self.o.get(path)
            self.assertEqual(st, 401, path)
            self.assertCommon(h)
            self.assertIn("Bearer", h.get("WWW-Authenticate"))
            self.assertPage(path, cc=cc, headers={"Authorization": AUTH_A})

    def test_auth(self):
        st, h, _ = self.o.get("/auth")
        self.assertEqual(st, 401)
        self.assertCommon(h)
        self.assertTrue(h.get("WWW-Authenticate").startswith("Basic "))
        self.assertPage("/auth", cc="max-age=60",
                        headers={"Authorization": "Basic dXNlcjpwYXNz"})

    def test_echo_ga(self):
        _, body = self.assertPage(
            "/echo-ga", cc="max-age=60",
            headers={"Cookie": "_gid=GA1.1.9.9; _ga=GA1.1.123.456"})
        self.assertIn(b"ga:GA1.1.123.456:ga", body)

    def test_reflect_host(self):
        _, body = self.assertPage(
            "/reflect-host", cc="max-age=60",
            headers={"X-Forwarded-Host": "evil.example.test"})
        self.assertIn(b'href="https://evil.example.test/fresh"', body)

    def test_never_store_shapes(self):
        h, _ = self.assertPage("/setcookie", cc="max-age=60")
        self.assertRegex(h.get("Set-Cookie"), r"^session=[0-9a-f]{16};")
        h, _ = self.assertPage("/vary-cookie", cc="max-age=60")
        self.assertEqual(h.get("Vary"), "Cookie")
        h, _ = self.assertPage("/vary-ua", cc="max-age=60")
        self.assertEqual(h.get("Vary"), "User-Agent")
        _, body = self.assertPage("/csrf", cc="max-age=60")
        self.assertRegex(body, rb'name="csrf_token" value="[0-9a-f]{32}"')
        h, body = self.assertPage("/nonce", cc="max-age=60")
        nonce = h.get("Content-Security-Policy").split("'nonce-")[1][:-1]
        self.assertIn(('<script nonce="%s">' % nonce).encode(), body)

    def test_gzip(self):
        st, h, raw = self.o.get("/gzip")
        self.assertEqual(st, 200)
        self.assertCommon(h)
        self.assertEqual(h.get("Content-Encoding"), "gzip")
        self.assertEqual(h.get("Cache-Control"), "max-age=60")
        self.assertEqual(int(h.get("Content-Length")), len(raw))
        token, cred = ao.parse_markers(gzip.decompress(raw))
        self.assertIsNotNone(token)
        self.assertEqual(cred, ao.ANONYMOUS_CRED_HASH)

    def test_csrf_late(self):
        _, body = self.assertPage("/csrf-late", cc="max-age=60")
        self.assertGreater(body.index(b'name="csrf_token"'), 64 * 1024)

    def test_inline_nonce(self):
        h, body = self.assertPage("/inline-nonce", cc="max-age=60")
        self.assertIsNone(h.get("Content-Security-Policy"))
        self.assertRegex(body, rb'var requestNonce = "[0-9a-f]{24}"')
        self.assertNotIn(b"<script nonce", body)

    def test_ua_variant(self):
        chrome = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                  "AppleWebKit/537.36 (KHTML, like Gecko) "
                  "Chrome/131.0.0.0 Safari/537.36")
        bot = ("Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; "
               "compatible; GPTBot/1.2; +https://openai.com/gptbot)")
        h, body = self.assertPage("/ua-variant", cc="max-age=60",
                                  headers={"User-Agent": chrome})
        self.assertIn(b"variant:browser:variant", body)
        h, body = self.assertPage("/ua-variant", cc="max-age=60",
                                  headers={"User-Agent": bot})
        self.assertIn(b"variant:agent:variant", body)
        self.assertIsNone(h.get("Vary"))
        _, body = self.assertPage("/ua-variant", cc="max-age=60",
                                  headers={"User-Agent": "NotGPTBot/1.0"})
        self.assertIn(b"variant:browser:variant", body)

    def test_subst(self):
        _, body = self.assertPage("/subst", cc="max-age=60")
        self.assertIn(b"a" * ao.SUBST_A_COUNT, body)

    def test_hdr_has_no_x_t(self):
        h, _ = self.assertPage("/hdr", cc="max-age=60")
        self.assertIsNone(h.get("X-T"))

    def test_validators(self):
        h, body = self.assertPage("/validators", cc="max-age=30")
        etag, lm = h.get("ETag"), h.get("Last-Modified")
        self.assertEqual(etag, ao.VALIDATOR_ETAG)
        self.assertEqual(lm, ao.VALIDATOR_LAST_MODIFIED)
        for cond in ({"If-None-Match": etag},
                     {"If-None-Match": '"other", ' + etag},
                     {"If-None-Match": "*"},
                     {"If-Modified-Since": lm}):
            st, h2, body2 = self.o.get("/validators", cond)
            self.assertEqual(st, 304, cond)
            self.assertCommon(h2)
            self.assertEqual(body2, b"")
            self.assertEqual(h2.get("ETag"), etag)
        for cond in ({"If-None-Match": '"other"'},
                     {"If-Modified-Since": "Sun, 01 Mar 2026 10:00:00 GMT"},
                     {"If-None-Match": '"other"', "If-Modified-Since": lm}):
            st, _, _ = self.o.get("/validators", cond)
            self.assertEqual(st, 200, cond)

    def test_notransform(self):
        self.assertPage("/notransform", cc="no-transform, max-age=30")

    def test_counter_json(self):
        st, h, body = self.o.get("/counter-json")
        self.assertEqual(st, 200)
        self.assertCommon(h)
        self.assertEqual(h.get("Content-Type"), "application/json")
        self.assertEqual(h.get("Cache-Control"), "max-age=30")
        doc = json.loads(body)
        self.assertEqual(ao.parse_counter(doc["counter"]), 1)
        self.assertEqual(ao.parse_markers(body)[1], ao.ANONYMOUS_CRED_HASH)

    def test_big(self):
        h, body = self.assertPage("/big", cc="max-age=60")
        self.assertGreater(len(body), 256 * 1024)
        self.assertEqual(int(h.get("Content-Length")), len(body))

    def test_big_default_size(self):
        o = Origin()
        try:
            st, h, body = o.get("/big")
            self.assertEqual(st, 200)
            self.assertGreater(len(body), ao.DEFAULT_BIG_BYTES)
        finally:
            o.close()

    def test_head(self):
        st, h, body = self.o.request("HEAD", "/fresh")
        self.assertEqual(st, 200)
        self.assertCommon(h)
        self.assertEqual(body, b"")
        self.assertGreater(int(h.get("Content-Length")), 0)

    # -- slow -------------------------------------------------------------

    def _wait_waiting(self, origin, n):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            doc = json.loads(origin.get("/_control/waiting?path=/slow")[2])
            if doc["waiting"] == n:
                return
            time.sleep(0.02)
        self.fail("waiting never reached %d" % n)

    def test_slow_holds_until_released(self):
        result = {}

        def fetch():
            result["r"] = self.o.get("/slow")

        t = threading.Thread(target=fetch)
        t.start()
        self._wait_waiting(self.o, 1)
        t.join(0.3)
        self.assertTrue(t.is_alive())
        st, h, _ = self.o.request("POST", "/_control/release?path=/slow")
        self.assertEqual(st, 200)
        self.assertEqual(h.get("Cache-Control"), "no-store")
        t.join(5)
        self.assertFalse(t.is_alive())
        st, h, body = result["r"]
        self.assertEqual(st, 200)
        self.assertCommon(h)
        self.assertEqual(h.get("Cache-Control"), "max-age=30")
        self.assertEqual(h.get("X-Origin-Slow"), "released")
        self.assertIsNotNone(ao.parse_markers(body)[0])
        self._wait_waiting(self.o, 0)

    def test_slow_times_out(self):
        o = Origin(slow_timeout=0.3)
        try:
            start = time.monotonic()
            st, h, _ = o.get("/slow")
            self.assertEqual(st, 200)
            self.assertEqual(h.get("X-Origin-Slow"), "timeout")
            self.assertLess(time.monotonic() - start, 5)
        finally:
            o.close()

    # -- unsafe methods ---------------------------------------------------

    def test_unsafe_fresh(self):
        for method, status in (("POST", 200), ("PUT", 200),
                               ("PATCH", 200), ("DELETE", 204)):
            st, h, body = self.o.request(method, "/fresh", body=b"x=1",
                                         headers={"Content-Length": "3"})
            self.assertEqual(st, status, method)
            self.assertCommon(h)
            if status == 204:
                self.assertEqual(body, b"")
        counts = self.o.requests()
        for method in ("POST", "PUT", "PATCH", "DELETE"):
            self.assertEqual(counts.get("%s /fresh" % method), 1)
        self.assertNotIn("/fresh", counts)

    def test_post_item(self):
        st, h, _ = self.o.request("POST", "/item", body=b"")
        self.assertEqual(st, 303)
        self.assertCommon(h)
        self.assertEqual(h.get("Location"), "/fresh")
        self.assertEqual(h.get("Content-Location"), "/item")

    def test_post_fail(self):
        st, h, _ = self.o.request("POST", "/fail", body=b"")
        self.assertEqual(st, 500)
        self.assertCommon(h)

    def test_unsafe_elsewhere_405(self):
        st, h, _ = self.o.request("POST", "/me", body=b"")
        self.assertEqual(st, 405)
        self.assertCommon(h)

    # -- control and errors -----------------------------------------------

    def test_counters_and_reset(self):
        self.o.get("/me")
        self.o.get("/me")
        self.o.get("/zero?x=1")
        counts = self.o.requests()
        self.assertEqual(counts, {"/me": 2, "/zero": 1})
        st, h, _ = self.o.get("/_control/requests")
        self.assertEqual(h.get("Cache-Control"), "no-store")
        self.assertCommon(h)
        st, h, _ = self.o.request("POST", "/_control/reset")
        self.assertEqual(st, 200)
        self.assertEqual(h.get("Cache-Control"), "no-store")
        self.assertCommon(h)
        self.assertEqual(self.o.requests(), {})

    def test_not_found(self):
        st, h, _ = self.o.get("/nope")
        self.assertEqual(st, 404)
        self.assertCommon(h)
        self.assertEqual(h.get("Cache-Control"), "no-store")
        st, h, _ = self.o.get("/_control/nope")
        self.assertEqual(st, 404)
        self.assertCommon(h)

    def test_every_endpoint_has_robots_and_markers(self):
        hdrs = {"Authorization": AUTH_A}
        for path in ao.GET_PATHS:
            if path == "/slow":
                continue
            st, h, body = self.o.get(path, hdrs)
            self.assertCommon(h)
            if h.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
            token, cred = ao.parse_markers(body)
            self.assertIsNotNone(token, path)
            self.assertEqual(cred, ao.credential_hash(None, AUTH_A, None),
                             path)


class StaticFilesTest(unittest.TestCase):
    def test_fragment_is_literal_directive(self):
        with open(os.path.join(STATIC, "ssi-fragment.txt")) as f:
            text = f.read()
        self.assertIn('<!--#echo var="DATE_LOCAL" -->', text)
        self.assertIn("literal:", text)

    def test_shtml_includes_fragment(self):
        with open(os.path.join(STATIC, "ssi-literal.shtml")) as f:
            text = f.read()
        self.assertIn('<!--#include virtual="ssi-fragment.txt" -->', text)
        self.assertIn('<!--#echo var="DATE_GMT" -->', text)

    def test_static_page(self):
        with open(os.path.join(STATIC, "static.html")) as f:
            text = f.read()
        self.assertIn("<h1>", text)
        self.assertGreater(len(text), 1500)


if __name__ == "__main__":
    unittest.main()
