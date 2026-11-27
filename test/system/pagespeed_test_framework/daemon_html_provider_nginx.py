# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""The nginx lane's provider for the HTML cache conformance rows.

The provider runs servers of its own, apart from the lane's main nginx, so a
row's configuration, purges and worker state touch nothing another test
reads:

  * one optimizer worker, in markdown-static mode with agent-optimize on,
    with its own socket and volume;
  * two nginx masters, "a" and "b", each with one worker process, on the
    same file cache and the same volume: a step without a `process` goes to
    "a", and the cross-process rows send "a" and "b" where they say;
  * one origin, in this process, answering every path with the row's
    status, headers and body, and logging the header lines it received.

Each instance has a TLS listener (the row's server, https://www.example.test),
a plain listener for the same server (a purge naming http://), a plain
listener for the module in front of which a chained row puts this one, and a
loopback listener for the generation probe and the global statistics.

Every row gets its own CacheFragment, so its keys are a site of their own on
the shared volume and nothing a previous row stored can be read for it. A
row's configuration is written and both instances reloaded in setup(); a
reload counts as done when the new generation answers the probe.

settle() waits for a positive signal: every commit the module posted has
finished (the daemon_html_commits_posted and daemon_html_commits_finished
counters of each instance agree). The volume is read directly through the
optimizer's client library (ctypes), never with a request, so reading it moves
no counter.

Environment (exported by run_nginx_tests.sh with the daemon_html_conformance
lane fixture):

  PAGESPEED_DHC_NGINX_BINARY  the nginx binary
  PAGESPEED_DHC_MODULE        the module (ngx_pagespeed_module.so)
  PAGESPEED_DHC_DAEMON_DIR    factory_worker and libpagespeed.so
  PAGESPEED_DHC_NGINX_SRC     the nginx source tree the binary was built from
                              (the lane's test filter module is built
                              against it)
  PAGESPEED_DHC_DIR           a scratch directory this provider owns
  PAGESPEED_DHC_PORT_BASE     the origin's port; the instances take the next
                              eight
"""

import atexit
import ctypes
import hashlib
import http.client
import itertools
import os
import shutil
import signal
import ssl
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, Iterable, List, Optional, Set, Tuple
from urllib.parse import quote, urlsplit

from pagespeed_test_framework import daemon_html_conformance as conf
from pagespeed_test_framework.stats import parse_statistics

HeaderLines = List[Tuple[str, str]]

ROW_HOST = "www.example.test"
ADMIN_PATH = "/pagespeed_admin"
GLOBAL_ADMIN_PATH = "/pagespeed_global_admin"
GLOBAL_STATISTICS_PATH = "/pagespeed_global_statistics"
# While this file exists, every HTML cache commit of the provider's servers
# waits on its commit thread (the module reads the variable naming it).
HOLD_ENV = "PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE"
POSTED = "daemon_html_commits_posted"
FINISHED = "daemon_html_commits_finished"

SETTLE_TIMEOUT_S = 90.0
RELOAD_TIMEOUT_S = 30.0

# The C API's exact-id alternates (pagespeed/system/daemon_abi.h).
ORIGINAL_ID = 0x0C
MARKDOWN_ID = 0x7C
WITHHELD_ID = 0xCC
# The brotli and the gzip copy of the Markdown copy.
CODED_IDS = {"br": 0xDC, "gzip": 0xEC}
# The worker's default compression levels, which restart_worker restores
# after a row.
WORKER_GZIP_LEVEL = 6
WORKER_BROTLI_LEVEL = 6
FACTS_PRESENT = 0x1
FACTS_BINDING_LIVE = 0x200
FACTS_WITHHELD = 0x800
SIDECAR_PAIRED = 0
HASH_BYTES = 32


class ProviderSetupError(RuntimeError):
    """The lane does not give this provider what it needs."""


def _env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise ProviderSetupError(f"{name} is not set (lane fixture daemon_html_conformance)")
    return value


def _pid_is(pid: int, needle: str) -> bool:
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return needle.encode() in f.read()
    except OSError:
        return False


# ---------------------------------------------------------------------------
# The origin


class _OriginState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.status = 200
        self.headers: HeaderLines = []
        self.body = b""
        self.hold_next = False
        self.release = threading.Event()
        self.held: List[threading.Event] = []
        # (method, path) of each response held now (headers out, body
        # waiting).
        self.holding: List[Tuple[str, str]] = []
        self.requests: List[HeaderLines] = []

    def reset(self, status: int, headers: HeaderLines, body: bytes) -> None:
        with self.lock:
            self.status = status
            self.headers = list(headers)
            self.body = body
            self.hold_next = False
            self.requests = []
            for e in self.held:
                e.set()
            self.held = []


class _OriginHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    state: _OriginState = _OriginState()

    def _answer(self, send_body: bool) -> None:
        st = self.state
        with st.lock:
            st.requests.append([(n, v) for n, v in self.headers.items()])
            status, headers, body = st.status, list(st.headers), st.body
            gate = None
            if st.hold_next:
                st.hold_next = False
                gate = threading.Event()
                st.held.append(gate)
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        # A single byte range is answered as an origin that supports ranges
        # would; anything else gets the whole body.
        content_range = None
        spec = self.headers.get("Range", "")
        if status == 200 and spec.startswith("bytes=") and "," not in spec:
            first, _, last = spec[len("bytes="):].partition("-")
            if first.isdigit() and (last.isdigit() or last == "") and int(first) < len(body):
                end = min(int(last), len(body) - 1) if last else len(body) - 1
                content_range = f"bytes {first}-{end}/{len(body)}"
                body = body[int(first):end + 1]
                status = 206
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        if content_range is not None:
            self.send_header("Content-Range", content_range)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.flush()
        if gate is not None:
            # The headers are out; the body waits for release_origin, so the
            # server has started the response (and its capture) before it.
            held = (self.command, self.path)
            with st.lock:
                st.holding.append(held)
            try:
                gate.wait(120)
            finally:
                with st.lock:
                    if held in st.holding:
                        st.holding.remove(held)
        if send_body:
            self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        self._answer(True)

    def do_HEAD(self):  # noqa: N802
        self._answer(False)

    def do_POST(self):  # noqa: N802
        self._answer(True)

    def log_message(self, *args):  # quiet
        pass


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


# from_module | host_vouched | html_stored, as the module's lost-copy notify.
NOTIFY_FROM_MODULE_VOUCHED_STORED = 0x1 | 0x2 | 0x8
CONTENT_HTML = 0
FACTS_NEEDS_VALIDATION = 0x2
FACTS_VARIANT_PRESENT = 0x100
# A page whose copy is not servable yet is asked for at most this often.
ASK_INTERVAL_S = 2.0


class _Volume:
    """A read handle on the worker's volume, opened as the module opens it."""

    def __init__(self, lib_path: str, volume_path: str) -> None:
        self.lib = lib = ctypes.CDLL(lib_path)
        vp = ctypes.c_void_p
        cp = ctypes.c_char_p
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
        lib.ps_read_content.restype = ctypes.c_int
        lib.ps_read_content.argtypes = [vp, ctypes.POINTER(ctypes.c_void_p),
                                        ctypes.POINTER(ctypes.c_size_t)]
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
        lib.ps_read_cache_inserted_at.restype = ctypes.c_uint32
        lib.ps_read_cache_inserted_at.argtypes = [vp]
        lib.ps_cache_read_site_purge_epoch.restype = ctypes.c_int
        lib.ps_cache_read_site_purge_epoch.argtypes = [vp, cp, ctypes.POINTER(ctypes.c_uint64)]
        lib.ps_notify_params_init_sized.argtypes = [ctypes.POINTER(_PsNotifyParams),
                                                    ctypes.c_size_t]
        lib.ps_notify_worker_ex.restype = ctypes.c_int
        lib.ps_notify_worker_ex.argtypes = [cp, ctypes.POINTER(_PsNotifyParams)]
        self.ask_socket: Optional[str] = None
        self._asked: Dict[tuple, float] = {}
        lib.ps_html_stored_facts_from_reads.restype = ctypes.c_int
        lib.ps_html_stored_facts_from_reads.argtypes = [vp, vp, vp, ctypes.c_int,
                                                        ctypes.POINTER(_PsHtmlStoredFacts)]

        self._volume_path = volume_path.encode()
        size = lib.ps_read_shared_config_volume_size(self._volume_path)
        if size == 0:
            raise ProviderSetupError(f"no volume size published for {volume_path}")
        # Over-allocated as the module does: an older initializer writes its
        # own struct's worth.
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
        # One open to prove the volume can be read at all.
        self._open()
        self.close()

    def _open(self) -> None:
        # A handle held across another process's writes keeps the volume's
        # index as it was at the open, so every read takes a fresh handle.
        self.cache = ctypes.c_void_p()
        rc = self.lib.ps_cache_open(ctypes.addressof(self._config), ctypes.byref(self.cache))
        if rc != 0 or not self.cache:
            raise conf.ProviderUnsupported(f"cannot open the volume ({rc})")

    def close(self) -> None:
        if self.cache:
            self.lib.ps_cache_close(self.cache)
            self.cache = ctypes.c_void_p()

    def stored_entry(self, key) -> Optional[conf.StoredEntry]:
        self._open()
        try:
            return self._stored_entry(key)
        finally:
            self.close()

    def markdown_ready(self, key) -> bool:
        """Whether the copy is servable now. A stored page whose copy is not
        is asked for through the worker's socket, as the module asks when a
        request for the copy finds none (an agent request for a page this
        module stored for a host it vouched for); nothing passes through the
        server, so no counter moves."""
        self._open()
        try:
            ready, present = self._markdown_ready(key)
        finally:
            self.close()
        if present and not ready and self.ask_socket:
            now = time.monotonic()
            if now - self._asked.get(key, -ASK_INTERVAL_S) >= ASK_INTERVAL_S:
                self._asked[key] = now
                self._ask(key)
        return ready

    def coded_ready(self, key, codings) -> bool:
        """Whether the volume holds each named coded copy of the key's
        Markdown copy with bytes (an entry without bytes is the optimizer's
        record that the coding is not smaller)."""
        self._open()
        try:
            for coding in codings:
                read = self._read(key, CODED_IDS[coding])
                if read is None:
                    return False
                try:
                    data, size = ctypes.c_void_p(), ctypes.c_size_t(0)
                    if self.lib.ps_read_content(read, ctypes.byref(data),
                                                ctypes.byref(size)) != 0 or size.value == 0:
                        return False
                finally:
                    self._free(read)
            return True
        finally:
            self.close()

    def _ask(self, key) -> None:
        path, hostname, scheme, site = key
        n = _PsNotifyParams()
        self.lib.ps_notify_params_init_sized(ctypes.byref(n), ctypes.sizeof(_PsNotifyParams))
        n.url = scheme + b"://" + hostname + path
        n.hostname, n.scheme, n.site = hostname, scheme, site
        n.content_type = CONTENT_HTML
        n.agent_request = 1
        n.notify_flags = NOTIFY_FROM_MODULE_VOUCHED_STORED
        self.lib.ps_notify_worker_ex(self.ask_socket.encode(), ctypes.byref(n))

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

    def _stored_entry(self, key) -> Optional[conf.StoredEntry]:
        original = self._read(key, ORIGINAL_ID)
        if os.environ.get("PAGESPEED_DHC_DEBUG"):
            print(f"dhc stored_entry {key!r}: {original is not None}", flush=True)
        if original is None:
            return None
        try:
            lifetime = int(self.lib.ps_read_origin_max_age(original))
            block = self._block(key)
            sidecar = None
            if block is not None:
                try:
                    sidecar = self._block_lines(block)
                finally:
                    self.lib.ps_header_block_free(block)
            return conf.StoredEntry(lifetime_s=lifetime, sidecar=sidecar)
        finally:
            self._free(original)

    def _markdown_ready(self, key) -> Tuple[bool, bool]:
        original = self._read(key, ORIGINAL_ID)
        markdown = self._read(key, MARKDOWN_ID)
        withheld = self._read(key, WITHHELD_ID)
        block = None
        try:
            if original is None:
                return False, False
            if markdown is None:
                if os.environ.get("PAGESPEED_DHC_DEBUG"):
                    print(f"dhc markdown_ready {key!r}: original={original is not None} "
                          f"markdown={markdown is not None}", flush=True)
                return False, True
            block = self._block(key)
            paired = block is not None and self.lib.ps_header_block_pairing(
                block, self.lib.ps_read_origin_html_hash(original)) == SIDECAR_PAIRED
            facts = _PsHtmlStoredFacts()
            facts.struct_size = ctypes.sizeof(_PsHtmlStoredFacts)
            rc = self.lib.ps_html_stored_facts_from_reads(
                original, markdown, withheld, 1 if paired else 0, ctypes.byref(facts))
            if os.environ.get("PAGESPEED_DHC_DEBUG"):
                print(f"dhc markdown_ready {key[0]!r}: rc={rc} paired={paired} "
                      f"withheld={withheld is not None} flags={facts.flags:#x}", flush=True)
            if rc != 0:
                return False, True
            need = FACTS_PRESENT | FACTS_VARIANT_PRESENT | FACTS_BINDING_LIVE
            if (facts.flags & need) != need:
                return False, True
            if facts.flags & (FACTS_WITHHELD | FACTS_NEEDS_VALIDATION):
                return False, True
            if time.time() - facts.stored_at >= facts.lifetime_s:
                return False, True
            epoch = ctypes.c_uint64(0)
            if self.lib.ps_cache_read_site_purge_epoch(self.cache, key[3],
                                                       ctypes.byref(epoch)) != 0:
                return False, True
            if facts.stored_at <= epoch.value:
                return False, True
            data, size = ctypes.c_void_p(), ctypes.c_size_t()
            if self.lib.ps_read_content(markdown, ctypes.byref(data), ctypes.byref(size)) != 0:
                return False, True
            return size.value > 0, True
        finally:
            if block is not None:
                self.lib.ps_header_block_free(block)
            self._free(original, markdown, withheld)


# ---------------------------------------------------------------------------
# The lane's test filter module


def _build_lane_filters(nginx_bin: str, nginx_src: str, out_dir: str) -> Dict[str, str]:
    """Builds test/system/nginx_lane_filter (and, when it builds, nginx's own
    sub filter) as dynamic modules for this nginx. Returns name -> .so."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lane_src = os.path.join(here, "nginx_lane_filter")
    done = os.path.join(out_dir, "modules")
    built = {}
    for name in ("ngx_http_lane_filter_module", "ngx_http_sub_filter_module"):
        path = os.path.join(done, f"{name}.so")
        if os.path.exists(path):
            built[name] = path
    if "ngx_http_lane_filter_module" in built:
        return built
    version = subprocess.run([nginx_bin, "-V"], capture_output=True, text=True)
    args = []
    for line in version.stderr.splitlines():
        if line.startswith("configure arguments:"):
            args = line[len("configure arguments:"):].split()
    if "--with-compat" not in args:
        raise ProviderSetupError("the lane nginx is not built --with-compat")

    sub_dir = os.path.join(out_dir, "sub_addon")
    os.makedirs(sub_dir, exist_ok=True)
    sub_c = os.path.join(nginx_src, "src/http/modules/ngx_http_sub_filter_module.c")
    if os.path.exists(sub_c):
        shutil.copy(sub_c, sub_dir)
        with open(os.path.join(sub_dir, "config"), "w") as f:
            f.write("ngx_addon_name=ngx_http_sub_filter_module\n"
                    "ngx_module_type=HTTP_FILTER\n"
                    "ngx_module_name=ngx_http_sub_filter_module\n"
                    "ngx_module_srcs=\"$ngx_addon_dir/ngx_http_sub_filter_module.c\"\n"
                    ". auto/module\n")

    def attempt(addons: List[str]) -> bool:
        tree = os.path.join(out_dir, "nginx-src")
        shutil.rmtree(tree, ignore_errors=True)
        shutil.copytree(nginx_src, tree, symlinks=True,
                        ignore=shutil.ignore_patterns("objs"))
        cmd = ["./configure"] + args
        for a in addons:
            cmd.append(f"--add-dynamic-module={a}")
        log = open(os.path.join(out_dir, "build.log"), "a")
        try:
            if subprocess.run(cmd, cwd=tree, stdout=log, stderr=log).returncode != 0:
                return False
            if subprocess.run(["make", "-j8", "modules"], cwd=tree, stdout=log,
                              stderr=log).returncode != 0:
                return False
        finally:
            log.close()
        os.makedirs(done, exist_ok=True)
        for so in os.listdir(os.path.join(tree, "objs")):
            if so.endswith(".so"):
                shutil.copy(os.path.join(tree, "objs", so), done)
        return True

    if not (os.path.exists(sub_c) and attempt([lane_src, sub_dir])):
        if not attempt([lane_src]):
            raise ProviderSetupError(
                f"cannot build the lane filter module (see {out_dir}/build.log)")
    for name in ("ngx_http_lane_filter_module", "ngx_http_sub_filter_module"):
        path = os.path.join(done, f"{name}.so")
        if os.path.exists(path):
            built[name] = path
    return built


# ---------------------------------------------------------------------------
# The servers


class _Instance:
    def __init__(self, name: str, root: str, ports: Dict[str, int]) -> None:
        self.name = name
        self.root = root
        self.ports = ports
        self.conf_path = os.path.join(root, "nginx.conf")
        self.pid_path = os.path.join(root, "nginx.pid")
        self.generation = 0
        # Requests this provider sent the instance since the row's setup,
        # and where the row's part of its access log starts.
        self.sent = 0
        self.purges = 0
        self.log_offset = 0
        for d in ("logs", "client_body_temp", "proxy_temp", "fastcgi_temp",
                  "uwsgi_temp", "scgi_temp"):
            os.makedirs(os.path.join(root, d), exist_ok=True)

    def master_pid(self) -> Optional[int]:
        try:
            with open(self.pid_path) as f:
                pid = int(f.read().strip())
        except (OSError, ValueError):
            return None
        return pid if _pid_is(pid, self.conf_path) else None


class NginxProvider(conf.Provider):
    port = "nginx"

    def __init__(self) -> None:
        self.nginx_bin = _env("PAGESPEED_DHC_NGINX_BINARY")
        self.module = _env("PAGESPEED_DHC_MODULE")
        self.daemon_dir = _env("PAGESPEED_DHC_DAEMON_DIR")
        self.dir = _env("PAGESPEED_DHC_DIR")
        nginx_src = _env("PAGESPEED_DHC_NGINX_SRC")
        base = int(_env("PAGESPEED_DHC_PORT_BASE"))
        for path in (self.nginx_bin, self.module,
                     os.path.join(self.daemon_dir, "factory_worker"),
                     os.path.join(self.daemon_dir, "libpagespeed.so")):
            if not os.path.exists(path):
                raise ProviderSetupError(f"{path} does not exist")
        os.makedirs(self.dir, exist_ok=True)
        self.hold_file = os.path.join(self.dir, "commit.hold")
        if os.path.exists(self.hold_file):
            os.unlink(self.hold_file)
        self.cache_dir = os.path.join(self.dir, "file_cache")
        self.flush_cache_dir = os.path.join(self.dir, "file_cache_flush")
        for d in (self.cache_dir, self.flush_cache_dir):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        self.filters = _build_lane_filters(self.nginx_bin, nginx_src,
                                           os.path.join(self.dir, "lane_filter"))
        self._make_cert()
        self.origin_port = base
        self.instances = {
            "a": _Instance("a", os.path.join(self.dir, "nginx_a"),
                           {"tls": base + 1, "plain": base + 2, "inner": base + 3,
                            "probe": base + 4}),
            "b": _Instance("b", os.path.join(self.dir, "nginx_b"),
                           {"tls": base + 5, "plain": base + 6, "inner": base + 7,
                            "probe": base + 8}),
        }
        self._row_seq = itertools.count(1)
        self._started: List[Tuple[int, str]] = []
        atexit.register(self.shutdown)
        self._start_origin()
        self._start_worker()
        self.volume = _Volume(os.path.join(self.daemon_dir, "libpagespeed.so"),
                              self.volume_path)
        self.volume.ask_socket = self.socket_path
        self.row = None
        self.live: dict = {}
        self.mode = "off"
        self._write_configs(None)
        for inst in self.instances.values():
            self._start_nginx(inst)

    # ---- process management

    def _make_cert(self) -> None:
        self.cert = os.path.join(self.dir, "server.crt")
        self.key = os.path.join(self.dir, "server.key")
        if os.path.exists(self.cert) and os.path.exists(self.key):
            return
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", self.key,
             "-out", self.cert, "-days", "30", "-nodes", "-subj", f"/CN={ROW_HOST}"],
            check=True, capture_output=True)

    def _start_origin(self) -> None:
        self.origin = _OriginState()
        handler = type("_RowOriginHandler", (_OriginHandler,), {"state": self.origin})
        self._origin_server = ThreadingHTTPServer(("127.0.0.1", self.origin_port), handler)
        self._origin_server.daemon_threads = True
        threading.Thread(target=self._origin_server.serve_forever, daemon=True).start()

    def _start_worker(self, fresh: bool = True,
                      levels: Tuple[int, int] = (WORKER_GZIP_LEVEL, WORKER_BROTLI_LEVEL)) -> None:
        """Start the worker; `fresh` on an empty volume, else on the one it
        had, with the compression levels (gzip, brotli) given."""
        wdir = os.path.join(self.dir, "worker")
        cache_root = os.path.join(wdir, "cache")
        if fresh:
            shutil.rmtree(wdir, ignore_errors=True)
            os.makedirs(cache_root)
        self.socket_path = os.path.join(wdir, "notify.sock")
        self.volume_path = os.path.join(cache_root, "cache")
        if not fresh and os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        self.worker_levels = levels
        log = open(os.path.join(self.dir, "worker.log"), "a")
        proc = subprocess.Popen(
            [os.path.join(self.daemon_dir, "factory_worker"),
             "--cache-dir", cache_root, "--socket", self.socket_path,
             "--cache-size", "536870912", "--html-mode", "markdown-static",
             "--agent-optimize", "--log-level", "info",
             "--gzip-level", str(levels[0]), "--brotli-level", str(levels[1])],
            stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True,
            env=dict(os.environ, LD_LIBRARY_PATH=self.daemon_dir))
        self._started.append((proc.pid, "factory_worker"))
        self._worker = proc
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            shared = False
            for dirpath, _, files in os.walk(wdir):
                if "pagespeed-shared.conf" in files:
                    shared = True
            if os.path.exists(self.socket_path) and shared:
                return
            if proc.poll() is not None:
                break
            time.sleep(0.2)
        raise ProviderSetupError(f"the optimizer worker did not start (see {self.dir}/worker.log)")

    def _nginx_env(self) -> Dict[str, str]:
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = self.daemon_dir + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        env[HOLD_ENV] = self.hold_file
        return env

    def _nginx(self, inst: _Instance, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([self.nginx_bin, "-p", inst.root, "-c", inst.conf_path, *args],
                              capture_output=True, text=True, env=self._nginx_env())

    def _start_nginx(self, inst: _Instance) -> None:
        r = self._nginx(inst)
        if r.returncode != 0:
            raise ProviderSetupError(f"nginx {inst.name} did not start: {r.stderr.strip()}")
        pid = inst.master_pid()
        if pid is None:
            raise ProviderSetupError(f"nginx {inst.name} wrote no pid file")
        self._started.append((pid, inst.conf_path))
        self._wait_generation(inst)

    def _accepting_workers(self, inst: _Instance) -> int:
        """Worker processes of the instance's master that still accept."""
        master = inst.master_pid()
        if master is None:
            return 0
        n = 0
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/stat") as f:
                    ppid = int(f.read().rsplit(")", 1)[1].split()[1])
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    cmd = f.read().replace(b"\0", b" ").strip()
            except (OSError, ValueError, IndexError):
                continue
            if ppid == master and cmd == b"nginx: worker process":
                n += 1
        return n

    def _wait_generation(self, inst: _Instance) -> None:
        deadline = time.monotonic() + RELOAD_TIMEOUT_S
        last = ""
        while time.monotonic() < deadline:
            try:
                conn = http.client.HTTPConnection("127.0.0.1", inst.ports["probe"], timeout=5)
                conn.request("GET", "/__dhc_probe", headers={"Host": "localhost"})
                r = conn.getresponse()
                r.read()
                conn.close()
                last = r.getheader("X-Dhc-Gen", "")
                # The new generation answers, but a worker of an older one
                # keeps accepting on the shared listeners until it starts to
                # exit (closes them and retitles itself "... is shutting
                # down"); a row request it took would run the previous row's
                # configuration. So the new generation counts only once it is
                # the one accepting worker left.
                if last == str(inst.generation) and self._accepting_workers(inst) == 1:
                    return
            except OSError as e:
                last = repr(e)
            time.sleep(0.05)
        raise conf.ProviderUnsupported(
            f"nginx {inst.name} did not serve generation {inst.generation} ({last})")

    def _reload(self) -> None:
        self._write_configs(self.row)
        for inst in self.instances.values():
            r = self._nginx(inst, "-s", "reload")
            if r.returncode != 0:
                raise conf.ProviderUnsupported(f"nginx {inst.name} reload: {r.stderr.strip()}")
        for inst in self.instances.values():
            self._wait_generation(inst)

    def shutdown(self) -> None:
        for inst in self.instances.values():
            pid = inst.master_pid()
            if pid is not None and (pid, inst.conf_path) in self._started:
                os.kill(pid, signal.SIGQUIT)
                for _ in range(100):
                    if not _pid_is(pid, inst.conf_path):
                        break
                    time.sleep(0.1)
        if getattr(self, "volume", None) is not None:
            self.volume.close()
        worker = getattr(self, "_worker", None)
        if worker is not None and worker.poll() is None and _pid_is(worker.pid, "factory_worker"):
            worker.terminate()
            try:
                worker.wait(10)
            except subprocess.TimeoutExpired:
                worker.kill()
        server = getattr(self, "_origin_server", None)
        if server is not None:
            server.shutdown()
            server.server_close()
            self._origin_server = None
        self._started = []

    # ---- configuration

    def _uses_flush_file(self, live: dict) -> bool:
        for step in live.get("steps", []):
            for op in step.get("pre", []):
                if op.get("purge_global", {}).get("via") == "flush_file":
                    return True
        return False

    def _row_server(self, inst: _Instance, live: dict) -> Tuple[str, Set[str]]:
        """The row's server block (and any server it needs behind it)."""
        applied: Set[str] = set()
        vhost = live.get("vhost", {})
        p = inst.ports
        default = " default_server" if vhost.get("default_vhost") else ""
        if "default_vhost" in vhost:
            applied.add("default_vhost")
        names = [ROW_HOST] + list(vhost.get("alias", []))
        if "alias" in vhost:
            applied.add("alias")
        upstream = f"http://127.0.0.1:{self.origin_port}"
        inner = ""
        if vhost.get("chained_module_origin"):
            applied.add("chained_module_origin")
            upstream = f"http://127.0.0.1:{p['inner']}"
            inner = f"""
    server {{
        listen 127.0.0.1:{p['inner']};
        server_name {ROW_HOST};
        pagespeed on;
        pagespeed RewriteLevel PassThrough;
        pagespeed CacheFragment {self.fragment}-inner;
        pagespeed AgentOptimize on;
        pagespeed DaemonHtmlCache serve-variants;
        location / {{
            proxy_pass http://127.0.0.1:{self.origin_port};
            proxy_set_header Host $host;
        }}
    }}
"""
        lines: List[str] = []
        flush = self._uses_flush_file(live)
        if flush:
            lines += ["pagespeed EnableCachePurge off;",
                      f"pagespeed FileCachePath {self.flush_cache_dir};"]
        else:
            lines += ["pagespeed EnableCachePurge on;", "pagespeed PurgeMethod PURGE;"]
        lines.append(f"pagespeed DaemonHtmlCache {self.mode};")
        if "mode" in live:
            applied.add("mode")
        lines.append(f"pagespeed AgentOptimize {'on' if live.get('agent_optimize') else 'off'};")
        if "agent_optimize" in live:
            applied.add("agent_optimize")
        for name, value in vhost.get("header_add", []):
            lines.append(f'add_header {name} "{value}";')
        if "header_add" in vhost:
            applied.add("header_add")
        for name, value in vhost.get("header_always", []):
            lines.append(f'add_header {name} "{value}" always;')
        if "header_always" in vhost:
            applied.add("header_always")
        if "expires" in vhost:
            lines.append(f"expires {vhost['expires']};")
            applied.add("expires")
        if "in_place" in vhost:
            ip = vhost["in_place"]
            if "enabled" in ip:
                lines.append(
                    f"pagespeed InPlaceResourceOptimization {'on' if ip['enabled'] else 'off'};")
            if "s_maxage" in ip:
                lines.append("pagespeed InPlaceResourceOptimization on;")
                lines.append(f"pagespeed InPlaceSMaxAgeSec {int(ip['s_maxage'])};")
            applied.add("in_place")
        cm = vhost.get("compress_markdown")
        if cm:
            lines += ["gzip on;", "gzip_types text/markdown;", "gzip_vary on;",
                      "gzip_min_length 1;"]
            if isinstance(cm, dict) and cm.get("ignores_no_compress"):
                lines.append("lane_filter_force_encoding on;")
            applied.add("compress_markdown")
        if vhost.get("merge_slashes") is False:
            lines.append("merge_slashes off;")
            applied.add("merge_slashes")
        elif "merge_slashes" in vhost:
            applied.add("merge_slashes")
        if "poll_interval_s" in vhost:
            lines.append(f"pagespeed CacheFlushPollIntervalSec {int(vhost['poll_interval_s'])};")
            applied.add("poll_interval_s")
        tpf = vhost.get("third_party_filter")
        if tpf:
            for name, value in tpf.get("adds", []):
                lines.append(f'lane_filter_add_header {name} "{value}";')
            if "rewrites_body" in tpf:
                if "ngx_http_sub_filter_module" in self.filters:
                    lines += [f"sub_filter_types {tpf['rewrites_body']};",
                              "sub_filter_once off;", "sub_filter ' ' '  ';"]
                else:
                    lines.append(f"lane_filter_rewrite_body {tpf['rewrites_body']};")
            applied.add("third_party_filter")

        for name in vhost.get("proxy_hide", []):
            lines.append(f"proxy_hide_header {name};")
        if "proxy_hide" in vhost:
            applied.add("proxy_hide")
        for name, value in vhost.get("trailer", []):
            lines.append(f'add_trailer {name} "{value}" always;')
        if "trailer" in vhost:
            applied.add("trailer")

        proxy = f"proxy_pass {upstream};\n            proxy_set_header Host $host;"
        if vhost.get("named_location"):
            # The page comes from a named location, after an internal
            # redirect: try_files finds no file and falls back to @app.
            locations = [f"""
        location / {{
            root {self.dir}/empty-root;
            try_files /nonexistent @app;
        }}
        location @app {{
            {proxy}
        }}"""]
            os.makedirs(os.path.join(self.dir, "empty-root"), exist_ok=True)
            applied.add("named_location")
        else:
            locations = [f"""
        location / {{
            {proxy}
        }}"""]
        auth = vhost.get("auth")
        if auth:
            htpasswd = os.path.join(self.dir, f"htpasswd-{self.fragment}")
            with open(htpasswd, "w") as f:
                f.write(f"{auth['user']}:{{PLAIN}}{auth['password']}\n")
            locations.append(f"""
        location {auth['location']} {{
            auth_basic "lane";
            auth_basic_user_file {htpasswd};
            {proxy}
        }}""")
            applied.add("auth")
        off = vhost.get("pagespeed_off_location")
        if off:
            locations.append(f"""
        location {off} {{
            pagespeed off;
            {proxy}
        }}""")
            applied.add("pagespeed_off_location")
        body = "\n        ".join(lines)
        server = f"""
    server {{
        listen 127.0.0.1:{p['tls']} ssl{default};
        listen 127.0.0.1:{p['plain']}{default};
        server_name {' '.join(names)};
        ssl_certificate {self.cert};
        ssl_certificate_key {self.key};

        pagespeed on;
        pagespeed RewriteLevel PassThrough;
        pagespeed CacheFragment {self.fragment};
        pagespeed DaemonSocketPath {self.socket_path};
        pagespeed DaemonVolumePath {self.volume_path};
        {body}

        location ~ "^/pagespeed_admin" {{ }}
        location ~ "^/pagespeed_global_admin" {{ }}
        location /pagespeed_statistics {{ }}
        {''.join(locations)}
    }}
{inner}"""
        return server, applied

    def _write_configs(self, live: Optional[dict]) -> Set[str]:
        applied: Set[str] = set()
        modules = [f"load_module {self.module};"]
        for name in ("ngx_http_lane_filter_module", "ngx_http_sub_filter_module"):
            if name in self.filters:
                modules.append(f"load_module {self.filters[name]};")
        for inst in self.instances.values():
            inst.generation += 1
            p = inst.ports
            row = ""
            catch_all = f"""
    server {{
        listen 127.0.0.1:{p['tls']} ssl default_server;
        listen 127.0.0.1:{p['plain']} default_server;
        server_name _;
        ssl_certificate {self.cert};
        ssl_certificate_key {self.key};
        return 404;
    }}
"""
            if live is not None:
                row, applied = self._row_server(inst, live)
                if live.get("vhost", {}).get("default_vhost"):
                    catch_all = ""
            text = f"""# The HTML cache conformance provider's nginx {inst.name}.
worker_processes 1;
pid {inst.pid_path};
error_log {inst.root}/logs/error.log info;
env LD_LIBRARY_PATH;
env {HOLD_ENV};
{chr(10).join(modules)}

events {{
    worker_connections 256;
}}

http {{
    types {{
        text/html html;
    }}
    default_type application/octet-stream;
    # The pid of the worker that answered each request (process_ids).
    log_format dhc '$pid $request_method $uri';
    access_log {inst.root}/logs/access.log dhc;
    sendfile off;

    pagespeed AdminPath {ADMIN_PATH};
    pagespeed GlobalAdminPath {GLOBAL_ADMIN_PATH};
    pagespeed StatisticsPath /pagespeed_statistics;
    pagespeed GlobalStatisticsPath {GLOBAL_STATISTICS_PATH};
    pagespeed FileCachePath {self.cache_dir};
    pagespeed CacheFlushPollIntervalSec 1;
    pagespeed Statistics on;
    pagespeed LogDir {inst.root}/logs;
    pagespeed MessageBufferSize 100000;

    server {{
        listen 127.0.0.1:{p['probe']};
        server_name localhost;
        pagespeed on;
        pagespeed RewriteLevel PassThrough;
        location = /__dhc_probe {{
            add_header X-Dhc-Gen {inst.generation} always;
            return 204;
        }}
        location {GLOBAL_STATISTICS_PATH} {{ }}
    }}
{catch_all}{row}}}
"""
            with open(inst.conf_path, "w") as f:
                f.write(text)
        return applied

    # ---- Provider

    def setup(self, row_id: str, live: dict, bodies: Dict[str, str]) -> Iterable[str]:
        self.row = live
        self.row_id = row_id
        self.bodies = bodies
        self.mode = live.get("mode", "off")
        self.fragment = f"dhc-{next(self._row_seq)}-{row_id}"[:60]
        origin = live.get("origin", {})
        self.origin.reset(origin.get("status", 200),
                          [tuple(h) for h in origin.get("headers", [])],
                          bodies[origin["body_ref"]].encode("utf-8"))
        applied = self._write_configs(live)
        for inst in self.instances.values():
            r = self._nginx(inst, "-s", "reload")
            if r.returncode != 0:
                raise conf.ProviderUnsupported(f"nginx {inst.name} reload: {r.stderr.strip()}")
        for inst in self.instances.values():
            self._wait_generation(inst)
        for inst in self.instances.values():
            inst.sent = 0
            inst.purges = 0
            inst.log_offset = self._access_log_size(inst)
        applied = set(applied)
        applied |= {f"origin.{k}" for k in origin}  # status, headers, body_ref, echo
        if "worker" in live:
            raise conf.ProviderUnsupported("worker state per row is not implemented")
        return applied

    def teardown(self) -> None:
        self.origin.reset(200, [], b"")
        if os.path.exists(self.hold_file):
            os.unlink(self.hold_file)
        if self.worker_levels != (WORKER_GZIP_LEVEL, WORKER_BROTLI_LEVEL):
            self.restart_worker(WORKER_GZIP_LEVEL, WORKER_BROTLI_LEVEL)

    def _instance(self, process: Optional[str]) -> _Instance:
        if process is None:
            return self.instances["a"]
        if process not in self.instances:
            raise conf.ProviderUnsupported(f"no process {process!r}")
        return self.instances[process]

    def _send(self, inst: _Instance, method: str, path: str, host: str,
              headers: HeaderLines, tls: bool = True) -> conf.Response:
        if tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            c = http.client.HTTPSConnection("127.0.0.1", inst.ports["tls"], timeout=60,
                                            context=ctx)
        else:
            c = http.client.HTTPConnection("127.0.0.1", inst.ports["plain"], timeout=60)
        inst.sent += 1
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
            c.close()

    def _access_log_size(self, inst: _Instance) -> int:
        try:
            return os.path.getsize(os.path.join(inst.root, "logs", "access.log"))
        except OSError:
            return 0

    def _access_lines(self, inst: _Instance) -> List[Tuple[str, str, str]]:
        """(pid, method, uri) of the row's requests in the instance's access
        log: the generation probe and statistics reads left out."""
        try:
            with open(os.path.join(inst.root, "logs", "access.log"), "rb") as f:
                f.seek(inst.log_offset)
                data = f.read().decode("latin-1")
        except OSError:
            return []
        out = []
        for line in data.splitlines():
            parts = line.split(" ", 2)
            if len(parts) != 3 or parts[2] in ("/__dhc_probe", GLOBAL_STATISTICS_PATH):
                continue
            out.append((parts[0], parts[1], parts[2]))
        return out

    def held_requests(self) -> List[Tuple[str, str]]:
        with self.origin.lock:
            return list(self.origin.holding)

    def process_ids(self) -> Dict[str, Set[str]]:
        """The worker pids in each instance's access log (one line per
        answered request, written in the log phase); waits until every
        request this provider sent the instance is there."""
        out: Dict[str, Set[str]] = {}
        purge: Set[str] = set()
        deadline = time.monotonic() + SETTLE_TIMEOUT_S
        for name, inst in self.instances.items():
            while True:
                lines = self._access_lines(inst)
                if len(lines) >= inst.sent:
                    break
                if time.monotonic() >= deadline:
                    raise conf.ProviderUnsupported(
                        f"nginx {name}: {inst.sent} requests sent, {len(lines)} logged")
                time.sleep(0.05)
            row = [pid for pid, method, uri in lines
                   if not (method == "PURGE" or uri.startswith(ADMIN_PATH)
                           or uri.startswith(GLOBAL_ADMIN_PATH))]
            if row:
                out[name] = set(row)
            purge |= {pid for pid, method, uri in lines
                      if method == "PURGE" or uri.startswith(ADMIN_PATH)
                      or uri.startswith(GLOBAL_ADMIN_PATH)}
        if purge:
            out["purge"] = purge
        return out

    def request(self, method: str, path: str, host: Optional[str], headers: HeaderLines,
                process: Optional[str] = None) -> conf.Response:
        return self._send(self._instance(process), method, path, host or ROW_HOST, headers)

    def _fail_on_crashed_worker(self, inst: _Instance) -> None:
        """Fails when the instance's error log records a worker that exited on
        a signal."""
        path = os.path.join(inst.root, "logs", "error.log")
        try:
            with open(path, errors="replace") as f:
                crashed = [line.strip() for line in f if "exited on signal" in line]
        except OSError:
            return
        if crashed:
            raise AssertionError(f"nginx {inst.name}: a worker crashed: {crashed[-1]}")

    def _stats(self, inst: _Instance) -> Dict[str, int]:
        # A worker of the previous generation that is closing after a reload
        # may drop a connection it had accepted; that is transport, so the
        # read is retried a few times (an assertion never is).
        for attempt in range(5):
            c = http.client.HTTPConnection("127.0.0.1", inst.ports["probe"], timeout=30)
            try:
                c.request("GET", GLOBAL_STATISTICS_PATH, headers={"Host": "localhost"})
                r = c.getresponse()
                text = r.read().decode("utf-8", "replace")
                break
            except (http.client.RemoteDisconnected, ConnectionResetError):
                # A dropped connection is only transport when no worker died:
                # a crash is never retried away.
                self._fail_on_crashed_worker(inst)
                if attempt == 4:
                    raise
                time.sleep(0.2)
            finally:
                c.close()
        if r.status != 200:
            raise conf.ProviderUnsupported(f"global statistics of {inst.name}: {r.status}")
        return parse_statistics(text)

    def commits(self) -> Dict[str, Tuple[int, int]]:
        """(posted, finished) per instance."""
        out = {}
        for name, inst in self.instances.items():
            s = self._stats(inst)
            out[name] = (s.get(POSTED, 0), s.get(FINISHED, 0))
        return out

    def settle(self) -> None:
        deadline = time.monotonic() + SETTLE_TIMEOUT_S
        while True:
            state = self.commits()
            if all(posted == finished for posted, finished in state.values()):
                return
            if time.monotonic() >= deadline:
                raise conf.ProviderUnsupported(f"commits never finished: {state}")
            time.sleep(0.05)

    def counters(self) -> Dict[str, int]:
        total: Dict[str, int] = {}
        for inst in self.instances.values():
            for name, value in self._stats(inst).items():
                if name.startswith("daemon_"):
                    total[name] = total.get(name, 0) + value
        return total

    def origin_requests(self) -> List[HeaderLines]:
        with self.origin.lock:
            return [list(r) for r in self.origin.requests]

    def _key(self, path: str, host: Optional[str]) -> Tuple[bytes, bytes, bytes, bytes]:
        # The C API's key: the request path (query kept) in the keyspace of
        # (hostname without port, scheme, site).
        h = (host or ROW_HOST).lower()
        hostname = h.rsplit(":", 1)[0] if ":" in h and not h.startswith("[") else h
        site = hashlib.sha256(self.fragment.encode()).hexdigest()
        return (path.encode(), hostname.encode(), b"https", site.encode())

    def stored_entry(self, path: str, host: Optional[str]) -> Optional[conf.StoredEntry]:
        return self.volume.stored_entry(self._key(path, host))

    def markdown_ready(self, path: str, host: Optional[str]) -> bool:
        return self.volume.markdown_ready(self._key(path, host))

    def coded_ready(self, path: str, host: Optional[str], codings: List[str]) -> bool:
        return self.volume.coded_ready(self._key(path, host), codings)

    def restart_worker(self, gzip_level: int, brotli_level: int) -> None:
        """The worker this provider started, stopped and started again on the
        same volume with these levels."""
        worker = getattr(self, "_worker", None)
        if worker is not None and worker.poll() is None and _pid_is(worker.pid, "factory_worker"):
            worker.terminate()
            try:
                worker.wait(10)
            except subprocess.TimeoutExpired:
                worker.kill()
                worker.wait(10)
        self._start_worker(fresh=False, levels=(gzip_level, brotli_level))
        # Every server process reads the restarted worker's state anew.
        if getattr(self, "row", None) is not None:
            self._reload()

    def _admin_post(self, target: str) -> None:
        r = self._send(self.instances["a"], "POST", target, ROW_HOST,
                       [("X-Requested-With", "XMLHttpRequest"), ("Content-Length", "0")])
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
            r = self._send(self.instances["a"], "PURGE", path, host, [], tls=tls)
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
        with self.origin.lock:
            self.origin.body = body.encode("utf-8")

    def hold_origin(self) -> None:
        with self.origin.lock:
            self.origin.hold_next = True

    def release_origin(self) -> None:
        with self.origin.lock:
            held, self.origin.held = self.origin.held, []
            self.origin.hold_next = False
        for e in held:
            e.set()

    def set_mode(self, mode: str) -> None:
        self.mode = mode
        self._reload()

    # ---- for the held-commit test

    def hold_commits(self, on: bool) -> None:
        if on:
            with open(self.hold_file, "w"):
                pass
        elif os.path.exists(self.hold_file):
            os.unlink(self.hold_file)


_PROVIDER: Optional[NginxProvider] = None


def make_provider(server_config) -> NginxProvider:
    """One provider (and one set of servers) per test process."""
    global _PROVIDER
    if _PROVIDER is None:
        _PROVIDER = NginxProvider()
    return _PROVIDER
