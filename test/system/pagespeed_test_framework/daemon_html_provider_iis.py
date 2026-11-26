# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""The IIS provider for the HTML cache conformance rows.

It runs inside the IIS host itself (the rig's lab guest), as Administrator,
next to an IIS the rig provisioned with guest_iis_html_conformance.ps1:

  * one site, "dhc", in its own application pool, bound to
    https://www.example.test:8443 and http://www.example.test:8080, whose
    every path an ASP.NET origin (DhcOrigin.cs) answers with the row's
    status, header lines and body and whose requests it logs;
  * the PageSpeed module with both module entries, PageSpeedFinalView
    first, and the optimizer service in markdown-static mode with
    agent-optimize on.

Each row gets its own CacheFragment, so its keys are a site of their own on
the shared volume. setup() writes the row's site configuration
(pagespeed.config, web.config, bindings, location authentication, the
pool's process count) and recycles the pool; the row starts once a worker
process that did not serve the previous row answers.

settle() waits for a positive signal: the module counts each captured
response whose body ended (daemon_html_iis_commits_started) and each such
response whose commit ran when IIS released the request
(daemon_html_iis_commits_finished); the two agree once every commit of the
responses already answered has run. The counters are the module's shared
statistics, summed over the pool's processes.

The volume is read directly through the optimizer's client library
(ctypes), never with a request, so reading it moves no counter.

Environment (set by the rig's driver, run_iis_html_conformance.py):

  PAGESPEED_DHC_IIS_DIR      the provider's root (C:\\dhc)
  PAGESPEED_DHC_IIS_LIBRARY  the optimizer client library (pagespeed.dll)
  PAGESPEED_DHC_IIS_VOLUME   the optimizer's volume path
  PAGESPEED_DHC_IIS_SOCKET   the optimizer's notification socket name
"""

import ctypes
import hashlib
import http.client
import itertools
import os
import socket
import ssl
import subprocess
import time
from typing import Dict, Iterable, List, Optional, Set, Tuple
from urllib.parse import quote, urlsplit

from pagespeed_test_framework import daemon_html_conformance as conf
from pagespeed_test_framework.stats import parse_statistics

HeaderLines = List[Tuple[str, str]]

ROW_HOST = "www.example.test"
SITE = "dhc"
POOL = "dhc"
HTTP_PORT = 8080
HTTPS_PORT = 8443
ADMIN_PATH = "/pagespeed_admin"
GLOBAL_ADMIN_PATH = "/pagespeed_global_admin"
STATISTICS_PATH = "/pagespeed_admin/statistics"
STARTED = "daemon_html_iis_commits_started"
FINISHED = "daemon_html_iis_commits_finished"
APPCMD = os.path.join(os.environ.get("windir", r"C:\Windows"), "system32", "inetsrv",
                      "appcmd.exe")

SETTLE_TIMEOUT_S = 90.0
RECYCLE_TIMEOUT_S = 90.0
PIN_ATTEMPTS = 40

# The C API's exact-id alternates (pagespeed/system/daemon_abi.h).
ORIGINAL_ID = 0x0C
MARKDOWN_ID = 0x7C
WITHHELD_ID = 0xCC
FACTS_PRESENT = 0x1
FACTS_BINDING_LIVE = 0x200
FACTS_WITHHELD = 0x800
SIDECAR_PAIRED = 0
HASH_BYTES = 32
NOTIFY_FROM_MODULE_VOUCHED_STORED = 0x1 | 0x2 | 0x8
CONTENT_HTML = 0
ASK_INTERVAL_S = 2.0


class ProviderSetupError(RuntimeError):
    """The host does not give this provider what it needs."""


def _env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise ProviderSetupError(f"{name} is not set (see run_iis_html_conformance.py)")
    return value


def _appcmd(*args: str) -> str:
    r = subprocess.run([APPCMD, *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise conf.ProviderUnsupported(f"appcmd {' '.join(args)}: {r.stdout}{r.stderr}".strip())
    return r.stdout


def _xml(s: str) -> str:
    return (s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")
            .replace(">", "&gt;"))


# ---------------------------------------------------------------------------
# The volume, read through the client library


class _PsCacheConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("volume_path", ctypes.c_char_p),
        ("volume_size", ctypes.c_uint64),
        ("enable_checksum", ctypes.c_int),
        ("ram_cache_size", ctypes.c_size_t),
        ("max_metadata_size", ctypes.c_size_t),
    ]


class _PsHtmlStoredFacts(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("flags", ctypes.c_uint32),
        ("stored_at", ctypes.c_uint32),
        ("age_at_store", ctypes.c_uint32),
        ("lifetime_s", ctypes.c_uint32),
        ("render_ttl_s", ctypes.c_uint32),
        ("withheld_reason", ctypes.c_int),
        ("binding_hash", ctypes.c_uint8 * HASH_BYTES),
        ("markdown_hash", ctypes.c_uint8 * HASH_BYTES),
    ]


class _PsNotifyParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("url", ctypes.c_char_p),
        ("hostname", ctypes.c_char_p),
        ("scheme", ctypes.c_char_p),
        ("content_type", ctypes.c_int),
        ("mask", ctypes.c_uint32),
        ("agent_request", ctypes.c_int),
        ("option_context", ctypes.c_char_p),
        ("option_context_length", ctypes.c_size_t),
        ("option_signature", ctypes.c_char_p),
        ("site", ctypes.c_char_p),
        ("notify_flags", ctypes.c_uint32),
    ]


class _Volume:
    """A read handle on the worker's volume, opened as the module opens it,
    fresh for every read (a handle keeps the index as it was at its open)."""

    def __init__(self, lib_path: str, volume_path: str, socket_name: str) -> None:
        self.lib = lib = ctypes.CDLL(lib_path)
        vp, cp = ctypes.c_void_p, ctypes.c_char_p
        lib.ps_read_shared_config_volume_size.restype = ctypes.c_uint64
        lib.ps_read_shared_config_volume_size.argtypes = [cp]
        lib.ps_cache_open.restype = ctypes.c_int
        lib.ps_cache_open.argtypes = [vp, ctypes.POINTER(vp)]
        lib.ps_cache_close.argtypes = [vp]
        lib.ps_cache_read_alternate_site.restype = ctypes.c_int
        lib.ps_cache_read_alternate_site.argtypes = [vp, cp, cp, cp, cp, ctypes.c_uint8,
                                                     ctypes.POINTER(vp)]
        lib.ps_read_origin_max_age.restype = ctypes.c_uint32
        lib.ps_read_origin_max_age.argtypes = [vp]
        lib.ps_read_origin_html_hash.restype = ctypes.POINTER(ctypes.c_uint8)
        lib.ps_read_origin_html_hash.argtypes = [vp]
        lib.ps_read_free.argtypes = [vp]
        lib.ps_headers_sidecar_read.restype = ctypes.c_int
        lib.ps_headers_sidecar_read.argtypes = [vp, cp, cp, cp, cp, ctypes.POINTER(vp)]
        lib.ps_header_block_count.restype = ctypes.c_size_t
        lib.ps_header_block_count.argtypes = [vp]
        lib.ps_header_block_get.restype = ctypes.c_int
        lib.ps_header_block_get.argtypes = [
            vp, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_size_t)]
        lib.ps_header_block_pairing.restype = ctypes.c_int
        lib.ps_header_block_pairing.argtypes = [vp, ctypes.POINTER(ctypes.c_uint8)]
        lib.ps_header_block_free.argtypes = [vp]
        lib.ps_html_stored_facts_from_reads.restype = ctypes.c_int
        lib.ps_html_stored_facts_from_reads.argtypes = [vp, vp, vp, ctypes.c_int,
                                                        ctypes.POINTER(_PsHtmlStoredFacts)]
        lib.ps_notify_params_init_sized.argtypes = [ctypes.POINTER(_PsNotifyParams),
                                                    ctypes.c_size_t]
        lib.ps_notify_worker_ex.restype = ctypes.c_int
        lib.ps_notify_worker_ex.argtypes = [cp, ctypes.POINTER(_PsNotifyParams)]
        self.socket_name = socket_name
        self._asked: Dict[tuple, float] = {}
        self._volume_path = volume_path.encode()
        size = lib.ps_read_shared_config_volume_size(self._volume_path)
        if size == 0:
            raise ProviderSetupError(f"no volume size published for {volume_path}")
        self._config_storage = (ctypes.c_uint8 * (ctypes.sizeof(_PsCacheConfig) * 8))()
        config = _PsCacheConfig.from_buffer(self._config_storage)
        sized = getattr(lib, "ps_cache_config_init_sized", None)
        if sized is not None:
            sized.argtypes = [vp, ctypes.c_size_t]
            sized(ctypes.addressof(config), ctypes.sizeof(_PsCacheConfig))
        else:
            lib.ps_cache_config_init.argtypes = [vp]
            lib.ps_cache_config_init(ctypes.addressof(config))
        config.volume_path = self._volume_path
        config.volume_size = size
        config.ram_cache_size = 0
        self._config = config
        self.cache = ctypes.c_void_p()
        self._open()
        self.close()

    def _open(self) -> None:
        self.cache = ctypes.c_void_p()
        rc = self.lib.ps_cache_open(ctypes.addressof(self._config), ctypes.byref(self.cache))
        if rc != 0 or not self.cache:
            raise conf.ProviderUnsupported(f"cannot open the volume ({rc})")

    def close(self) -> None:
        if self.cache:
            self.lib.ps_cache_close(self.cache)
            self.cache = ctypes.c_void_p()

    def _read(self, key, alt_id: int) -> Optional[ctypes.c_void_p]:
        out = ctypes.c_void_p()
        rc = self.lib.ps_cache_read_alternate_site(self.cache, *key, alt_id, ctypes.byref(out))
        return out if rc == 0 and out else None

    def _free(self, *reads) -> None:
        for r in reads:
            if r is not None:
                self.lib.ps_read_free(r)

    def _block(self, key) -> Optional[ctypes.c_void_p]:
        out = ctypes.c_void_p()
        rc = self.lib.ps_headers_sidecar_read(self.cache, *key, ctypes.byref(out))
        return out if rc == 0 and out else None

    def _block_lines(self, block) -> HeaderLines:
        lines: HeaderLines = []
        for i in range(self.lib.ps_header_block_count(block)):
            name, nlen = ctypes.c_void_p(), ctypes.c_size_t()
            value, vlen = ctypes.c_void_p(), ctypes.c_size_t()
            if self.lib.ps_header_block_get(block, i, ctypes.byref(name), ctypes.byref(nlen),
                                            ctypes.byref(value), ctypes.byref(vlen)) == 0:
                lines.append((ctypes.string_at(name, nlen.value).decode("latin-1"),
                              ctypes.string_at(value, vlen.value).decode("latin-1")))
        return lines

    def stored_entry(self, key) -> Optional[conf.StoredEntry]:
        self._open()
        try:
            original = self._read(key, ORIGINAL_ID)
            if original is None:
                return None
            try:
                lifetime = int(self.lib.ps_read_origin_max_age(original))
                block = self._block(key)
                sidecar = None
                if block is not None:
                    sidecar = self._block_lines(block)
                    self.lib.ps_header_block_free(block)
                return conf.StoredEntry(lifetime, sidecar)
            finally:
                self._free(original)
        finally:
            self.close()

    def markdown_ready(self, key) -> bool:
        """Whether the copy is servable now; a stored page whose copy is not
        is asked for through the worker's socket, as the module asks for a
        lost copy, so nothing passes through the server."""
        self._open()
        try:
            original = self._read(key, ORIGINAL_ID)
            markdown = self._read(key, MARKDOWN_ID)
            withheld = self._read(key, WITHHELD_ID)
            try:
                present = original is not None
                if original is None or markdown is None:
                    ready = False
                else:
                    block = self._block(key)
                    paired = False
                    if block is not None:
                        paired = self.lib.ps_header_block_pairing(
                            block, self.lib.ps_read_origin_html_hash(original)) == SIDECAR_PAIRED
                        self.lib.ps_header_block_free(block)
                    facts = _PsHtmlStoredFacts()
                    facts.struct_size = ctypes.sizeof(facts)
                    rc = self.lib.ps_html_stored_facts_from_reads(
                        original, markdown, withheld, 1 if paired else 0, ctypes.byref(facts))
                    ready = (rc == 0 and facts.flags & FACTS_PRESENT
                             and facts.flags & FACTS_BINDING_LIVE
                             and not facts.flags & FACTS_WITHHELD)
            finally:
                self._free(original, markdown, withheld)
        finally:
            self.close()
        if present and not ready:
            now = time.monotonic()
            if now - self._asked.get(key, -ASK_INTERVAL_S) >= ASK_INTERVAL_S:
                self._asked[key] = now
                self._ask(key)
        return bool(ready)

    def _ask(self, key) -> None:
        path, hostname, scheme, site = key
        n = _PsNotifyParams()
        self.lib.ps_notify_params_init_sized(ctypes.byref(n), ctypes.sizeof(_PsNotifyParams))
        n.url = scheme + b"://" + hostname + path
        n.hostname, n.scheme, n.site = hostname, scheme, site
        n.content_type = CONTENT_HTML
        n.agent_request = 1
        n.notify_flags = NOTIFY_FROM_MODULE_VOUCHED_STORED
        self.lib.ps_notify_worker_ex(self.socket_name.encode(), ctypes.byref(n))


# ---------------------------------------------------------------------------
# The provider


class IisProvider(conf.Provider):
    port = "iis"

    def __init__(self) -> None:
        self.dir = _env("PAGESPEED_DHC_IIS_DIR")
        self.site_dir = os.path.join(self.dir, "site")
        self.state = os.path.join(self.dir, "state")
        self.logs = os.path.join(self.dir, "logs")
        self.cache_dir = os.path.join(self.dir, "cache")
        self.flush_cache_dir = os.path.join(self.dir, "cache-flush")
        self.hold_file = os.path.join(self.state, "commit.hold")
        for d in (self.site_dir, self.state, self.logs, self.cache_dir, self.flush_cache_dir):
            if not os.path.isdir(d):
                raise ProviderSetupError(f"{d} does not exist (guest setup not run)")
        self.volume = _Volume(_env("PAGESPEED_DHC_IIS_LIBRARY"), _env("PAGESPEED_DHC_IIS_VOLUME"),
                              _env("PAGESPEED_DHC_IIS_SOCKET"))
        self._row_seq = itertools.count(1)
        self._auth_locations: Set[str] = set()
        self.processes: Dict[str, int] = {}
        self.max_processes = 1
        self.live: dict = {}
        self.mode = "off"
        self.fragment = "dhc-0"
        self._clear_hold()

    # ---- configuration

    def _uses_flush_file(self, live: dict) -> bool:
        return any(op.get("purge_global", {}).get("via") == "flush_file"
                   for step in live.get("steps", []) for op in step.get("pre", []))

    def _pagespeed_config(self, live: dict, applied: Set[str]) -> str:
        vhost = live.get("vhost", {})
        flush = self._uses_flush_file(live)
        lines = [
            "pagespeed on",
            "pagespeed RewriteLevel PassThrough",
            f"pagespeed CacheFragment {self.fragment}",
            f'pagespeed LogDir "{self.logs}"',
            f'pagespeed FileCachePath "{self.flush_cache_dir if flush else self.cache_dir}"',
            "pagespeed MessageBufferSize 200000",
            "pagespeed Statistics on",
            "pagespeed DaemonSocketPath " + os.environ["PAGESPEED_DHC_IIS_SOCKET"],
            f'pagespeed DaemonVolumePath "{os.environ["PAGESPEED_DHC_IIS_VOLUME"]}"',
            f"pagespeed DaemonHtmlCache {self.mode}",
            f"pagespeed AgentOptimize {'on' if live.get('agent_optimize') else 'off'}",
            f"pagespeed EnableCachePurge {'off' if flush else 'on'}",
        ]
        if not flush:
            lines.append("pagespeed PurgeMethod PURGE")
        applied.update(k for k in ("mode", "agent_optimize") if k in live)
        lines.append(f"pagespeed CacheFlushPollIntervalSec {int(vhost.get('poll_interval_s', 1))}")
        if "poll_interval_s" in vhost:
            applied.add("poll_interval_s")
        if "in_place" in vhost:
            ip = vhost["in_place"]
            if set(ip) - {"enabled"}:
                raise conf.ProviderUnsupported(f"in_place {ip}")
            lines.append(f"pagespeed InPlaceResourceOptimization {'on' if ip['enabled'] else 'off'}")
            applied.add("in_place")
        off = vhost.get("pagespeed_off_location")
        if off:
            # IIS has no per-location PageSpeed switch; the URL is not
            # allowed, which is what the serve gate reads.
            lines.append(f'pagespeed Disallow "*{off}*"')
            applied.add("pagespeed_off_location")
        return "\r\n".join(lines) + "\r\n"

    def _web_config(self, live: dict, applied: Set[str]) -> str:
        vhost = live.get("vhost", {})
        headers = ['<remove name="X-Powered-By" />']
        for knob in ("header_add", "header_always"):
            for name, value in vhost.get(knob, []):
                headers.append(f'<add name="{_xml(name)}" value="{_xml(value)}" />')
            if knob in vhost:
                applied.add(knob)
        compression = ""
        if vhost.get("compress_markdown"):
            if isinstance(vhost["compress_markdown"], dict):
                raise conf.ProviderUnsupported("compress_markdown with options")
            compression = '<urlCompression doDynamicCompression="true" doStaticCompression="true" />'
            applied.add("compress_markdown")
        else:
            compression = '<urlCompression doDynamicCompression="false" doStaticCompression="false" />'
        return f"""<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <system.web>
    <compilation debug="false" targetFramework="4.5" />
    <httpRuntime enableVersionHeader="false" targetFramework="4.5" requestPathInvalidCharacters="" relaxedUrlToFileSystemMapping="true" />
  </system.web>
  <system.webServer>
    <handlers>
      <clear />
      <add name="dhc-origin" path="*" verb="*" type="DhcOrigin" resourceType="Unspecified" preCondition="integratedMode" />
    </handlers>
    <httpProtocol>
      <customHeaders>
        {chr(10).join(headers)}
      </customHeaders>
    </httpProtocol>
    {compression}
  </system.webServer>
</configuration>
"""

    def _apply_site(self, live: dict, applied: Set[str]) -> None:
        vhost = live.get("vhost", {})
        bindings = [f"http/*:{HTTP_PORT}:{ROW_HOST}", f"https/*:{HTTPS_PORT}:{ROW_HOST}"]
        if vhost.get("default_vhost"):
            bindings += [f"http/*:{HTTP_PORT}:", f"https/*:{HTTPS_PORT}:"]
        if "default_vhost" in vhost:
            applied.add("default_vhost")
        for alias in vhost.get("alias", []):
            bindings += [f"http/*:{HTTP_PORT}:{alias}", f"https/*:{HTTPS_PORT}:{alias}"]
        if "alias" in vhost:
            applied.add("alias")
        _appcmd("set", "site", SITE, f"/bindings:{','.join(bindings)}")
        for location in sorted(self._auth_locations):
            for section in ("basicAuthentication", "anonymousAuthentication"):
                subprocess.run([APPCMD, "clear", "config", location,
                                f"-section:system.webServer/security/authentication/{section}",
                                "/commit:apphost"], capture_output=True)
        self._auth_locations = set()
        auth = vhost.get("auth")
        if auth:
            if auth.get("require") != "valid-user" or (auth["user"], auth["password"]) != ("u", "p"):
                raise conf.ProviderUnsupported(f"auth {auth}: the guest has the user u only")
            location = SITE + "/" + auth["location"].strip("/")
            _appcmd("set", "config", location,
                    "-section:system.webServer/security/authentication/basicAuthentication",
                    "/enabled:true", "/commit:apphost")
            _appcmd("set", "config", location,
                    "-section:system.webServer/security/authentication/anonymousAuthentication",
                    "/enabled:false", "/commit:apphost")
            self._auth_locations.add(location)
            applied.add("auth")
        procs = 2 if any("process" in s for s in live.get("steps", [])) else 1
        if procs != self.max_processes:
            _appcmd("set", "apppool", POOL, f"/processModel.maxProcesses:{procs}")
            self.max_processes = procs
        for knob in ("header_early", "expires", "third_party_filter", "merge_slashes",
                     "chained_module_origin"):
            if knob in vhost:
                raise conf.ProviderUnsupported(f"the IIS rig has no {knob}")

    def _write_origin(self, status: int, headers: HeaderLines, body: bytes) -> None:
        with open(os.path.join(self.state, "origin.txt"), "w", encoding="utf-8", newline="\r\n") as f:
            f.write(f"{status}\n")
            for name, value in headers:
                f.write(f"{name}\t{value}\n")
        with open(os.path.join(self.state, "origin.body"), "wb") as f:
            f.write(body)

    def _write_configs(self, live: dict) -> Set[str]:
        applied: Set[str] = set()
        with open(os.path.join(self.site_dir, "pagespeed.config"), "w", encoding="ascii",
                  newline="") as f:
            f.write(self._pagespeed_config(live, applied))
        with open(os.path.join(self.site_dir, "web.config"), "w", encoding="utf-8",
                  newline="\r\n") as f:
            f.write(self._web_config(live, applied))
        self._apply_site(live, applied)
        return applied

    def _recycle(self) -> None:
        before = set(self._pids())
        _appcmd("recycle", "apppool", f"/apppool.name:{POOL}")
        deadline = time.monotonic() + RECYCLE_TIMEOUT_S
        last = ""
        while time.monotonic() < deadline:
            try:
                pid = self._origin_pid()
                if pid not in before:
                    self.processes = {}
                    return
                last = f"pid {pid} still serving"
            except OSError as e:
                last = repr(e)
            except conf.ProviderUnsupported as e:
                last = str(e)
            time.sleep(0.2)
        raise conf.ProviderUnsupported(f"the pool did not recycle ({last})")

    def _pids(self) -> List[int]:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Get-CimInstance Win32_Process -Filter \"Name='w3wp.exe'\" | "
                            "Where-Object { $_.CommandLine -match '-ap \"dhc\"' } | "
                            "ForEach-Object { $_.ProcessId }"],
                           capture_output=True, text=True)
        return [int(x) for x in r.stdout.split() if x.isdigit()]

    def _origin_pid(self, conn: Optional[http.client.HTTPConnection] = None) -> int:
        r = self._send("GET", "/__dhc_pid", ROW_HOST, [], tls=conn is not None, conn=conn)
        if r.status != 200 or not r.body.strip().isdigit():
            raise conf.ProviderUnsupported(f"/__dhc_pid: {r.status} {r.body[:120]!r}")
        return int(r.body.strip())

    def _clear_hold(self) -> None:
        for name in ("hold-next", "held", "commit.hold"):
            path = os.path.join(self.state, name)
            if os.path.exists(path):
                os.unlink(path)

    # ---- Provider

    def setup(self, row_id: str, live: dict, bodies: Dict[str, str]) -> Iterable[str]:
        if "worker" in live:
            raise conf.ProviderUnsupported("worker state per row is not implemented")
        self.live = live
        self.mode = live.get("mode", "off")
        self.fragment = f"dhc-{next(self._row_seq)}-{row_id}"[:60]
        self._clear_hold()
        origin = live.get("origin", {})
        self._write_origin(origin.get("status", 200),
                           [tuple(h) for h in origin.get("headers", [])],
                           bodies[origin["body_ref"]].encode("utf-8"))
        open(os.path.join(self.state, "requests.log"), "w").close()
        applied = self._write_configs(live)
        self._recycle()
        applied |= {f"origin.{k}" for k in origin}
        return applied

    def teardown(self) -> None:
        self._clear_hold()

    def set_mode(self, mode: str) -> None:
        self.mode = mode
        self._write_configs(self.live)
        self._recycle()

    def _connection(self, tls: bool) -> http.client.HTTPConnection:
        if tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            return http.client.HTTPSConnection("127.0.0.1", HTTPS_PORT, timeout=60, context=ctx)
        return http.client.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=60)

    def _send(self, method: str, path: str, host: str, headers: HeaderLines, tls: bool = True,
              conn: Optional[http.client.HTTPConnection] = None) -> conf.Response:
        c = conn or self._connection(tls)
        try:
            c.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
            c.putheader("Host", host)
            for name, value in headers:
                c.putheader(name, value)
            c.endheaders()
            r = c.getresponse()
            body = r.read()
            return conf.Response(r.status, list(r.getheaders()), body)
        finally:
            if conn is None:
                c.close()

    def _pinned_connection(self, process: str) -> http.client.HTTPConnection:
        """A kept-alive connection the named worker process answers on. The
        names are given in the order the processes are first met."""
        for _ in range(PIN_ATTEMPTS):
            c = self._connection(tls=True)
            pid = self._origin_pid(conn=c)
            if process not in self.processes and pid not in self.processes.values():
                self.processes[process] = pid
            if self.processes.get(process) == pid:
                # The request goes on the same connection; the next probe
                # proves the connection stays with the process.
                if self._origin_pid(conn=c) == pid:
                    return c
            c.close()
        raise conf.ProviderUnsupported(f"no connection reached process {process!r}")

    def request(self, method: str, path: str, host: Optional[str], headers: HeaderLines,
                process: Optional[str] = None) -> conf.Response:
        if process is None:
            return self._send(method, path, host or ROW_HOST, headers)
        c = self._pinned_connection(process)
        try:
            return self._send(method, path, host or ROW_HOST, headers, conn=c)
        finally:
            c.close()

    def _stats(self) -> Dict[str, int]:
        r = self._send("GET", STATISTICS_PATH, ROW_HOST, [], tls=False)
        if r.status != 200:
            raise conf.ProviderUnsupported(f"statistics: {r.status}")
        return parse_statistics(r.body.decode("utf-8", "replace"))

    def commits(self) -> Tuple[int, int]:
        s = self._stats()
        return s.get(STARTED, 0), s.get(FINISHED, 0)

    def settle(self) -> None:
        deadline = time.monotonic() + SETTLE_TIMEOUT_S
        while True:
            started, finished = self.commits()
            if started == finished:
                return
            if time.monotonic() >= deadline:
                raise conf.ProviderUnsupported(f"commits never finished: {started} {finished}")
            time.sleep(0.05)

    def hold_commits(self, on: bool) -> None:
        if on:
            open(self.hold_file, "w").close()
        elif os.path.exists(self.hold_file):
            os.unlink(self.hold_file)

    def counters(self) -> Dict[str, int]:
        return {k: v for k, v in self._stats().items() if k.startswith("daemon_")}

    def origin_requests(self) -> List[HeaderLines]:
        out: List[HeaderLines] = []
        lines: HeaderLines = []
        with open(os.path.join(self.state, "requests.log"), encoding="utf-8",
                  errors="replace") as f:
            for raw in f.read().splitlines():
                if raw == "--":
                    out.append(lines)
                    lines = []
                elif "\t" in raw:
                    name, value = raw.split("\t", 1)
                    lines.append((name, value))
        return out

    def _key(self, path: str, host: Optional[str]) -> Tuple[bytes, bytes, bytes, bytes]:
        h = (host or ROW_HOST).lower()
        hostname = h.rsplit(":", 1)[0] if ":" in h and not h.startswith("[") else h
        site = hashlib.sha256(self.fragment.encode()).hexdigest()
        return (path.encode(), hostname.encode(), b"https", site.encode())

    def stored_entry(self, path: str, host: Optional[str]) -> Optional[conf.StoredEntry]:
        return self.volume.stored_entry(self._key(path, host))

    def markdown_ready(self, path: str, host: Optional[str]) -> bool:
        return self.volume.markdown_ready(self._key(path, host))

    def _admin_post(self, target: str) -> None:
        r = self._send("POST", target, ROW_HOST,
                       [("X-Requested-With", "XMLHttpRequest"), ("Content-Length", "0")],
                       tls=False)
        if r.status != 200:
            raise conf.ProviderUnsupported(f"POST {target}: {r.status} {r.body[:200]!r}")

    def purge_url(self, url: str, form: str) -> None:
        parts = urlsplit(url)
        if parts.scheme:
            absolute = url
            tls = parts.scheme == "https"
            host = parts.netloc
            path = parts.path + (f"?{parts.query}" if parts.query else "")
        else:
            absolute = f"https://{ROW_HOST}{url}"
            tls, host, path = True, ROW_HOST, url
        if form == "PURGE":
            r = self._send("PURGE", path, host, [], tls=tls)
            if r.status != 200:
                raise conf.ProviderUnsupported(f"PURGE {url}: {r.status} {r.body[:200]!r}")
        elif form == "query":
            self._admin_post(f"{ADMIN_PATH}/cache?purge={quote(absolute, safe='')}")
        else:
            raise conf.ProviderUnsupported(f"purge form {form!r}")

    def purge_global(self, via: str) -> None:
        if via == "admin":
            self._admin_post(f"{GLOBAL_ADMIN_PATH}/cache?purge=*")
        elif via == "flush_file":
            path = os.path.join(self.flush_cache_dir, "cache.flush")
            with open(path, "a"):
                pass
            now = time.time()
            os.utime(path, (now, now))
        else:
            raise conf.ProviderUnsupported(f"global purge via {via!r}")

    def set_origin_body(self, body: str) -> None:
        with open(os.path.join(self.state, "origin.body"), "wb") as f:
            f.write(body.encode("utf-8"))

    def hold_origin(self) -> None:
        open(os.path.join(self.state, "held"), "w").close()
        open(os.path.join(self.state, "hold-next"), "w").close()

    def release_origin(self) -> None:
        held = os.path.join(self.state, "held")
        if os.path.exists(held):
            os.unlink(held)


def make_provider(server_config=None) -> IisProvider:
    del server_config  # The provider runs on the IIS host; see the module doc.
    return IisProvider()
