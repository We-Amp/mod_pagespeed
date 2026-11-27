# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Programmable HTTP origin for E2E tests.

A lightweight Python HTTP server that tests can configure per-test
with specific routes, content, headers, status codes, and delays.
Runs in a background thread.

For Phase 6 this starts simple (serves static testdata files), but the
per-route API enables future tests to inject specific HTML/CSS/JS/image
content and verify the exact optimization applied.
"""

import http.server
import threading
import time
from dataclasses import dataclass, field


@dataclass
class RequestRecord:
    """A recorded incoming request."""

    method: str
    path: str
    headers: dict[str, str]
    timestamp: float


@dataclass
class Route:
    """A configured route with response content."""

    content: bytes
    content_type: str
    status: int = 200
    delay: float = 0.0
    headers: dict[str, str] = field(default_factory=dict)


class OriginHandler(http.server.BaseHTTPRequestHandler):
    """HTTP request handler that serves configured routes."""

    def do_GET(self):
        server = self.server
        # Record the request
        headers = {k: v for k, v in self.headers.items()}
        record = RequestRecord(
            method="GET",
            path=self.path,
            headers=headers,
            timestamp=time.time(),
        )
        server.request_log.append(record)

        # Check for a configured route
        route = server.routes.get(self.path)
        if route is None:
            self.send_error(404, f"No route configured for {self.path}")
            return

        if route.delay > 0:
            time.sleep(route.delay)

        self.send_response(route.status)
        self.send_header("Content-Type", route.content_type)
        self.send_header("Content-Length", str(len(route.content)))
        for key, value in route.headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(route.content)

    def log_message(self, format, *args):
        """Suppress default logging."""
        pass


class OriginServer:
    """Programmable HTTP origin for E2E tests.

    Each test can register routes with specific content, headers,
    status codes, and delays. Runs in a background thread.

    Usage:
        origin = OriginServer(port=9999)
        origin.add_route("/style.css", b"body{}", "text/css")
        origin.start()
        # ... run tests ...
        origin.stop()
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 0):
        self._host = host
        self._port = port
        self._server = None
        self._thread = None

    def add_route(
        self,
        path: str,
        content: bytes | str,
        content_type: str,
        status: int = 200,
        delay: float = 0.0,
        headers: dict[str, str] | None = None,
    ):
        """Register a route with specific response content."""
        if isinstance(content, str):
            content = content.encode("utf-8")
        if self._server is None:
            self._ensure_server()
        self._server.routes[path] = Route(
            content=content,
            content_type=content_type,
            status=status,
            delay=delay,
            headers=headers or {},
        )

    def add_static_dir(self, url_prefix: str, local_dir: str):
        """Register all files in a local directory as routes.

        Reads files at registration time (not dynamically).
        """
        import os

        for root, _dirs, files in os.walk(local_dir):
            for fname in files:
                fpath = os.path.join(root, fname)
                rel = os.path.relpath(fpath, local_dir)
                url_path = url_prefix.rstrip("/") + "/" + rel
                # Guess content type
                ct = "application/octet-stream"
                if fname.endswith(".html"):
                    ct = "text/html"
                elif fname.endswith(".css"):
                    ct = "text/css"
                elif fname.endswith(".js"):
                    ct = "application/javascript"
                elif fname.endswith(".jpg") or fname.endswith(".jpeg"):
                    ct = "image/jpeg"
                elif fname.endswith(".png"):
                    ct = "image/png"
                elif fname.endswith(".gif"):
                    ct = "image/gif"
                elif fname.endswith(".webp"):
                    ct = "image/webp"
                with open(fpath, "rb") as f:
                    content = f.read()
                self.add_route(url_path, content, ct)

    @property
    def request_log(self) -> list[RequestRecord]:
        """Return the list of recorded requests."""
        if self._server is None:
            return []
        return self._server.request_log

    @property
    def port(self) -> int:
        """Return the port the server is listening on."""
        if self._server is None:
            return 0
        return self._server.server_address[1]

    def reset(self):
        """Clear all routes and request log."""
        if self._server:
            self._server.routes.clear()
            self._server.request_log.clear()

    def _ensure_server(self):
        if self._server is None:
            self._server = http.server.HTTPServer(
                (self._host, self._port), OriginHandler
            )
            self._server.routes = {}
            self._server.request_log = []

    def start(self):
        """Start the server in a background thread."""
        self._ensure_server()
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def stop(self):
        """Stop the server."""
        if self._server:
            self._server.shutdown()
            self._server.server_close()
        if self._thread:
            self._thread.join(timeout=5)
