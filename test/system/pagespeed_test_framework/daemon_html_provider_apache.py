# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""The Apache lane's provider for the HTML cache live conformance rows.

Lane fixture "daemon_html_conformance" (setup_apache_test.sh): an optimizer
worker of its own (HTML in markdown-static, agent-optimize on) and one site
file this provider rewrites for every row, then reloads the server with a
graceful restart. Each row gets:

  * www.example.test on port 443 over TLS (the host and scheme the rows'
    URLs name), with a CacheFragment of its own, so its entries are in a
    keyspace no other row has written;
  * the origin this provider runs on 127.0.0.1, behind mod_proxy, serving
    the row's status, header lines and body for every path;
  * the row's knobs as Apache directives (see _vhost).

settle() waits for a positive signal: the access log line of every request
it has received a response for. Apache writes it from the log_transaction
hook, after the handler and every output filter returned, and the HTML
cache's commit runs inline in an output filter after the response's last
brigade went out, so a logged request's store or removal has finished. A
request whose response has not arrived (a held origin) is not waited for.

`process` names a server process: Apache runs prefork here, and a connection
is served by one child for as long as it is open, so each name keeps one
connection open for the row.
"""

import hashlib
import http.client
import json
import os
import re
import socket
import ssl
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, Iterable, List, Optional, Tuple
from urllib.parse import quote, urlsplit

from pagespeed_test_framework import daemon_html_conformance as conf
from pagespeed_test_framework.daemon_html_conformance import (
    HeaderLines,
    ProviderUnsupported,
    Response,
    StoredEntry,
)

HOST = "www.example.test"
# What the admin console sends with a state-changing cache action.
ADMIN_HEADERS = [("X-Requested-With", "XMLHttpRequest"), ("Content-Length", "0")]
SCHEME = "https"
PROBE_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "daemon_html_volume_probe.py")

# Every knob this provider turns into configuration.
KNOBS = frozenset([
    "header_add", "header_always", "header_early", "expires", "auth",
    "alias", "default_vhost", "in_place", "pagespeed_off_location",
    "compress_markdown", "third_party_filter", "poll_interval_s",
    "chained_module_origin",
])


def _env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise ProviderUnsupported(f"{name} is not set (lane fixture daemon_html_conformance)")
    return value


class _Origin:
    """The row's origin: one status, header set and body for every path.
    Records the header lines of every request it receives."""

    def __init__(self, port: int) -> None:
        self.lock = threading.Lock()
        self.status = 200
        self.headers: HeaderLines = []
        self.body = b""
        self.echo = False
        self.seen: List[HeaderLines] = []
        # hold_origin: the next response waits for `release` after its
        # headers.
        self.hold_next = False
        self.release = threading.Event()
        origin = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):  # quiet
                pass

            def _answer(self, head: bool) -> None:
                lines = [(n, v) for n, v in self.headers.items()]
                with origin.lock:
                    origin.seen.append(lines)
                    status, headers, body = origin.status, list(origin.headers), origin.body
                    hold = origin.release if origin.hold_next else None
                    origin.hold_next = False
                self.send_response(status)
                for n, v in headers:
                    self.send_header(n, v)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if hold is not None:
                    self.wfile.flush()
                    hold.wait(timeout=60)
                if not head:
                    self.wfile.write(body)

            def do_GET(self):  # noqa: N802
                self._answer(False)

            def do_HEAD(self):  # noqa: N802
                self._answer(True)

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def configure(self, status: int, headers: HeaderLines, body: bytes, echo: bool) -> None:
        with self.lock:
            self.status, self.headers, self.body, self.echo = status, headers, body, echo
            self.seen = []
            self.hold_next = False
            self.release.set()
            self.release = threading.Event()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


class ApacheProvider(conf.Provider):
    port = "apache"

    def __init__(self, server_config) -> None:
        self.server_config = server_config
        self.state_dir = _env("PAGESPEED_DHC_DIR")
        self.site_conf = _env("PAGESPEED_DHC_SITE_CONF")
        self.volume = _env("PAGESPEED_DHC_VOLUME")
        self.socket_path = _env("PAGESPEED_DHC_SOCKET")
        self.library = os.environ.get("PAGESPEED_DHC_LIBRARY", "/usr/local/lib/libpagespeed.so")
        self.origin_port = int(_env("PAGESPEED_DHC_ORIGIN_PORT"))
        self.tls_port = int(os.environ.get("PAGESPEED_DHC_TLS_PORT", "443"))
        self.inner_port = int(_env("PAGESPEED_DHC_INNER_PORT"))
        self.cert = _env("PAGESPEED_DHC_TLS_CERT")
        self.key = _env("PAGESPEED_DHC_TLS_KEY")
        self.ctl = os.environ.get("PAGESPEED_APACHE_CTL", "apache2ctl")
        self.worker_user = os.environ.get("PAGESPEED_DHC_WORKER_USER", "www-data")
        self.access_log = os.path.join(self.state_dir, "log", "access.log")
        self.origin: Optional[_Origin] = None
        self.generation = 0
        self.row_index = 0
        self.live: dict = {}
        self.mode = "off"
        self.fragment = ""
        self.answered: Dict[int, int] = {}
        self.answered_lock = threading.Lock()
        self.log_offset = 0
        self.connections: Dict[str, http.client.HTTPSConnection] = {}

    # ---- configuration

    def _site_id(self) -> str:
        return hashlib.sha256(self.fragment.encode()).hexdigest()

    def _uses_flush_file(self) -> bool:
        return any(
            next(iter(op)) == "purge_global" and op["purge_global"].get("via") == "flush_file"
            for s in self.live.get("steps", []) for op in s.get("pre", [])
        )

    def _vhost(self, mode: str) -> str:
        live = self.live
        knobs = live.get("vhost", {})
        agent = "on" if live.get("agent_optimize") else "off"
        poll = knobs.get("poll_interval_s", 1)
        enable_purge = not self._uses_flush_file()
        backend = f"http://127.0.0.1:{self.origin_port}"
        inner = []
        if knobs.get("chained_module_origin"):
            # A server running this module in front of the origin, on this
            # same server: the row's host proxies to it.
            inner = [
                f"<VirtualHost 127.0.0.1:{self.inner_port}>",
                "  ServerName dhc-inner.example.test",
                f'  DocumentRoot "{self.state_dir}/docroot"',
                "  ModPagespeed on",
                f'  ModPagespeedFileCachePath "{self.state_dir}/filecache"',
                f"  ModPagespeedCacheFragment {self.fragment}-inner",
                "  ModPagespeedRewriteLevel CoreFilters",
                "  ModPagespeedAgentOptimize on",
                "  ModPagespeedDaemonHtmlCache serve-variants",
                f"  ModPagespeedDaemonSocketPath {self.socket_path}",
                f"  ModPagespeedDaemonVolumePath {self.volume}",
                f"  ProxyPass / {backend}/",
                "</VirtualHost>",
                "",
            ]
            backend = f"http://127.0.0.1:{self.inner_port}"
        v: List[str] = [
            f"Listen 127.0.0.1:{self.inner_port}" if inner else "",
            *inner,
            f"<VirtualHost *:{self.tls_port}>",
            f"  ServerName {HOST}",
        ]
        for alias in knobs.get("alias", []):
            v.append(f"  ServerAlias {alias}")
        v += [
            "  SSLEngine on",
            f"  SSLCertificateFile {self.cert}",
            f"  SSLCertificateKeyFile {self.key}",
            f'  DocumentRoot "{self.state_dir}/docroot"',
            f'  <Directory "{self.state_dir}/docroot">',
            "    Require all granted",
            "  </Directory>",
            "  DirectoryIndex /page",
            "  KeepAlive On",
            "  KeepAliveTimeout 60",
            f'  CustomLog "{self.access_log}" "%{{remote}}p %>s %m %U"',
            # The generation probe: answered by mod_alias, never proxied.
            f"  Redirect 410 /__dhc_generation_{self.generation}",
            '  ProxyPassMatch "^/__dhc_generation_" !',
            "  ProxyPass /pagespeed_admin !",
            "  ProxyPass /pagespeed_global_admin !",
            "  ProxyPass /mod_pagespeed_statistics !",
            "  ProxyPassMatch ^/(.+)$ " + backend + "/$1",
        ]
        if inner:
            v += ["  ProxyPreserveHost On", "  RequestHeader set Host dhc-inner.example.test"]
        v += [
            "  ModPagespeed on",
            f'  ModPagespeedFileCachePath "{self.state_dir}/filecache"',
            f"  ModPagespeedCacheFragment {self.fragment}",
            "  ModPagespeedRewriteLevel CoreFilters",
            f"  ModPagespeedAgentOptimize {agent}",
            f"  ModPagespeedDaemonHtmlCache {mode}",
            f"  ModPagespeedDaemonSocketPath {self.socket_path}",
            f"  ModPagespeedDaemonVolumePath {self.volume}",
            f'  ModPagespeedLogDir "{self.state_dir}/log"',
            f"  ModPagespeedCacheFlushPollIntervalSec {int(poll)}",
            f"  ModPagespeedEnableCachePurge {'on' if enable_purge else 'off'}",
            "  ModPagespeedPurgeMethod PURGE",
            "  <Location /pagespeed_admin>",
            "    SetHandler pagespeed_admin",
            "  </Location>",
            "  <Location /pagespeed_global_admin>",
            "    SetHandler pagespeed_global_admin",
            "  </Location>",
        ]
        in_place = knobs.get("in_place", {})
        v.append("  ModPagespeedInPlaceResourceOptimization "
                 + ("off" if in_place.get("enabled") is False else "on"))
        if "s_maxage" in in_place:
            v.append(f"  ModPagespeedInPlaceSMaxAgeSec {int(in_place['s_maxage'])}")
        if knobs.get("expires"):
            seconds = {"1h": 3600}.get(knobs["expires"])
            if seconds is None:
                raise ProviderUnsupported(f"expires {knobs['expires']!r}")
            v += ["  ExpiresActive On", f'  ExpiresDefault "access plus {seconds} seconds"']
        for name, value in knobs.get("header_add", []):
            verb = "append" if name.lower() == "vary" else (
                "set" if name.lower() == "cache-control" else "add")
            v.append(f'  Header {verb} {name} "{value}"')
        for name, value in knobs.get("header_always", []):
            v.append(f'  Header always set {name} "{value}"')
        for name, value in knobs.get("header_early", []):
            # httpd's early form: applied before the handler runs.
            v.append(f'  Header set {name} "{value}" early')
        auth = knobs.get("auth")
        if auth:
            if auth.get("user") != "u" or auth.get("password") != "p":
                raise ProviderUnsupported("auth: the lane's password file holds u:p only")
            v += [
                f'  <Location {auth["location"]}>',
                "    AuthType Basic",
                "    AuthName conformance",
                f'    AuthUserFile "{self.state_dir}/htpasswd"',
                f'    Require {auth["require"]}',
                "  </Location>",
            ]
        off = knobs.get("pagespeed_off_location")
        if off:
            v += [f"  <Location {off}>", "    ModPagespeed off", "  </Location>"]
        compress = knobs.get("compress_markdown")
        if compress:
            if isinstance(compress, dict) and compress.get("ignores_no_compress"):
                # A filter that marks every response it sees encoded, never
                # looking at no-gzip.
                v += [
                    f'  LuaOutputFilter dhc_mark_encoded "{self.state_dir}/lua/filters.lua" mark_encoded',
                    "  AddOutputFilterByType dhc_mark_encoded text/markdown",
                ]
            else:
                v.append("  AddOutputFilterByType DEFLATE text/markdown text/html")
        third = knobs.get("third_party_filter")
        if third:
            if "adds" in third:
                if third["adds"] != [["X-Filter", "1"]]:
                    raise ProviderUnsupported(f"third_party_filter adds {third['adds']!r}")
                v += [
                    f'  LuaOutputFilter dhc_add_header "{self.state_dir}/lua/filters.lua" add_header',
                    "  AddOutputFilterByType dhc_add_header text/markdown text/html",
                ]
            if "rewrites_body" in third:
                v += [
                    f"  AddOutputFilterByType SUBSTITUTE {third['rewrites_body']}",
                    '  Substitute "s/e/EE/"',
                ]
        v += ["</VirtualHost>", ""]
        return "\n".join(v)

    def _write_and_reload(self, mode: str) -> None:
        self.generation += 1
        text = self._vhost(mode)
        # The first virtual host on the port answers unknown names: a site
        # file that sorts first, enabled directly.
        first = os.path.join(os.path.dirname(os.path.dirname(self.site_conf)),
                             "sites-enabled", "000-dhc-default.conf")
        if self.live.get("vhost", {}).get("default_vhost"):
            path, other = first, self.site_conf
        else:
            path, other = self.site_conf, first
        self._sudo_write(path, text)
        self._sudo_write(other, "# not used by this row\n")
        self._graceful()
        self.mode = mode

    def _sudo_write(self, path: str, text: str) -> None:
        subprocess.run(["sudo", "tee", path], input=text.encode(), check=True,
                       stdout=subprocess.DEVNULL)

    def _graceful(self) -> None:
        test = subprocess.run(["sudo", self.ctl, "-t"], capture_output=True, text=True)
        if test.returncode != 0:
            raise AssertionError(
                f"the row's configuration does not load: {test.stderr[-1500:]}")
        subprocess.run(["sudo", self.ctl, "graceful"], check=True, capture_output=True)
        # Live once the probe of this generation is answered by mod_alias,
        # several times in a row (old children may still take a connection).
        deadline = time.monotonic() + 30
        good = 0
        while time.monotonic() < deadline:
            try:
                r = self._send("GET", f"/__dhc_generation_{self.generation}", HOST, [], track=False)
                good = good + 1 if r.status == 410 else 0
            except OSError:
                good = 0
            if good >= 5:
                return
            time.sleep(0.1)
        raise AssertionError("the server never answered with the row's configuration")

    # ---- Provider

    def setup(self, row_id: str, live: dict, bodies: Dict[str, str]) -> Iterable[str]:
        self.row_index += 1
        self.live = live
        self.fragment = "dhc-" + re.sub(r"[^a-z0-9]", "-", row_id.lower())
        if self.origin is None:
            self.origin = _Origin(self.origin_port)
        origin = live["origin"]
        self.origin.configure(
            int(origin["status"]), [tuple(h) for h in origin["headers"]],
            bodies[origin["body_ref"]].encode("utf-8"),
            bool(origin.get("echo_conditionals")))
        with self.answered_lock:
            self.answered = {}
        self.log_offset = self._log_size()
        self._write_and_reload(live["mode"])
        applied = {"mode", "agent_optimize", "worker"}
        applied |= {f"origin.{k}" for k in conf.ORIGIN_KEYS}
        applied |= set(live.get("vhost", {})) & KNOBS
        return applied

    def teardown(self) -> None:
        if self.origin is not None:
            with self.origin.lock:
                self.origin.hold_next = False
                self.origin.release.set()
        for c in self.connections.values():
            c.close()
        self.connections = {}

    def _log_size(self) -> int:
        try:
            return os.path.getsize(self.access_log)
        except OSError:
            return 0

    def _logged(self) -> Dict[int, int]:
        counts: Dict[int, int] = {}
        try:
            with open(self.access_log, "rb") as f:
                f.seek(self.log_offset)
                data = f.read().decode("latin-1")
        except OSError:
            return counts
        for line in data.splitlines():
            parts = line.split(" ", 1)
            if parts and parts[0].isdigit():
                counts[int(parts[0])] = counts.get(int(parts[0]), 0) + 1
        return counts

    def settle(self) -> None:
        with self.answered_lock:
            want = dict(self.answered)
        deadline = time.monotonic() + 30
        while True:
            got = self._logged()
            if all(got.get(p, 0) >= n for p, n in want.items()):
                return
            if time.monotonic() > deadline:
                raise AssertionError(
                    "requests answered but never finished on the server: "
                    f"{sorted(p for p, n in want.items() if got.get(p, 0) < n)}")
            time.sleep(0.02)

    def _connect(self, host: str) -> http.client.HTTPSConnection:
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        class Conn(http.client.HTTPSConnection):
            def connect(self):
                sock = socket.create_connection((self.host, self.port), self.timeout)
                self.sock = context.wrap_socket(sock, server_hostname=host)

        conn = Conn("127.0.0.1", self.tls_port, timeout=60)
        conn.connect()
        return conn

    def _send(self, method: str, path: str, host: str, headers: HeaderLines,
              process: Optional[str] = None, track: bool = True) -> Response:
        if process is None:
            conn = self._connect(host)
        else:
            conn = self.connections.get(process)
            if conn is None:
                conn = self.connections[process] = self._connect(host)
        local_port = conn.sock.getsockname()[1]
        try:
            conn.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
            conn.putheader("Host", host)
            for name, value in headers:
                conn.putheader(name, value)
            conn.endheaders()
            r = conn.getresponse()
            body = r.read()
            response = Response(r.status, list(r.getheaders()), body)
        finally:
            if process is None:
                conn.close()
        if track:
            with self.answered_lock:
                self.answered[local_port] = self.answered.get(local_port, 0) + 1
        return response

    def request(self, method, path, host, headers, process=None) -> Response:
        return self._send(method, path, host or HOST, headers, process)

    def _probe(self, path: str, host: Optional[str], ask: bool = False) -> dict:
        h = (host or HOST).lower()
        # The volume keys an entry by its path, host name, scheme and site.
        out = subprocess.run(
            ["sudo", "-u", self.worker_user, "python3", "-I", PROBE_SCRIPT,
             "--library", self.library, "--volume", self.volume, "--url", path,
             "--hostname", h.split(":")[0], "--scheme", SCHEME, "--site", self._site_id()]
            + (["--ask-socket", self.socket_path] if ask else []),
            capture_output=True, text=True, timeout=60)
        if out.returncode != 0:
            raise AssertionError(f"volume probe failed: {out.stderr.strip()}")
        return json.loads(out.stdout)

    def stored_entry(self, path, host) -> Optional[StoredEntry]:
        p = self._probe(path, host)
        if not p.get("present"):
            return None
        sidecar = [tuple(x) for x in p["sidecar"]] if "sidecar" in p else None
        return StoredEntry(lifetime_s=int(p["lifetime_s"]), sidecar=sidecar)

    def markdown_ready(self, path, host) -> bool:
        # The worker derives a page's copy once a request asks for it; the
        # probe asks as such a request would, without sending one through the
        # server, whose counters the row reads.
        return bool(self._probe(path, host, ask=True).get("markdown_ready"))

    def counters(self) -> Dict[str, int]:
        conn = http.client.HTTPConnection(self.server_config.host, self.server_config.port, timeout=30)
        try:
            conn.request("GET", self.server_config.stats_path + "?PageSpeed=off")
            text = conn.getresponse().read().decode("utf-8", "replace")
        finally:
            conn.close()
        from pagespeed_test_framework.stats import parse_statistics
        return {k: v for k, v in parse_statistics(text).items() if k.startswith("daemon_")}

    def origin_requests(self) -> List[HeaderLines]:
        with self.origin.lock:
            return [list(x) for x in self.origin.seen]

    def purge_url(self, url: str, form: str) -> None:
        if form == "PURGE":
            target = url
            if url.startswith("/"):
                target = url
            r = self._send("PURGE", target, HOST, [], track=False)
        elif form == "query":
            full = url if "://" in url else f"{SCHEME}://{HOST}{url}"
            # The console's purge: POST with X-Requested-With, the URL's path.
            target = urlsplit(full).path.lstrip("/")
            r = self._send("POST", "/pagespeed_admin/cache?purge=" + quote(target, safe=""),
                           HOST, ADMIN_HEADERS, track=False)
        else:
            raise ProviderUnsupported(f"purge form {form}")
        if r.status != 200:
            raise AssertionError(f"purge of {url} ({form}) answered {r.status}: {r.body[:200]!r}")

    def purge_global(self, via: str) -> None:
        if via == "admin":
            r = self._send("POST", "/pagespeed_global_admin/cache?purge=*", HOST,
                           ADMIN_HEADERS, track=False)
            if r.status != 200:
                raise AssertionError(f"global purge answered {r.status}: {r.body[:200]!r}")
        elif via == "flush_file":
            path = os.path.join(self.state_dir, "filecache", "cache.flush")
            subprocess.run(["sudo", "-u", self.worker_user, "touch", path], check=True)
        else:
            raise ProviderUnsupported(f"purge via {via}")

    def set_origin_body(self, body: str) -> None:
        with self.origin.lock:
            self.origin.body = body.encode("utf-8")

    def hold_origin(self) -> None:
        with self.origin.lock:
            self.origin.release = threading.Event()
            self.origin.hold_next = True

    def release_origin(self) -> None:
        with self.origin.lock:
            self.origin.release.set()

    def set_mode(self, mode: str) -> None:
        if mode != self.mode:
            self._write_and_reload(mode)


def make_provider(server_config) -> ApacheProvider:
    return ApacheProvider(server_config)
