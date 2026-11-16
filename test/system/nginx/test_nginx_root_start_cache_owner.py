#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""A start as root creates the cache for the worker user.

nginx started as root with a non-root `user` must leave the cache
directory and every cache volume file in it owned by that user, also when a
root-owned volume from an earlier run is already there, and `nginx -t` must
leave no cache file and no directory the workers cannot use, wherever the
`user` directive stands. Needs root; skipped otherwise.
"""

import grp
import os
import pwd
import re
import signal
import socket
import subprocess
import time

import pytest

WORKER = "nobody"


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_gone(pid, timeout=20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.1)
    return False


def _owners(path):
    out = {}
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            full = os.path.join(root, name)
            out[os.path.relpath(full, path)] = os.lstat(full).st_uid
    return out


@pytest.fixture(params=["user-first", "user-last"])
def root_nginx(tmp_path, request):
    binary = os.environ.get("PAGESPEED_NGINX_BINARY", "")
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    if not binary or not os.path.exists(conf):
        pytest.skip("run via run_nginx_tests.sh (nginx binary and config)")
    if os.geteuid() != 0:
        pytest.skip("needs root: the start under test is a start as root")
    try:
        worker = pwd.getpwnam(WORKER)
    except KeyError:
        pytest.skip(f"no {WORKER} user")
    group = grp.getgrgid(worker.pw_gid).gr_name
    load_module = re.findall(r"^\s*load_module\s+[^;]+;", open(conf).read(), re.M)

    base = tmp_path / "root_start"
    base.mkdir()
    os.chmod(tmp_path, 0o755)
    os.chmod(base, 0o755)
    cache = base / "cache"
    test_conf = base / "nginx.conf"
    pid_file = base / "nginx.pid"
    user_line = f"user {WORKER} {group};"
    first = user_line if request.param == "user-first" else ""
    last = user_line if request.param == "user-last" else ""
    test_conf.write_text(
        "\n".join(load_module)
        + f"""
{first}
worker_processes 1;
pid {pid_file};
error_log {base}/error.log info;
events {{ worker_connections 64; }}
http {{
  access_log off;
  client_body_temp_path {base}/body;
  proxy_temp_path {base}/proxy;
  fastcgi_temp_path {base}/fastcgi;
  uwsgi_temp_path {base}/uwsgi;
  scgi_temp_path {base}/scgi;
  pagespeed on;
  pagespeed FileCachePath {cache};
  server {{ listen 127.0.0.1:{_free_port()}; location / {{ return 200; }} }}
}}
{last}
"""
    )

    def run(*args):
        return subprocess.run(
            [binary, "-p", str(base), "-e", "stderr", "-c", str(test_conf), *args],
            capture_output=True,
            text=True,
            timeout=120,
        )

    def start_and_stop():
        r = run()
        assert r.returncode == 0, r.stderr
        deadline = time.monotonic() + 60
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        pid = int(pid_file.read_text().strip())
        # Our own master, by the pid file it just wrote.
        os.kill(pid, signal.SIGQUIT)
        assert _wait_gone(pid), "nginx did not stop"

    return run, start_and_stop, cache, worker.pw_uid


def test_root_start_creates_cache_for_worker_user(root_nginx):
    run, start_and_stop, cache, worker_uid = root_nginx

    r = run("-t")
    assert r.returncode == 0, r.stderr
    assert not cache.exists() or (
        _owners(cache) == {} and os.stat(cache).st_uid == worker_uid
    )

    start_and_stop()
    owners = _owners(cache)
    volumes = [p for p in owners if os.path.basename(p).startswith("cyclone")]
    assert volumes, f"no cache volume created: {sorted(owners)}"
    assert os.stat(cache).st_uid == worker_uid
    assert {p: u for p, u in owners.items() if p in volumes} == {
        p: worker_uid for p in volumes
    }

    # A root-owned volume left by an earlier run is given to the worker user.
    for p in volumes:
        os.chown(cache / p, 0, 0)
    start_and_stop()
    assert {p: os.stat(cache / p).st_uid for p in volumes} == {
        p: worker_uid for p in volumes
    }


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
