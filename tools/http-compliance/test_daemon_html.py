# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Pages a web-server module stores itself, and the worker's html_mode.

Two parts.

The module path runs against `module-worker`, a second worker on its own
volume with no network. A stand-in for the module's HTML store
(module_standin.py, executed inside that container) stores a page through the
public C API exactly as the module does: the store verdict from
ps_html_store_classify, a paired headers sidecar, then the durable original
stamped with the SHA-256 of its bytes, all in one site's keyspace. It then
notifies the worker with FROM_MODULE | HOST_VOUCHED | HTML_STORED and reads
back what the module's serve path would read: the stored facts, the Markdown
copy's binding and entity tag, and the serve verdict for an agent request.

The html_mode part runs against the nginx front's own worker: `full` behaves
as `disable_html=false`, and a `markdown-*` mode writes no optimized HTML.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import time
import uuid

import module_standin
import pytest
from common.worker_health import wait_worker_socket
from conftest import ComplianceClient, docker_exec

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ABI_GOLDEN = os.path.join(REPO_ROOT, "lib", "pagespeed", "abi", "abi-golden.json")

MODULE = "module-worker"
HOST = "module.example"
SITE = hashlib.sha256(b"http-compliance module site").hexdigest()
ACCEPTED = ["from_module", "host_vouched", "html_stored"]
HTML_MODE_VALUE = {"off": 0, "markdown-static": 1, "markdown-rendered": 2, "full": 3}

ARTICLE = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>{title}</title></head>
<body><main><article>
<h1>{title}</h1>
<p>The harbour opened at dawn, and the first boats went out past the breakwater
while the light was still grey over the water. {marker}</p>
<p>By noon the market had filled with crates of fish, rope, nets and the people
who bargain over them, and the gulls had found every one of the crates.</p>
<p>In the evening the boats came back in the order they had left, and the
harbour master wrote each one down in the ledger he keeps by the door.</p>
</article></main></body></html>
"""

SHELL = (
    "<!DOCTYPE html><html><head><title>x</title></head>"
    '<body><div id="app"></div></body></html>'
)


def _page(marker: str) -> str:
    return ARTICLE.format(title=f"Harbour log {marker}", marker=marker)


def _url() -> str:
    return f"/module/{uuid.uuid4().hex}.html"


def _poll(fn, timeout: float = 20.0, interval: float = 0.25):
    """Call fn until it returns a truthy value; return that value or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(interval)
    return None


def _is_plain_variant(alternate_id) -> bool:
    """An id selection could serve as an optimized page (not a sentinel)."""
    return alternate_id is not None and ((alternate_id >> 2) & 0x03) != 3


class Worker:
    """The management API and stats of one worker container."""

    def __init__(self, service: str):
        self.service = service

    def _exec(self, argv):
        return docker_exec(self.service, argv)

    def api(self, method: str, path: str, body: dict | None = None) -> dict:
        argv = [
            "curl",
            "-sS",
            "--fail-with-body",
            "--unix-socket",
            "/shared/api.sock",
            "-X",
            method,
            "-H",
            "X-Requested-With: XMLHttpRequest",
        ]
        if body is not None:
            argv += ["-H", "Content-Type: application/json", "--data", json.dumps(body)]
        argv.append("http://localhost" + path)
        r = self._exec(argv)
        assert r.returncode == 0, (
            f"{method} {path} on {self.service}: rc={r.returncode} "
            f"stdout={r.stdout[-500:]!r} stderr={r.stderr[-500:]!r}"
        )
        return json.loads(r.stdout)

    def stats(self) -> dict:
        return self.api("GET", "/v1/stats")

    def counter(self, section: str, key: str) -> int:
        return int(self.stats()[section][key])

    def set_html_mode(self, body: dict) -> dict:
        # The nginx front notices a shared-config change by its mtime, which
        # has one-second resolution: never write it twice in one second.
        time.sleep(1.1)
        return self.api("PATCH", "/v1/config", body)


class ModuleStandIn:
    """Runs module_standin.py inside the module worker's container."""

    def __call__(self, cmd: str, **req) -> dict:
        req = {"cmd": cmd, "host": HOST, "site": SITE, "scheme": "http", **req}
        r = docker_exec(MODULE, ["python3", "/app/module_standin.py", json.dumps(req)])
        assert r.returncode == 0, (
            f"module stand-in {cmd} failed: rc={r.returncode} "
            f"stdout={r.stdout[-500:]!r} stderr={r.stderr[-1000:]!r}"
        )
        return json.loads(r.stdout.strip().splitlines()[-1])

    def store(self, url: str, body: str, **kw) -> dict:
        return self("store", url=url, body=body, **kw)

    def notify(self, url: str, agent: bool = False, flags=None, **kw) -> str:
        return self(
            "notify",
            url=url,
            agent=agent,
            flags=ACCEPTED if flags is None else flags,
            **kw,
        )["rc"]

    def facts(self, url: str, **kw) -> dict:
        return self("facts", url=url, **kw)


@pytest.fixture(scope="session")
def module_worker(compliance_services):
    """The module worker, in markdown-static with agent optimization on."""
    wait_worker_socket(docker_exec, timeout=30, container=MODULE)
    worker = Worker(MODULE)
    standin = ModuleStandIn()
    worker.set_html_mode({"html_mode": "markdown-static"})
    shared = _poll(
        lambda: (lambda s: s if s["html_mode"] == 1 else None)(standin("shared_config"))
    )
    assert shared, "the module worker never published markdown-static"
    assert shared["agent_entitled"] == 1, shared
    assert shared["version_minor"] >= 14, shared
    return worker


@pytest.fixture
def standin(module_worker) -> ModuleStandIn:
    return ModuleStandIn()


@pytest.fixture
def module_mode(module_worker, standin):
    """Switch the module worker's html_mode for one test, then put it back."""

    def switch(mode: str):
        module_worker.set_html_mode({"html_mode": mode})
        assert _poll(
            lambda: standin("shared_config")["html_mode"] == HTML_MODE_VALUE[mode]
        ), f"html_mode {mode} not published"

    yield switch
    switch("markdown-static")


def _derive(worker: Worker, standin: ModuleStandIn, url: str, body: str):
    """Store a page, send the store frame and the agent frame, and wait for
    the copy. Returns the facts the serve path reads afterwards."""
    stored = standin.store(url, body)
    assert stored["action"] == "store", stored
    assert stored["original"] == "ok" and stored["sidecar_stored"], stored
    before = worker.counter("agent_markdown", "module_derived")
    assert standin.notify(url) == "ok"
    assert standin.notify(url, agent=True) == "ok"
    assert _poll(lambda: worker.counter("agent_markdown", "module_derived") > before), (
        "no Markdown copy was derived"
    )
    return stored, standin.facts(url)


# ---------------------------------------------------------------------------
# The stand-in itself
# ---------------------------------------------------------------------------


class TestStandInLayout:
    def test_ctypes_layouts_match_the_abi_golden(self):
        """The stand-in's ctypes mirrors are the published layouts."""
        with open(ABI_GOLDEN) as f:
            golden = json.load(f)["structs"]
        for name, cls in {
            **module_standin.STRUCTS,
            **module_standin.PREFIX_STRUCTS,
        }.items():
            spec = golden[name]
            offsets = {f[0]: getattr(cls, f[0]).offset for f in cls._fields_}
            for field, offset in spec["fields"].items():
                assert offsets[field] == offset, f"{name}.{field}"
            if name in module_standin.STRUCTS:
                assert ctypes.sizeof(cls) == spec["size"], name


# ---------------------------------------------------------------------------
# Module-stored HTML
# ---------------------------------------------------------------------------


class TestModuleStoredHtml:
    def test_copy_is_derived_and_bound_to_the_stamp(self, module_worker, standin):
        """A store frame then an agent frame: the copy is bound to the stored
        page's stamp, carries an entity tag, and serves to an agent."""
        url = _url()
        stored, facts = _derive(module_worker, standin, url, _page("bound"))
        assert facts["original"] and facts["markdown"], facts
        assert facts["stamp"] == stored["hash"]
        assert facts["markdown_parent"] == stored["hash"]
        assert facts["sidecar"] == "paired"
        assert "binding_live" in facts["facts"], facts
        assert "present" in facts["facts"], facts
        assert "harbour master" in facts["markdown_body"]
        assert facts["markdown_hash"] != "0" * 64
        assert facts["etag"] and facts["etag"].startswith('W/"psm-'), facts
        serve = facts["serve"]
        assert serve["action"] == "serve_markdown", serve
        assert serve["wants_markdown"] and serve["etag"] == facts["etag"]

    def test_changed_parent_unbinds_the_copy(self, module_worker, standin):
        """The page stored again with other bytes: the old copy no longer
        binds, gets no entity tag and is not served, until it is derived
        again for the new bytes."""
        url = _url()
        _derive(module_worker, standin, url, _page("first"))
        restored = standin.store(url, _page("second"))
        assert restored["original"] == "ok", restored
        facts = standin.facts(url)
        assert facts["markdown"], "the earlier copy is still stored"
        assert facts["markdown_parent"] != restored["hash"]
        assert "binding_live" not in facts["facts"], facts
        assert facts["etag"] is None, facts
        assert facts["serve"]["action"] == "fall_through", facts
        assert facts["serve"]["reason"] == "binding_stale", facts

        before = module_worker.counter("agent_markdown", "module_derived")
        assert standin.notify(url, agent=True) == "ok"
        assert _poll(
            lambda: module_worker.counter("agent_markdown", "module_derived") > before
        )
        facts = standin.facts(url)
        assert facts["markdown_parent"] == restored["hash"]
        assert "binding_live" in facts["facts"], facts
        assert "second" in facts["markdown_body"]
        assert facts["serve"]["action"] == "serve_markdown", facts

    @pytest.mark.parametrize("mode", ["markdown-static", "markdown-rendered"])
    def test_no_html_variant_in_markdown_modes(
        self, module_worker, standin, module_mode, mode
    ):
        """Module HTML in a markdown-* mode yields the Markdown copy and
        nothing else: no optimized HTML variant, no HTML pass."""
        module_mode(mode)
        html_before = _html_passes(module_worker)
        variants_before = module_worker.counter("variants", "written")
        url = _url()
        _stored, facts = _derive(module_worker, standin, url, _page(mode))
        assert facts["markdown"] and "binding_live" in facts["facts"]
        assert not _is_plain_variant(facts["best_alternate_id"]), facts
        assert _html_passes(module_worker) == html_before
        assert module_worker.counter("variants", "written") == variants_before

    def test_no_network_in_markdown_static(self, module_worker, standin):
        """The copy is derived with no network at all: the worker's
        container has only a loopback interface and no browser."""
        r = docker_exec(MODULE, ["ls", "/sys/class/net"])
        assert r.returncode == 0 and r.stdout.split() == ["lo"], r.stdout
        health = module_worker.api("GET", "/v1/health")
        assert health["browser"] == {"enabled": False}, health
        _stored, facts = _derive(module_worker, standin, _url(), _page("offline"))
        assert "binding_live" in facts["facts"], facts

    def test_below_floor_is_withheld_once(self, module_worker, standin):
        """A page with too little text gets no copy but a withheld record;
        a second agent frame for the same content does no derivation."""
        url = _url()
        assert standin.store(url, SHELL)["original"] == "ok"
        withheld = module_worker.counter("agent_markdown", "module_withheld")
        assert standin.notify(url, agent=True) == "ok"
        assert _poll(
            lambda: (
                module_worker.counter("agent_markdown", "module_withheld") > withheld
            )
        )
        facts = standin.facts(url)
        assert not facts["markdown"] and facts["withheld"], facts
        assert facts["withheld_reason"] == "below_floor", facts
        assert facts["serve"]["reason"] == "withheld", facts

        skips = module_worker.counter("agent_markdown", "module_withheld_skips")
        assert standin.notify(url, agent=True) == "ok"
        assert _poll(
            lambda: (
                module_worker.counter("agent_markdown", "module_withheld_skips") > skips
            )
        )
        assert not standin.facts(url)["markdown"]

    def test_hash_mismatch_is_withheld_until_stored_again(self, module_worker, standin):
        """Bytes that do not match their stamp get no copy; storing the page
        again clears the record and the next agent frame derives it."""
        url = _url()
        body = _page("mismatch")
        bad = standin.store(url, body, stamp="ab" * 32)
        assert bad["original"] == "ok", bad
        withheld = module_worker.counter("agent_markdown", "module_withheld")
        assert standin.notify(url, agent=True) == "ok"
        assert _poll(
            lambda: (
                module_worker.counter("agent_markdown", "module_withheld") > withheld
            )
        )
        facts = standin.facts(url)
        assert facts["withheld"] and not facts["markdown"], facts
        assert facts["withheld_reason"] == "hash_mismatch", facts
        assert facts["serve"]["reason"] == "withheld", facts

        _stored, facts = _derive(module_worker, standin, url, body)
        assert not facts["withheld"], facts
        assert facts["serve"]["action"] == "serve_markdown", facts


class TestRefusedShapes:
    def test_library_refuses_unvouched_module_html(self, module_worker, standin):
        """Module HTML without host validation, or without the stored-page
        bit, never leaves the library, so the worker receives nothing."""
        url = _url()
        assert standin.store(url, _page("unvouched"))["original"] == "ok"
        received = module_worker.counter("notifications", "received")
        for flags in (
            ["from_module"],
            ["from_module", "html_stored"],
            ["from_module", "host_vouched"],
        ):
            for agent in (False, True):
                assert standin.notify(url, agent=agent, flags=flags) == "invalid_arg", (
                    flags
                )
        # A frame the library accepts, sent after them, proves the worker
        # was listening: only it arrives.
        assert standin.notify(url) == "ok"
        assert _poll(
            lambda: module_worker.counter("notifications", "received") > received
        )
        time.sleep(0.5)
        assert module_worker.counter("notifications", "received") == received + 1
        assert not standin.facts(url)["markdown"]

    @pytest.mark.parametrize(
        "case",
        [
            "mode_full",
            "mode_off",
            "not_from_html_store",
            "no_transform",
            "nothing_stored",
        ],
    )
    def test_refused_and_counted(self, module_worker, standin, module_mode, case):
        """Frames the library accepts but the worker must refuse: each is
        counted once in notifications.rejected_module_render and writes no
        copy."""
        url = _url()
        body = _page(case)
        if case == "mode_full":
            module_mode("full")
        elif case == "mode_off":
            module_mode("off")
        if case == "not_from_html_store":
            assert standin.store(url, body, unclassified=True)["original"] == "ok"
        elif case == "no_transform":
            stored = standin.store(
                url,
                body,
                headers=[
                    ["Content-Type", "text/html; charset=utf-8"],
                    ["Cache-Control", "public, max-age=600, no-transform"],
                ],
            )
            # The module would store it without notifying; a module that
            # notifies anyway is refused.
            assert stored["action"] == "store_no_notify", stored
            assert stored["original"] == "ok", stored
        elif case != "nothing_stored":
            assert standin.store(url, body)["original"] == "ok"

        # The page is there (or, for nothing_stored, is not), so each case is
        # refused for its own reason and not for a write that failed.
        assert standin.facts(url)["original"] == (case != "nothing_stored"), case
        rejected = module_worker.counter("notifications", "rejected_module_render")
        derived = module_worker.counter("agent_markdown", "module_derived")
        assert standin.notify(url, agent=True) == "ok"
        assert _poll(
            lambda: (
                module_worker.counter("notifications", "rejected_module_render")
                > rejected
            )
        ), case
        time.sleep(0.5)
        assert (
            module_worker.counter("notifications", "rejected_module_render")
            == rejected + 1
        ), case
        assert module_worker.counter("agent_markdown", "module_derived") == derived
        facts = standin.facts(url)
        assert not facts["markdown"] and not facts["withheld"], facts


# ---------------------------------------------------------------------------
# html_mode on the nginx front
# ---------------------------------------------------------------------------


@pytest.fixture
def front_worker(compliance_services):
    """The nginx front's worker, put back to full after the test."""
    worker = Worker("worker")
    yield worker
    worker.set_html_mode({"html_mode": "full"})
    # The next test file must not meet a front still in another mode: wait
    # until full is published, then for the front's next shared-config poll.
    assert _poll(lambda: _shared_conf().get("html_mode") == "full", timeout=10), (
        "full was not published"
    )
    time.sleep(1.5)


def _shared_conf() -> dict:
    r = docker_exec("worker", ["cat", "/shared/pagespeed-shared.conf"])
    assert r.returncode == 0, r.stderr
    return dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)


def _html_passes(worker: Worker) -> int:
    """HTML passes the worker ran, whether or not they changed the page."""
    stats = worker.stats()
    return int(
        stats["by_type"]["html"]["count"]
        + stats["html_assembly"]["complete"]
        + stats["html_assembly"]["skipped"]
    )


class TestHtmlModeOnTheFront:
    def test_full_equals_disable_html_false(
        self, front_worker, client: ComplianceClient
    ):
        """disable_html=false is html_mode full, everywhere it is read, and
        the front's HTML gets the optimizer's HTML pass under it."""
        front_worker.set_html_mode({"disable_html": True})
        assert front_worker.api("GET", "/v1/config")["html_mode"] == "off"
        front_worker.set_html_mode({"disable_html": False})
        cfg = front_worker.api("GET", "/v1/config")
        assert cfg["html_mode"] == "full" and cfg["disable_html"] is False
        conf = _shared_conf()
        assert conf.get("html_mode") == "full", conf
        assert conf.get("disable_html") == "false", conf
        time.sleep(1.5)  # the front's next shared-config poll
        before = _html_passes(front_worker)
        r = client.get(f"/small.html?mode=full-{uuid.uuid4().hex}")
        assert r.status_code == 200
        assert _poll(lambda: _html_passes(front_worker) > before), (
            "no HTML pass ran under full"
        )

    @pytest.mark.parametrize("mode", ["markdown-static", "markdown-rendered"])
    def test_markdown_mode_writes_no_html_variant(
        self, front_worker, client: ComplianceClient, origin_client, mode
    ):
        """In a markdown-* mode the front's HTML gets no HTML pass, and what it
        serves is the origin's page byte for byte."""
        front_worker.set_html_mode({"html_mode": mode})
        conf = _shared_conf()
        assert conf.get("html_mode") == mode, conf
        # Readers that predate html_mode see the legacy key as "not off".
        assert conf.get("disable_html") == "false", conf
        time.sleep(1.5)  # the front's next shared-config poll
        before = _html_passes(front_worker)
        path = f"/small.html?mode={mode}-{uuid.uuid4().hex}"
        origin = origin_client.get(path)
        deadline = time.time() + 6
        while time.time() < deadline:
            r = client.get(path)
            assert r.status_code == 200
            assert r.content == origin.content, r.headers
            time.sleep(0.5)
        assert _html_passes(front_worker) == before
