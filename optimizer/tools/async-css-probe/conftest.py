# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Fixtures for the async-CSS probe.

Manages the origin / worker / nginx compose trio, warms the cache in the order
the deferral decision requires, and hands the tests a parsed view of the served
markup.

The harness runs the same fixture three ways, selected by
``ASYNC_CSS_PROBE_MODE``. Each mode differs only in the worker's argv:

``gated``    the sufficiency floor is raised so the gate REFUSES to defer.
             Asserts nothing is deferred, and no loader is injected.

``forced``   the same raised floor PLUS ``--unsafe-force-async-css``. Asserts the
             deferred markup is correct in every part.

``shipped``  stock flags. Records what the product does today on this page, so
             the lane notices when PR-C changes it.

``gated`` and ``forced`` differ by exactly one flag, so the pair is what proves
the switch does what it claims end to end — and what proves the gate, not luck,
is what suppresses deferral in ``gated``. A single mode could show neither.
"""

import json
import os
import re
import subprocess
import sys
import time
from html.parser import HTMLParser

import pytest
import requests

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from common.async_css_loader_path import async_css_loader_path_from_source
from common.worker_health import wait_worker_socket

PROBE_DIR = os.path.dirname(os.path.abspath(__file__))
COMPOSE_FILE = os.path.join(PROBE_DIR, "docker-compose.yml")
NGINX_PORT = os.environ.get("PROBE_NGINX_PORT", "8280")
ORIGIN_PORT = os.environ.get("PROBE_ORIGIN_PORT", "8281")
# Overridable because the rendered lane runs INSIDE a container on the compose
# network, where the services are reachable by service name and the host port
# mappings do not exist.
NGINX_URL = os.environ.get("PROBE_NGINX_URL", f"http://localhost:{NGINX_PORT}")
ORIGIN_URL = os.environ.get("PROBE_ORIGIN_URL", f"http://localhost:{ORIGIN_PORT}")
STARTUP_TIMEOUT = 180

MODE = os.environ.get("ASYNC_CSS_PROBE_MODE", "shipped")
assert MODE in ("gated", "forced", "shipped"), f"unknown probe mode: {MODE}"

# ---------------------------------------------------------------------------
# The deferral primitive — shared with the rendered lane
# ---------------------------------------------------------------------------
# primitive.json is the single flip point for BOTH lanes (probe_rendered.mjs
# reads the same file). PR-E edits it once; neither lane needs rewriting.
with open(os.path.join(PROBE_DIR, "primitive.json"), encoding="utf-8") as _f:
    PRIMITIVE = json.load(_f)

PRIMITIVE_ATTR = PRIMITIVE["primitive_attr"]
PRIMITIVE_VALUE = PRIMITIVE["primitive_value"]
# The companion attribute. It is not part of the attr/value pair the rendered
# lane toggles, but it is what makes the primitive a correctly-typed fetch —
# and what the preload is matched against when the loader turns the link into
# its consumer. A missing/wrong `as` downloads the sheet twice.
PRIMITIVE_AS = PRIMITIVE["primitive_as"]
DEFERRED_LINK_MARKER = PRIMITIVE["deferred_link_marker"]
LOADER_MARKER = PRIMITIVE["loader_marker"]
FALLBACK_MARKER = PRIMITIVE["fallback_marker"]
CRITICAL_BLOCK_MARKER = PRIMITIVE["critical_block_marker"]
SAVED_MEDIA_ATTR = PRIMITIVE["saved_media_attr"]
DEFERRED_SHEET_IS_EARLY_HINT_PROMOTED = PRIMITIVE["early_hint_promoted"]

# The fixture's one external stylesheet, at the path its captured HTML
# references (fixtures/modpagespeed-com/CAPTURE.md).
FIXTURE_HTML_PATH = "/"
FIXTURE_CSS_PATH = PRIMITIVE["fixture_stylesheet_path"]

# The capture directory mirrors the site's URL space (probe_origin.py), so a
# served path IS the path on disk. Derived rather than spelled out twice: the
# stylesheet's URL already lives in primitive.json, and a second copy of it
# here would be free to drift from the file the origin actually serves.
FIXTURE_DIR = os.path.join(PROBE_DIR, "fixtures", "modpagespeed-com")

# The subresources the fold actually renders with: the three web fonts the
# captured HTML preloads or its stylesheet declares @font-face for, and the two
# header/footer logos. They exist so the rendered lane can see a flash caused by
# a font or an image swapping in; a 404 on any of them puts that lane back to
# measuring a fold drawn in fallback fonts with no logo, silently.
#
# NOT in the fixture, and staying out: the QuickMessage island's JS bundle and
# /api/geo. Neither paints the fold — one is behaviour, one is data — and
# freezing a script into the capture would make the lane's verdict depend on
# whatever that script did on the day it was captured.
FIXTURE_FOLD_SUBRESOURCE_PATHS = (
    "/fonts/inter-variable-subset.woff2",
    "/fonts/plex-mono-600.woff2",
    "/fonts/plex-mono-400.woff2",
    "/logo.svg",
    "/logo-dark.svg",
)


# The second page: stock Bootstrap 5.3.3 (fixtures/bootstrap-5.3.3), served at
# /plain/ with its stylesheet at /bootstrap.min.css. Its stylesheet holds no
# `<` followed by a letter, `/`, `!` or `?`, which is what lets the optimizer
# inline a critical block for it. The primary page's Tailwind v4 sheet holds
# `@property ... syntax:"<color>"`: a `<` inside a CSS string, which the
# optimizer escapes and inlines (#681). The pair covers a sheet with and without
# such a `<`.
PLAIN_HTML_PATH = "/plain/"
PLAIN_CSS_PATH = "/bootstrap.min.css"
PLAIN_FIXTURE_DIR = os.path.join(PROBE_DIR, "fixtures", "bootstrap-5.3.3")


def fixture_file(url_path: str) -> str:
    """The fixture file the origin serves at ``url_path``."""
    rel = "index.html" if url_path == "/" else url_path.lstrip("/")
    return os.path.join(FIXTURE_DIR, *rel.split("/"))


_UNSAFE_FORCE_HEADER = os.path.join(
    PROBE_DIR, "..", "..", "src", "worker", "unsafe_force_async_css.h"
)


def _unsafe_force_warning_text() -> str:
    """The startup warning, read out of the C++ header that defines it.

    Read rather than duplicated for the same reason the loader path is
    recomputed rather than hardcoded: a copy here would let the warning be
    reworded, or deleted, with the test still green.
    """
    with open(_UNSAFE_FORCE_HEADER, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"kUnsafeForceAsyncCssWarning\s*=\s*(.*?\")\s*;", text, re.S)
    assert m, f"kUnsafeForceAsyncCssWarning not found in {_UNSAFE_FORCE_HEADER}"
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    warning = "".join(parts)
    assert warning, "empty parse of kUnsafeForceAsyncCssWarning"
    return warning


UNSAFE_FORCE_WARNING = _unsafe_force_warning_text()

# ---------------------------------------------------------------------------
# What the product does today, on this page, with stock flags
# ---------------------------------------------------------------------------
# FALSE since the empirical gate landed. It used to be TRUE, and that was the
# defect: on a warm cache the extractor produces ~45 KB of critical CSS against
# the fixture's ~115 KB sheet — over the 0.10 byte-ratio floor — so the deferral
# was permitted although nothing had checked that the inlined block actually
# covers the fold. The floor is a proxy; this page was its counter-example.
#
# Deferral now additionally requires a validated profile whose stylesheet hash
# still matches the sheet being served. This fixture has neither (no browser in
# the probe stack, so no analysis ever runs), so the stock-flag run keeps its
# stylesheet render-blocking with the critical CSS still inlined — which is what
# the shipped-mode assertions below now pin.
DEFERS_WITHOUT_A_VALIDATED_PROFILE = False


def names_cascade_layer(css: str) -> bool:
    """Mirror of CriticalCssNamesCascadeLayer (src/worker/html_css_injector.h):
    does the block name a cascade layer? Such a block goes first only with the
    page's proven layer order in front of it (leading_layer_statement), and
    otherwise keeps the old placement, before </head> after every head
    stylesheet."""
    for m in re.finditer(r"@layer(?![\w-])\s*(?:/\*.*?\*/\s*)*(.)", css, re.I | re.S):
        if m.group(1) != "{":
            return True
    return any(
        re.search(r"[\s)\"'](layer)[\s(;]|[\s)\"']layer$", rule, re.I)
        for rule in re.findall(r"@import[^;]*;?", css, re.I)
    )


LAYER_STATEMENT_RE = re.compile(r"@layer\s+([\w.-]+(?:\s*,\s*[\w.-]+)*)\s*;")


def leading_layer_statement(css: str) -> list[str] | None:
    """The `@layer a,b,...;` statement the worker puts in front of a layered
    block it places before the sheets (cascade_layer_order.h),
    as a name list; None when the block does not start with one."""
    m = LAYER_STATEMENT_RE.match(css)
    if not m:
        return None
    return [n.strip() for n in m.group(1).split(",")]


def top_level_layer_order(css: str) -> list[str]:
    """Named layers of a sheet in first-mention order, for a sheet whose
    layers are all top-level `@layer name{` blocks or statements (the probe
    fixture's Tailwind v4 output). Not a general parser: the unit tests of
    cascade_layer_order cover the general case."""
    names: list[str] = []
    depth = 0
    i = 0
    while i < len(css):
        c = css[i]
        if c == "\\":
            # An escape is part of an ident: `.a\{` opens no block.
            i += 2
            continue
        if c in "\"'":
            # Strings, escapes inside included, end at the matching quote or
            # a newline (CSS Syntax 3 bad string).
            j = i + 1
            while j < len(css) and css[j] not in (c, "\n"):
                j += 2 if css[j] == "\\" else 1
            i = j + 1 if j < len(css) and css[j] == c else j
            continue
        if css.startswith("/*", i):
            end = css.find("*/", i + 2)
            i = len(css) if end < 0 else end + 2
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 0 and css.startswith("@layer", i):
            m = re.match(r"@layer\s*([^{;]*)[{;]", css[i:])
            if m:
                for n in m.group(1).split(","):
                    n = n.strip()
                    if n and n not in names:
                        names.append(n)
        i += 1
    return names


class Element:
    """One start tag: its name, its attributes, whether it was inside
    <noscript> and whether it came before </head>; for a <style>, its body."""

    def __init__(self, tag: str, attrs: list, in_noscript: bool, in_head=True):
        self.tag = tag
        self.in_head = in_head
        self.text = ""
        # Attribute order is not semantic; presence-with-no-value is (a bare
        # `crossorigin` must stay bare), so keep None distinct from "".
        self.attrs = {k: v for k, v in attrs}
        self.in_noscript = in_noscript

    def has(self, name: str) -> bool:
        return name in self.attrs

    def __repr__(self):
        return f"<{self.tag} {self.attrs} noscript={self.in_noscript}>"


class MarkupIndex(HTMLParser):
    """Elements of interest from a served page, plus inline <style> bodies."""

    def __init__(self, html: str):
        super().__init__(convert_charrefs=True)
        self.elements: list[Element] = []
        self.styles: list[str] = []
        self._noscript_depth = 0
        self._head_done = False
        self._style_el: Element | None = None
        self._in_style = False
        self._style_buf: list[str] = []
        self.feed(html)
        self.close()

    def handle_starttag(self, tag, attrs):
        if tag == "noscript":
            self._noscript_depth += 1
        if tag == "body":
            self._head_done = True
        el = Element(tag, attrs, self._noscript_depth > 0, not self._head_done)
        if tag == "style":
            self._in_style = True
            self._style_buf = []
            self._style_el = el
        self.elements.append(el)

    def handle_startendtag(self, tag, attrs):
        self.elements.append(
            Element(tag, attrs, self._noscript_depth > 0, not self._head_done)
        )

    def handle_endtag(self, tag):
        if tag == "head":
            self._head_done = True
        if tag == "noscript" and self._noscript_depth > 0:
            self._noscript_depth -= 1
        if tag == "style" and self._in_style:
            self._in_style = False
            self.styles.append("".join(self._style_buf))
            if self._style_el is not None:
                self._style_el.text = self.styles[-1]

    def handle_data(self, data):
        if self._in_style:
            self._style_buf.append(data)

    def by_tag(self, tag: str) -> list[Element]:
        return [e for e in self.elements if e.tag == tag]

    def stylesheet_links(self) -> list[Element]:
        return [
            e
            for e in self.elements
            if e.tag == "link" and (e.attrs.get("rel") or "").lower() == "stylesheet"
        ]

    def deferred_links(self) -> list[Element]:
        return [
            e for e in self.elements if e.tag == "link" and e.has(DEFERRED_LINK_MARKER)
        ]

    def critical_block_positions(self) -> list[int]:
        """Document-order positions of the inlined critical <style>."""
        return [
            i
            for i, e in enumerate(self.elements)
            if e.tag == "style" and e.has(CRITICAL_BLOCK_MARKER)
        ]

    def stylesheet_source_positions(self) -> list[int]:
        """Document-order positions of every stylesheet source that applies
        when scripts run: each <style> other than the critical block, each
        <link> whose rel names a stylesheet, and each deferred <link> (which
        the loader turns back into one). <noscript> content is excluded — it
        is raw text whenever scripts run."""

        def is_source(e: Element) -> bool:
            if e.tag == "style":
                return not e.has(CRITICAL_BLOCK_MARKER)
            if e.tag == "link":
                rel = (e.attrs.get("rel") or "").lower().split()
                return "stylesheet" in rel or e.has(DEFERRED_LINK_MARKER)
            return False

        return [
            i for i, e in enumerate(self.elements) if not e.in_noscript and is_source(e)
        ]

    def loader_scripts(self, loader_path: str) -> list[Element]:
        return [
            e
            for e in self.elements
            if e.tag == "script" and (e.attrs.get("src") or "") == loader_path
        ]


# The optimized HTML variant is keyed on the client's capability mask, and the
# response Varies on User-Agent. Pin one so every request in the lane reads the
# same variant — an unpinned UA makes "which page did we just assert on?" a
# function of the requests version.
PROBE_USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36"
)


class ProbeClient:
    def __init__(self, base_url: str):
        self.base_url = base_url
        self.session = requests.Session()
        self.session.headers["User-Agent"] = PROBE_USER_AGENT

    def get(self, path: str, **kwargs) -> requests.Response:
        return self.session.get(self.base_url + path, **kwargs)

    def head(self, path: str, **kwargs) -> requests.Response:
        return self.session.head(self.base_url + path, **kwargs)

    def poll_for_hit(self, path: str, timeout: float = 30.0, interval: float = 0.5):
        """Poll until the response is served from cache."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                r = self.get(path)
                last = r
                if r.headers.get("X-PageSpeed") == "HIT":
                    return r
            except requests.RequestException:
                pass
            time.sleep(interval)
        assert last is not None, f"No response at all for {path}"
        raise AssertionError(
            f"{path} never reached X-PageSpeed: HIT within {timeout}s "
            f"(last: {last.status_code} "
            f"X-PageSpeed={last.headers.get('X-PageSpeed', '<absent>')})"
        )

    def poll_for_optimized(
        self,
        path: str,
        origin_bytes: bytes,
        timeout: float = 60.0,
        interval: float = 0.25,
    ):
        """Poll until the served page is the OPTIMIZER'S output.

        ``X-PageSpeed: HIT`` alone is not that signal: the front end stores and
        serves the origin bytes while the worker optimizes out of band, so a HIT
        arriving 0.3s in is routinely the unmodified page. Asserting on it reads
        as "the optimizer did nothing", which is the single most misleading way
        this harness can fail. The optimizer's output is by definition not the
        origin's bytes, so wait for exactly that.
        """
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                r = self.get(path)
                last = r
                if r.headers.get("X-PageSpeed") == "HIT" and r.content != origin_bytes:
                    return r
            except requests.RequestException:
                pass
            time.sleep(interval)
        detail = (
            f"{last.status_code} X-PageSpeed="
            f"{last.headers.get('X-PageSpeed', '<absent>')} "
            f"{len(last.content)} bytes vs {len(origin_bytes)} at the origin"
            if last is not None
            else "no response at all"
        )
        raise AssertionError(
            f"{path} was never served as optimized output within {timeout}s ({detail})"
        )

    def poll_for_settled(
        self,
        path: str,
        origin_bytes: bytes,
        timeout: float = 60.0,
        interval: float = 0.25,
    ):
        """Poll until the optimizer has DECIDED about the page.

        ``poll_for_optimized`` waits for bytes that differ from the origin's,
        which a page the optimizer leaves alone never produces: a plain page
        with no critical block, no deferral and nothing else to rewrite is
        served as the origin sent it, and that is the correct outcome. So the
        decision is either bytes that differ, or the worker's own record that
        it found nothing to transform for this page.
        """
        deadline = time.time() + timeout
        last = None
        next_log_check = 0.0
        while time.time() < deadline:
            try:
                r = self.get(path)
                last = r
                if r.headers.get("X-PageSpeed") == "HIT":
                    if r.content != origin_bytes:
                        return r
                    if time.time() >= next_log_check:
                        next_log_check = time.time() + 2.0
                        logs = compose_run("logs", "--no-color", "worker").stdout
                        if any(
                            f"{line} {path}" in logs
                            for line in (
                                "No HTML transformations applicable for",
                                "HTML transform produced no changes for",
                            )
                        ):
                            return r
            except requests.RequestException:
                pass
            time.sleep(interval)
        detail = (
            f"{last.status_code} X-PageSpeed="
            f"{last.headers.get('X-PageSpeed', '<absent>')}"
            if last is not None
            else "no response at all"
        )
        raise AssertionError(
            f"{path} was neither rewritten nor declared untouched by the worker "
            f"within {timeout}s ({detail})"
        )


def compose_run(*args):
    return subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, *args],
        capture_output=True,
        text=True,
    )


def compose_check(*args):
    return subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, *args],
        capture_output=True,
        text=True,
        check=True,
    )


def docker_exec(service, command):
    cmd_parts = command.split() if isinstance(command, str) else list(command)
    return subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, "exec", "-T", service] + cmd_parts,
        capture_output=True,
        text=True,
    )


def _wait_for_service(url: str, path: str, timeout: float = STARTUP_TIMEOUT):
    deadline = time.time() + timeout
    last = "no response"
    while time.time() < deadline:
        try:
            r = requests.get(url + path, timeout=3)
            if r.status_code < 500:
                return
            last = f"HTTP {r.status_code}"
        except requests.RequestException as e:
            last = str(e)
        time.sleep(0.5)
    raise TimeoutError(f"Service not ready at {url}{path} after {timeout}s ({last})")


@pytest.fixture(scope="session")
def probe_services():
    """Start the compose trio, wait for readiness, yield, tear down.

    Set PROBE_NO_LIFECYCLE=1 when the services are managed externally (CI
    brings them up in its own steps so their logs land in the right place).
    """
    no_lifecycle = os.environ.get("PROBE_NO_LIFECYCLE", "") == "1"
    if not no_lifecycle:
        compose_check("build")
        compose_check("up", "-d")
    try:
        _wait_for_service(ORIGIN_URL, FIXTURE_HTML_PATH)
        _wait_for_service(ORIGIN_URL, PLAIN_HTML_PATH)
        # Readiness is probed on the STYLESHEET, never on the page under test.
        # A readiness request to "/" would be the page's first optimization, run
        # before the sheet is cached — so the page would be stored in the
        # cold-cache state and every later assertion would read a page the
        # optimizer never got to see properly.
        _wait_for_service(NGINX_URL, FIXTURE_CSS_PATH)
        _wait_for_service(NGINX_URL, PLAIN_CSS_PATH)
        # Confirm the worker is actually ready: a still-starting worker
        # performs no optimization at all and every assertion below would
        # fail as an unexplained absence of markup. Skipped only where
        # there is no docker socket to ask through — the rendered lane runs
        # inside a container on the compose network, and the markup lane has
        # already made the same check against the same worker.
        if os.environ.get("PROBE_NO_DOCKER", "") != "1":
            wait_worker_socket(docker_exec, timeout=60)
        yield {"nginx_url": NGINX_URL, "origin_url": ORIGIN_URL}
    finally:
        if not no_lifecycle:
            logs = compose_run("logs", "--no-color")
            if logs.stdout:
                print("\n=== Docker Compose Logs (tail) ===")
                print(logs.stdout[-8000:])
            compose_run("down", "--remove-orphans", "-v")


@pytest.fixture(scope="session")
def client(probe_services) -> ProbeClient:
    return ProbeClient(probe_services["nginx_url"])


@pytest.fixture(scope="session")
def async_css_loader_path() -> str:
    """The path the worker serves the loader at, recomputed from
    src/worker/async_css_loader.h — never hardcoded."""
    return async_css_loader_path_from_source()


@pytest.fixture(scope="session")
def fixture_html_bytes() -> bytes:
    with open(fixture_file(FIXTURE_HTML_PATH), "rb") as f:
        return f.read()


@pytest.fixture(scope="session")
def optimized_home(client, fixture_html_bytes) -> requests.Response:
    """The fixture home page as the optimizer serves it.

    Warm order matters, and is the reason this is a session fixture rather than
    a line in each test: the stylesheet must be in cache BEFORE the page is
    first optimized. Until it is, the worker sees an uncached external <link>,
    derives its critical block from the page's own inline CSS alone, and stores
    THAT — a page the optimizer never really got to look at. The result reads
    like a product bug and is a warm-up race.
    """
    client.poll_for_hit(FIXTURE_CSS_PATH)
    return client.poll_for_optimized(FIXTURE_HTML_PATH, fixture_html_bytes)


@pytest.fixture(scope="session")
def home_markup(optimized_home) -> MarkupIndex:
    return MarkupIndex(optimized_home.text)


@pytest.fixture(scope="session")
def optimized_plain(client) -> requests.Response:
    """The plain (Bootstrap) page as the optimizer serves it. Same warm order
    as ``optimized_home``: its stylesheet is cached before the page is first
    optimized."""
    with open(os.path.join(PLAIN_FIXTURE_DIR, "index.html"), "rb") as f:
        origin_bytes = f.read()
    client.poll_for_hit(PLAIN_CSS_PATH)
    return client.poll_for_settled(PLAIN_HTML_PATH, origin_bytes)


@pytest.fixture(scope="session")
def plain_markup(optimized_plain) -> MarkupIndex:
    return MarkupIndex(optimized_plain.text)


@pytest.fixture(scope="session")
def plain_css_bytes() -> bytes:
    with open(os.path.join(PLAIN_FIXTURE_DIR, "bootstrap.min.css"), "rb") as f:
        return f.read()


class Page:
    """One optimized fixture page: where it lives, what was served, its markup."""

    def __init__(self, name, html_path, css_path, response):
        self.name = name
        self.html_path = html_path
        self.css_path = css_path
        self.response = response
        self.markup = MarkupIndex(response.text)


@pytest.fixture(scope="session")
def tailwind_page(optimized_home) -> Page:
    return Page("tailwind", FIXTURE_HTML_PATH, FIXTURE_CSS_PATH, optimized_home)


@pytest.fixture(scope="session")
def plain_page(optimized_plain) -> Page:
    return Page("plain", PLAIN_HTML_PATH, PLAIN_CSS_PATH, optimized_plain)


@pytest.fixture(params=["tailwind", "plain"])
def page(request) -> Page:
    """Each fixture page in turn, for the invariants that hold for both."""
    return request.getfixturevalue(f"{request.param}_page")


@pytest.fixture(scope="session")
def fixture_css_bytes() -> bytes:
    with open(fixture_file(FIXTURE_CSS_PATH), "rb") as f:
        return f.read()


def link_headers(response: requests.Response) -> list[str]:
    """Every Link header value, split on the comma-joined form requests
    produces when nginx emits the header more than once."""
    raw = response.headers.get("Link")
    if not raw:
        return []
    return [part.strip() for part in re.split(r",(?=\s*<)", raw) if part.strip()]


def _mode_only(mode: str):
    return pytest.mark.skipif(
        mode != MODE, reason=f"runs in ASYNC_CSS_PROBE_MODE={mode} only"
    )


forced_only = _mode_only("forced")
gated_only = _mode_only("gated")
shipped_only = _mode_only("shipped")
