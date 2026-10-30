# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""IIS admin page access: local requests, relayed requests and *Domains.

The admin, statistics, console and message pages are served to local requests
(and to hosts on the matching AdminDomains / StatisticsDomains allowlist).  A
request relayed by a reverse proxy on the same server carries forwarding
headers and is not local unless TreatProxiedRequestsAsLocal is on.

The lane's site config (setup_iis_full.ps1) has:
    AdminDomains allow admin.example.com
    StatisticsDomains allow stats.example.com
and leaves TreatProxiedRequestsAsLocal at its default (off).

Two ways of producing a relayed request are used:
- headers added by the test client (what a proxy adds), and
- a small forwarding proxy started by the test on the loopback address,
  standing in for an ARR rule or a local reverse proxy when that is not
  installed on the host.
"""

import http.client
import os
import subprocess
import time
import http.server
import socketserver
import threading

import pytest

from pagespeed_test_framework import PageSpeedClient

ADMIN_HOST = "admin.example.com"
STATS_HOST = "stats.example.com"
OTHER_HOST = "www.example.com"

FORWARDING_HEADERS = [
    {"X-Forwarded-For": "203.0.113.9"},
    {"X-ARR-ClientIP": "203.0.113.9"},
    {"Forwarded": "for=203.0.113.9;proto=http"},
    {"X-Real-IP": "203.0.113.9"},
]

# (path, JSON or HTML) pages that follow the rule; the host list is the
# allowlist that applies to the page.
PAGES = [
    ("/pagespeed_admin/statistics", ADMIN_HOST),
    ("/pagespeed_global_admin/statistics", None),
    ("/pagespeed_statistics", STATS_HOST),
    ("/pagespeed_console", None),
    ("/pagespeed_message", None),
]


def _served(response) -> bool:
    return response.status in (200, 301, 302)


def _refused(response) -> bool:
    content_type = response.headers.get("Content-Type", "").lower()
    return response.status == 404 and "application/json" not in content_type


class _ForwardingProxy:
    """A same-host reverse proxy: forwards GETs to the site and adds the
    headers a proxy adds.  The Host header is passed through."""

    def __init__(self, target_host: str, target_port: int):
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                headers = {
                    k: v
                    for k, v in self.headers.items()
                    if k.lower() not in ("connection", "keep-alive")
                }
                headers["X-Forwarded-For"] = "203.0.113.9"
                headers["X-ARR-ClientIP"] = "203.0.113.9"
                conn = http.client.HTTPConnection(
                    outer.target_host, outer.target_port, timeout=30
                )
                try:
                    conn.request("GET", self.path, headers=headers)
                    upstream = conn.getresponse()
                    body = upstream.read()
                    self.send_response(upstream.status)
                    for k, v in upstream.getheaders():
                        if k.lower() not in (
                            "transfer-encoding",
                            "connection",
                            "content-length",
                        ):
                            self.send_header(k, v)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                finally:
                    conn.close()

        self.target_host = target_host
        self.target_port = target_port
        self._server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture(scope="module")
def proxied_client(server_config) -> PageSpeedClient:
    proxy = _ForwardingProxy(server_config.host, server_config.port)
    proxy.start()
    try:
        yield PageSpeedClient(host="127.0.0.1", port=proxy.port)
    finally:
        proxy.stop()


class TestDirectLocalRequests:
    @pytest.mark.iis_only
    @pytest.mark.parametrize("path,host", PAGES)
    def test_local_request_is_served(self, client: PageSpeedClient, path, host):
        response = client.get(path)
        assert _served(response), f"{path}: got {response.status}"

    @pytest.mark.iis_only
    @pytest.mark.parametrize("path,host", PAGES)
    def test_local_request_with_unlisted_host_is_served(
        self, client: PageSpeedClient, path, host
    ):
        response = client.get(path, headers={"Host": OTHER_HOST})
        assert _served(response), f"{path}: got {response.status}"


class TestRelayedRequests:
    @pytest.mark.iis_only
    @pytest.mark.parametrize("forwarding", FORWARDING_HEADERS)
    @pytest.mark.parametrize("path,host", PAGES)
    def test_relayed_request_is_refused(
        self, client: PageSpeedClient, path, host, forwarding
    ):
        response = client.get(path, headers=forwarding)
        assert _refused(response), (
            f"{path} with {list(forwarding)}: got {response.status} "
            f"({response.headers.get('Content-Type', '')})"
        )

    @pytest.mark.iis_only
    @pytest.mark.parametrize("path,host", [p for p in PAGES if p[1]])
    def test_relayed_request_for_a_configured_domain_is_served(
        self, client: PageSpeedClient, path, host
    ):
        headers = {"Host": host, "X-Forwarded-For": "203.0.113.9"}
        response = client.get(path, headers=headers)
        assert _served(response), f"{path} for {host}: got {response.status}"

    @pytest.mark.iis_only
    def test_relayed_request_for_another_pages_domain_is_refused(
        self, client: PageSpeedClient
    ):
        headers = {"Host": STATS_HOST, "X-Forwarded-For": "203.0.113.9"}
        assert _refused(client.get("/pagespeed_admin/statistics", headers=headers))
        headers = {"Host": ADMIN_HOST, "X-Forwarded-For": "203.0.113.9"}
        assert _refused(client.get("/pagespeed_statistics", headers=headers))

    @pytest.mark.iis_only
    def test_relayed_request_for_an_unlisted_domain_is_refused(
        self, client: PageSpeedClient
    ):
        headers = {"Host": OTHER_HOST, "X-Forwarded-For": "203.0.113.9"}
        assert _refused(client.get("/pagespeed_admin/statistics", headers=headers))


class TestThroughLocalProxy:
    @pytest.mark.iis_only
    @pytest.mark.parametrize("path,host", PAGES)
    def test_visitor_behind_the_proxy_is_refused(
        self, proxied_client: PageSpeedClient, path, host
    ):
        response = proxied_client.get(path, headers={"Host": OTHER_HOST})
        assert _refused(response), f"{path}: got {response.status}"

    @pytest.mark.iis_only
    @pytest.mark.parametrize("path,host", [p for p in PAGES if p[1]])
    def test_configured_domain_behind_the_proxy_is_served(
        self, proxied_client: PageSpeedClient, path, host
    ):
        response = proxied_client.get(path, headers={"Host": host})
        assert _served(response), f"{path} for {host}: got {response.status}"

    @pytest.mark.iis_only
    def test_site_pages_are_still_served_through_the_proxy(
        self, proxied_client: PageSpeedClient
    ):
        response = proxied_client.get("/", headers={"Host": OTHER_HOST})
        assert response.status in (200, 301, 302, 404)


SITE_CONFIG = os.environ.get(
    "PAGESPEED_SITE_CONFIG", r"C:\inetpub\pagespeed_test\pagespeed.config"
)


APP_POOL = os.environ.get("PAGESPEED_APP_POOL", "PageSpeedTestPool")
APPCMD = os.path.join(
    os.environ.get("SystemRoot", r"C:\Windows"), "System32", "inetsrv", "appcmd.exe"
)


def _recycle_app_pool():
    """Recycle the site's application pool so the module re-reads its config."""
    result = subprocess.run(
        [APPCMD, "recycle", "apppool", f"/apppool.name:{APP_POOL}"],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    time.sleep(3)


@pytest.fixture
def proxied_requests_as_local(client):
    """Turn TreatProxiedRequestsAsLocal on in the site config for one test.

    The application pool is recycled so the module re-reads the site config;
    the original file is restored (and the pool recycled again) afterwards.
    """
    if not os.path.exists(SITE_CONFIG):
        pytest.skip(f"site config not found: {SITE_CONFIG}")
    with open(SITE_CONFIG, "rb") as f:
        original = f.read()
    # Insert the directive after the first line: the lane config ends with
    # host-specific blocks, and a directive appended there would only apply
    # to the last host.
    text = original.decode("utf-8-sig")
    first, _, rest = text.partition("\n")
    patched = first + "\npagespeed TreatProxiedRequestsAsLocal on\n" + rest
    with open(SITE_CONFIG, "wb") as f:
        f.write(b"\xef\xbb\xbf" + patched.encode("utf-8"))
    _recycle_app_pool()
    try:
        yield
    finally:
        with open(SITE_CONFIG, "wb") as f:
            f.write(original)
        _recycle_app_pool()
        # Wait for the restored config to be picked up, so later tests see
        # the default again.
        headers = {"Host": OTHER_HOST, "X-Forwarded-For": "203.0.113.9"}
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if _refused(client.get("/pagespeed_admin/statistics", headers=headers)):
                break
            time.sleep(2)


class TestTreatProxiedRequestsAsLocalOn:
    @pytest.mark.iis_only
    def test_relayed_loopback_requests_are_served(
        self, client: PageSpeedClient, proxied_requests_as_local
    ):
        headers = {"Host": OTHER_HOST, "X-Forwarded-For": "203.0.113.9"}
        deadline = time.monotonic() + 60
        response = client.get("/pagespeed_admin/statistics", headers=headers)
        while not _served(response) and time.monotonic() < deadline:
            time.sleep(2)
            response = client.get("/pagespeed_admin/statistics", headers=headers)
        assert _served(response), f"got {response.status}"
        for path, _ in PAGES:
            assert _served(client.get(path, headers=headers)), path


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
