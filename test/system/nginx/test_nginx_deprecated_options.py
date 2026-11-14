#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Deprecated option names load with a warning; unknown names still fail.

An upgraded nginx configuration may still name options that were removed. As
in Apache, `nginx -t` must accept them as no-ops and warn once per name, while a
name that was never an option keeps failing the configuration test.
"""

import os
import pathlib
import re
import signal
import socket
import subprocess
import time
import urllib.request

import pytest

# The deprecated names and their scopes come straight from the
# AddDeprecatedProperty() calls in rewrite_options.cc, so this list cannot drift
# from the module (the same source-text approach as ngx_daemon_options_pin_test).
_OPTIONS_SOURCE = (
    pathlib.Path(__file__).resolve().parents[3]
    / "net/instaweb/rewriter/rewrite_options.cc"
)
DEPRECATED_SCOPES = dict(
    re.findall(r'AddDeprecatedProperty\(\s*"(\w+)"\s*,\s*(k\w+Scope)\s*\)', _OPTIONS_SOURCE.read_text())
)
DEPRECATED = {name: "1" for name in DEPRECATED_SCOPES}
SERVER_SCOPED = [n for n, s in DEPRECATED_SCOPES.items() if s == "kServerScope"]
DIRECTORY_SCOPED = [n for n, s in DEPRECATED_SCOPES.items() if s == "kDirectoryScope"]


def test_deprecated_list_was_derived():
    assert SERVER_SCOPED and DIRECTORY_SCOPED, DEPRECATED_SCOPES


def _module_line():
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    if not os.environ.get("PAGESPEED_NGINX_BINARY") or not os.path.exists(conf):
        pytest.skip("run via run_nginx_tests.sh (nginx binary and config)")
    m = re.search(r"^\s*load_module\s+[^;]+;", open(conf).read(), re.M)
    return m.group(0) if m else ""


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class _Instance:
    """A throwaway nginx with its own prefix, pid file, port and error log."""

    def __init__(self, tmp_path, http_lines=(), server_lines=(), location_lines=()):
        self.binary = os.environ["PAGESPEED_NGINX_BINARY"]
        self.dir = tmp_path
        self.port = _free_port()
        (tmp_path / "cache").mkdir()
        (tmp_path / "www").mkdir()
        (tmp_path / "www" / "index.html").write_text("<html><body>hi</body></html>")
        self.log = tmp_path / "error.log"
        self.conf = tmp_path / "nginx.conf"
        self.conf.write_text(
            f"""{_module_line()}
worker_processes 1;
pid {tmp_path}/nginx.pid;
error_log {self.log} info;
events {{ worker_connections 64; }}
http {{
  access_log off;
  pagespeed FileCachePath {tmp_path}/cache;
  pagespeed ProcessScriptVariables all;
  {chr(10).join(http_lines)}
  server {{
    listen 127.0.0.1:{self.port};
    root {tmp_path}/www;
    pagespeed on;
    {chr(10).join(server_lines)}
    location / {{
      {chr(10).join(location_lines)}
    }}
  }}
}}
"""
        )

    def _run(self, *extra):
        return subprocess.run(
            [self.binary, "-p", str(self.dir), "-e", str(self.log), "-c", str(self.conf), *extra],
            capture_output=True, text=True, timeout=60,
        )

    def test(self):
        return self._run("-t")

    def start(self):
        r = self._run()
        assert r.returncode == 0, r.stderr + self.log_text()
        return self

    def pid(self):
        return int((self.dir / "nginx.pid").read_text())

    def log_text(self):
        return self.log.read_text() if self.log.exists() else ""

    def warnings(self, name):
        return len(re.findall(rf"\[warn\].*pagespeed {name} is deprecated and has no effect", self.log_text(), re.I))

    def stop(self):
        try:
            os.kill(self.pid(), signal.SIGTERM)
        except (OSError, ValueError, FileNotFoundError):
            pass


def _config_test(tmp_path, http_lines, location_lines=()):
    binary = os.environ.get("PAGESPEED_NGINX_BINARY", "")
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    if not binary or not os.path.exists(conf):
        pytest.skip("run via run_nginx_tests.sh (nginx binary and config)")
    text = open(conf).read()
    # Keep `nginx -t` from touching the shared cache directory.
    cache = tmp_path / "cache"
    cache.mkdir()
    text, n = re.subn(
        r"pagespeed\s+FileCachePath\s+[^;]+;", f"pagespeed FileCachePath {cache};", text
    )
    if n == 0:
        pytest.skip("the test config carries no FileCachePath (module not loaded)")
    text, n = re.subn(r"\bhttp\s*\{", "http {\n" + "\n".join(http_lines) + "\n", text, count=1)
    assert n == 1, "no http block in the test config"
    if location_lines:
        text, n = re.subn(
            r"\blocation\s+/\s*\{", "location / {\n" + "\n".join(location_lines) + "\n", text, count=1
        )
        assert n == 1, "no `location /` block in the test config"
    test_conf = os.path.join(os.path.dirname(conf), "nginx_deprecated_options.conf")
    with open(test_conf, "w") as f:
        f.write(text)
    try:
        return subprocess.run(
            [binary, "-t", "-p", os.path.dirname(conf), "-e", "stderr", "-c", test_conf],
            capture_output=True,
            text=True,
            timeout=60,
        )
    finally:
        os.unlink(test_conf)


def test_deprecated_names_load_with_one_warning_each(tmp_path):
    http_lines = [f"pagespeed {name} {value};" for name, value in DEPRECATED.items()]
    # A second occurrence, in another context and spelled with the legacy
    # prefix and another case, must not repeat the warning.
    http_lines.append("pagespeed ModPagespeedFileCacheInodeLimit 1000;")
    r = _config_test(
        tmp_path,
        http_lines,
        location_lines=["pagespeed maxprefetchjselements 2;"],
    )
    assert r.returncode == 0, r.stderr
    for name in DEPRECATED:
        warnings = re.findall(
            rf"\[warn\].*pagespeed {name} is deprecated and has no effect", r.stderr, re.I
        )
        assert len(warnings) == 1, f"{name}: {len(warnings)} warnings\n{r.stderr}"


def test_unknown_name_still_fails(tmp_path):
    r = _config_test(tmp_path, ["pagespeed FileCacheInodeLimitt 500000;"])
    assert r.returncode != 0, r.stderr
    assert "not recognized" in r.stderr, r.stderr
    assert "is deprecated" not in r.stderr, r.stderr


def test_server_scoped_names_rejected_in_location(tmp_path):
    """A name keeps the scope it had as an option: not accepted in a location."""
    for name in SERVER_SCOPED:
        d = tmp_path / name
        d.mkdir()
        r = _Instance(d, location_lines=[f"pagespeed {name} 1;"]).test()
        assert r.returncode != 0, f"{name} accepted in a location\n{r.stderr}"
        assert "cannot be set at this scope" in r.stderr, r.stderr


def test_server_scoped_names_accepted_in_server(tmp_path):
    inst = _Instance(tmp_path, server_lines=[f"pagespeed {n} 1;" for n in SERVER_SCOPED])
    r = inst.test()
    assert r.returncode == 0, r.stderr


def test_script_variable_line_with_deprecated_name(tmp_path):
    """A deprecated name whose value is a script variable still loads and serves."""
    name = DIRECTORY_SCOPED[0]
    inst = _Instance(tmp_path, location_lines=[f"pagespeed {name} $arg_n;"]).start()
    try:
        for _ in range(2):
            with urllib.request.urlopen(f"http://127.0.0.1:{inst.port}/index.html?n=3", timeout=30) as resp:
                assert resp.status == 200
        assert "emerg" not in inst.log_text() and "[alert]" not in inst.log_text(), inst.log_text()
        # One warning per name however many requests evaluate the line.
        assert inst.warnings(name) <= 1, inst.log_text()
    finally:
        inst.stop()


def test_warning_reappears_after_reload(tmp_path):
    """The warn-once set is per factory, so a reload warns again."""
    names = list(DEPRECATED)
    inst = _Instance(tmp_path, http_lines=[f"pagespeed {n} 1;" for n in names]).start()
    try:
        assert all(inst.warnings(n) == 1 for n in names), inst.log_text()
        os.kill(inst.pid(), signal.SIGHUP)
        deadline = time.time() + 30
        while time.time() < deadline and not all(inst.warnings(n) == 2 for n in names):
            time.sleep(0.2)
        assert all(inst.warnings(n) == 2 for n in names), inst.log_text()
    finally:
        inst.stop()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
