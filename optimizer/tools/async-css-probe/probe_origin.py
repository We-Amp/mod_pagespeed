# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Static origin for the async-CSS probe.

The fixture directory mirrors the captured site's URL space: a file at
``<fixture>/a/b.css`` is served at ``/a/b.css``, and ``index.html`` at the
fixture root is served at ``/`` (see ``fixtures/modpagespeed-com/CAPTURE.md``).
One rule, no per-asset special cases — a capture refresh drops files at the
path the page asks for and they are served, which is what adding the fonts and
logos needed.

The route table is still built once at startup from what is on disk, and
anything not in it is a 404. That is deliberate: the fold must render from
frozen bytes, so a path the capture does not contain has to fail loudly rather
than be improvised.

Deliberately dumb: no conditional requests, no ranges, no programmable routes.
The probe is about what the optimizer does to a realistic page, so the origin
must contribute nothing of its own. The one thing it does contribute is a
cacheable response — without ``Cache-Control`` the optimizer has nothing to
store, and the deferral decision never runs.
"""

import hashlib
import http.server
import os
import sys
from email.utils import formatdate

FIXTURE_DIR = os.environ.get("PROBE_FIXTURE", "/var/www/fixture")

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".svg": "image/svg+xml",
    ".woff2": "font/woff2",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def build_routes(fixture_dir: str) -> dict[str, str]:
    """Map request paths to fixture files, mirroring the fixture tree."""
    routes: dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(fixture_dir):
        dirnames.sort()
        for name in sorted(filenames):
            if name.endswith(".md"):
                continue  # CAPTURE.md documents the capture; it is not served
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, fixture_dir)
            route = "/" + rel.replace(os.sep, "/")
            routes[route] = path
            if rel == "index.html":
                routes["/"] = path
    return routes


ROUTES = build_routes(FIXTURE_DIR)

# A second page, served beside the first: its index.html answers at PLAIN_PATH
# and every other file at its own path (the page links its stylesheet by an
# absolute path). The two fixtures share no file name, so neither shadows the
# other; a collision is refused rather than resolved.
PLAIN_FIXTURE_DIR = os.environ.get("PROBE_PLAIN_FIXTURE", "")
PLAIN_PATH = "/plain/"
if PLAIN_FIXTURE_DIR:
    for _route, _target in build_routes(PLAIN_FIXTURE_DIR).items():
        _route = PLAIN_PATH if _route == "/" else _route
        if _route == "/index.html":
            continue  # the plain page's index is served at PLAIN_PATH only
        if _route in ROUTES:
            sys.exit(f"ERROR: plain fixture path {_route} collides with the primary")
        ROUTES[_route] = _target
    ROUTES[PLAIN_PATH] = os.path.join(PLAIN_FIXTURE_DIR, "index.html")


class ProbeHandler(http.server.BaseHTTPRequestHandler):
    server_version = "AsyncCssProbeOrigin/1.0"

    def _send(self, path: str, send_body: bool = True):
        with open(path, "rb") as f:
            content = f.read()
        _, ext = os.path.splitext(path)
        self.send_response(200)
        self.send_header(
            "Content-Type", MIME_TYPES.get(ext.lower(), "application/octet-stream")
        )
        self.send_header("Content-Length", str(len(content)))
        digest = hashlib.sha256(content).hexdigest()[:16]
        self.send_header("ETag", f'"{digest}"')
        self.send_header("Cache-Control", "public, max-age=3600")
        self.send_header("Date", formatdate(usegmt=True))
        self.end_headers()
        if send_body:
            self.wfile.write(content)

    def do_GET(self):
        path = self.path.split("?")[0]
        target = ROUTES.get(path)
        if target is None:
            self.send_error(404, f"Not in fixture: {path}")
            return
        self._send(target)

    def do_HEAD(self):
        path = self.path.split("?")[0]
        target = ROUTES.get(path)
        if target is None:
            self.send_error(404, f"Not in fixture: {path}")
            return
        self._send(target, send_body=False)

    def log_message(self, format, *args):
        sys.stderr.write(f"[probe-origin] {self.address_string()} - {format % args}\n")


def main():
    port = int(os.environ.get("PORT", "8081"))
    bind = os.environ.get("BIND", "0.0.0.0")
    if not ROUTES:
        print(f"ERROR: no fixture files found in {FIXTURE_DIR}", file=sys.stderr)
        sys.exit(1)
    print(f"Probe origin listening on {bind}:{port}", flush=True)
    for route, target in ROUTES.items():
        print(f"  {route} -> {target}", flush=True)
    # Threading: a browser opens several connections at once, and a
    # single-threaded origin serializes them into what looks like a slow site —
    # which is exactly the variable the rendered lane is trying to hold still.
    server = http.server.ThreadingHTTPServer((bind, port), ProbeHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
