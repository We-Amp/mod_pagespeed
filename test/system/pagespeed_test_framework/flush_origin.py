#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Slow, flushing HTTP origin for the pytest system-test lanes.

Stands in for the PHP pages the bash suite used to exercise flush handling
(install/mod_pagespeed_test/{slow_flushing_html_response,php_withflush,
php_withoutflush,php_gzip}.php and image_rewrite_with_flush/*.php). The
Apache lane proxies /mod_pagespeed_test/flush_origin/ here with
"ProxyPass ... flushpackets=on", so every chunk written and flushed below
reaches mod_pagespeed as a FLUSH bucket -- the event mod_php's flush()
produced.

Routes (the path after /mod_pagespeed_test/flush_origin/):
  slow_flushing_html_response  <html><head></head><body>, then five
                               "<p>foo</p><div>bar:N</div>" chunks, each
                               followed by a pause (1 s; ?delay=S overrides),
                               then </body></html>
  withoutflush                 one Content-Length response,
                               X-My-PHP-Header: without_flush
  withflush                    X-My-PHP-Header: with_flush; the page head is
                               flushed as its own chunk before the text
  image_rewrite_with_flush     flush inside <li data-thumb="a.jpg">
  split_head                   a flush inside <head>, after the charset meta
                               and before anything else of the head; with
                               ?canonical=wrong the first window also holds
                               two conflicting canonical links, which the
                               rule_packs filter can no longer remove; the
                               response carries a Link header with a
                               canonical entry, which the flushed headers
                               keep
  link_header                  one unflushed HTML response without a
                               canonical, whose Link header has a preload, a
                               stale canonical and a preconnect entry
  link_header_late_body        link_header, but the response headers go out
                               alone and the body follows 0.3 s later, as when
                               an origin writes its headers before it renders
                               the page
  plain_text_html              text/html whose body is plain text, streamed
                               in flushed chunks 0.5 s apart
  early_flush_empty_body       headers flushed at once (Link, Set-Cookie and
                               Cache-Control among them), then an empty chunked
                               body 0.3 s later
  empty_html                   200 text/html with Content-Length: 0, no
                               flush, with Link, Set-Cookie and Cache-Control
                               headers (also answers HEAD, with the headers
                               of a non-empty page and no body)
  a.jpg                        <image-dir>/a.jpg
  gzip.css                     pre-gzipped CSS (Content-Encoding: gzip)
  content_type_present/        200 text/plain "This file should be proxied"
  content_type_absent/         200 without a Content-Type header (proxying.sh)
  health                       200 "ok" (setup_apache_test.sh readiness probe)

Usage: flush_origin.py --port 8093 --image-dir <dir holding a.jpg>
"""

import argparse
import gzip
import http.server
import os
import time
from email.utils import formatdate
from typing import List, Tuple
from urllib.parse import parse_qs, urlsplit

# Pause after each flushed chunk of the with-flush pages: long enough that
# the proxy's non-blocking read of the backend comes up empty and the proxy
# flushes what it has, as it would for a PHP page blocked after flush().
FLUSH_PAUSE_S = 0.2

# The Link header of the rule_packs routes: a preload that must survive, a
# canonical the pack's rule disagrees with, and a preconnect that must too.
LINK_HEADER_LINK = (
    "</mod_pagespeed_test/rule_packs/app.css>; rel=preload; as=style, "
    '<https://wrong.example.com/stale>; rel="canonical", '
    "<https://cdn.example.net>; rel=preconnect"
)
# The headers of empty_html that a HEAD request or an empty body must keep.
EMPTY_HTML_HEADERS = [
    ("Link", "</mod_pagespeed_test/empty_html.css>; rel=preload; as=style"),
    ("Set-Cookie", "origin_cookie=1; Path=/"),
    ("Cache-Control", "private, max-age=60, must-revalidate"),
]
# split_head's: a canonical in the headers that the flushed headers keep.
SPLIT_HEAD_LINK = (
    "</mod_pagespeed_test/rule_packs/app.css>; rel=preload; as=style, "
    "<https://wrong.example.com/split>; rel=canonical"
)

XHTML_HEAD = (
    '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Strict//EN" '
    '"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd">\n'
    '<html xmlns="http://www.w3.org/1999/xhtml" lang="en">\n'
    "<head>\n"
    "</head>\n"
    "<body>\n"
)


LINK_HEADER_BODY = (
    b'<!doctype html>\n<html>\n<head>\n<meta charset="utf-8">\n'
    b"<title>A page whose Link header names another canonical</title>\n"
    b"</head>\n<body>\n<p>The canonical is in the Link header only.</p>\n"
    b"</body>\n</html>\n"
)


class FlushOriginHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FlushOrigin/1.0"

    def log_message(self, fmt, *args):  # keep the lane log readable
        pass

    # -- response helpers ---------------------------------------------------

    def _start(self, content_type: str, extra: List[Tuple[str, str]], length=None):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        for name, value in extra:
            self.send_header(name, value)
        if length is None:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(length))
        self.end_headers()

    def _chunk(self, data: bytes, pause: float = 0.0):
        self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
        self.wfile.flush()
        if pause:
            time.sleep(pause)

    def _end_chunks(self):
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _whole(self, content_type: str, body: bytes, extra: List[Tuple[str, str]]):
        self._start(content_type, extra, length=len(body))
        self.wfile.write(body)
        self.wfile.flush()

    # -- routes ---------------------------------------------------------------

    def do_HEAD(self):
        route = urlsplit(self.path).path.lstrip("/")
        if route == "empty_html":
            # What GET would send, minus the body.
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=UTF-8")
            for name, value in EMPTY_HTML_HEADERS:
                self.send_header(name, value)
            self.send_header("Content-Length", "120")
            self.end_headers()
        elif route == "link_header":
            # What GET sends, minus the body.
            self._start(
                "text/html; charset=UTF-8",
                [("Link", LINK_HEADER_LINK)],
                length=len(LINK_HEADER_BODY),
            )
        else:
            self.send_error(404)

    def do_GET(self):
        parts = urlsplit(self.path)
        route = parts.path.lstrip("/")
        query = parse_qs(parts.query)
        if route == "slow_flushing_html_response":
            delay = float(query.get("delay", ["1"])[0])
            self._start("text/html", [])
            self._chunk(b"<html><head></head><body>\n")
            for i in range(1, 6):
                self._chunk(b"<p>foo</p><div>bar:%d</div>\n" % i, pause=delay)
            self._chunk(b"</body></html>\n")
            self._end_chunks()
        elif route == "withoutflush":
            body = (
                XHTML_HEAD
                + "Content generated by PHP without a call to flush()!\n</body>\n</html>\n"
            ).encode()
            self._whole("text/html; charset=UTF-8", body,
                        [("X-My-PHP-Header", "without_flush")])
        elif route == "withflush":
            self._start("text/html; charset=UTF-8", [("X-My-PHP-Header", "with_flush")])
            self._chunk(XHTML_HEAD.encode(), pause=FLUSH_PAUSE_S)
            self._chunk(b"Content generated by PHP with a call to flush()!\n</body>\n</html>\n")
            self._end_chunks()
        elif route == "image_rewrite_with_flush":
            self._start("text/html; charset=UTF-8", [("X-My-PHP-Header", "with_flush")])
            self._chunk(
                (
                    XHTML_HEAD.replace(
                        "<head>\n</head>\n",
                        "<head>\n  <title>Title of the document </title>\n</head>\n",
                    )
                    + '<ul>\n  <li data-thumb="a.jpg">\n    '
                ).encode(),
                pause=FLUSH_PAUSE_S,
            )
            self._chunk(
                b"Content generated by PHP with a call to flush()!\n"
                b'    <img src="a.jpg"/>\n   </li>\n</ul>\n</body>\n</html>\n'
            )
            self._end_chunks()
        elif route == "split_head":
            # The head is cut in two windows: the first one has gone out
            # before the second starts. An element the filter needs to edit
            # in the first window is out of reach; an insert into the still
            # open head is not.
            first = b'<!doctype html>\n<html>\n<head>\n<meta charset="utf-8">\n'
            if query.get("canonical", [""])[0] == "wrong":
                first += (
                    b'<link rel="canonical" href="https://wrong.example.com/a">\n'
                    b'<link rel="canonical" href="https://wrong.example.com/b">\n'
                )
            self._start("text/html; charset=UTF-8", [("Link", SPLIT_HEAD_LINK)])
            self._chunk(first, pause=FLUSH_PAUSE_S)
            self._chunk(b"</head>\n<body>\n<p>A head split by a flush.</p>\n</body>\n</html>\n")
            self._end_chunks()
        elif route == "link_header":
            # One window, no flush before the head closes: the response
            # headers are still writable when the rule_packs filter decides.
            self._whole("text/html; charset=UTF-8", LINK_HEADER_BODY, [("Link", LINK_HEADER_LINK)])
        elif route == "link_header_late_body":
            body = (
                b'<!doctype html>\n<html>\n<head>\n<meta charset="utf-8">\n'
                b"<title>A page whose Link header names another canonical</title>\n"
                b"</head>\n<body>\n<p>The canonical is in the Link header only.</p>\n"
                b"</body>\n</html>\n"
            )
            self._start("text/html; charset=UTF-8", [("Link", LINK_HEADER_LINK)], length=len(body))
            self.wfile.flush()
            time.sleep(FLUSH_PAUSE_S + 0.1)
            self.wfile.write(body)
            self.wfile.flush()
        elif route == "plain_text_html":
            self._start("text/html; charset=UTF-8", [])
            self.wfile.flush()
            for i in range(3):
                self._chunk(b"plain text chunk %d\n" % i, pause=0.5)
            self._end_chunks()
        elif route == "early_flush_empty_body":
            self._start(
                "text/html; charset=UTF-8",
                [
                    ("Link", LINK_HEADER_LINK),
                    ("Set-Cookie", "origin_cookie=1; Path=/"),
                    ("X-Origin-Header", "kept"),
                ],
            )
            self.wfile.flush()
            time.sleep(FLUSH_PAUSE_S + 0.1)
            self._end_chunks()
        elif route == "empty_html":
            self._whole("text/html; charset=UTF-8", b"", EMPTY_HTML_HEADERS)
        elif route == "a.jpg":
            with open(os.path.join(self.server.image_dir, "a.jpg"), "rb") as f:
                body = f.read()
            self._whole("image/jpeg", body, [("Cache-Control", "max-age=600")])
        elif route == "gzip.css":
            unique = "%x" % time.time_ns()
            pad = (unique * (10000 // len(unique) + 1))[:10000]
            css = (
                ".peachpuff {background-color: peachpuff;}\n."
                + pad
                + " {background-color: antiquewhite;}\n"
            ).encode()
            self._whole(
                "text/css",
                gzip.compress(css, compresslevel=2, mtime=0),
                [
                    ("Cache-Control", "max-age=86400"),
                    ("Expires", formatdate(time.time() + 86400, usegmt=True)),
                    ("Content-Encoding", "gzip"),
                ],
            )
        elif route == "content_type_present/":
            self._whole("text/plain", b"This file should be proxied\n", [])
        elif route == "content_type_absent/":
            body = b"This file should not be proxied\n"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
        elif route == "health":
            self._whole("text/plain", b"ok", [])
        else:
            self.send_error(404)


def make_server(port: int, image_dir: str, host: str = "127.0.0.1"):
    """A threading origin on host:port (0 = ephemeral), serving image_dir/a.jpg."""
    server = http.server.ThreadingHTTPServer((host, port), FlushOriginHandler)
    server.daemon_threads = True
    server.image_dir = image_dir
    return server


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8093)
    parser.add_argument("--image-dir", required=True)
    args = parser.parse_args()
    make_server(args.port, args.image_dir).serve_forever()


if __name__ == "__main__":
    main()
