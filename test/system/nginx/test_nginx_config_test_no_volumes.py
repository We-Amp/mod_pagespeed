#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""`nginx -t` must validate the configuration without creating cache volumes."""

import os
import re
import subprocess

import pytest


def test_config_test_creates_no_cache_files(tmp_path):
    binary = os.environ.get("PAGESPEED_NGINX_BINARY", "")
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    if not binary or not os.path.exists(conf):
        pytest.skip("run via run_nginx_tests.sh (nginx binary and config)")
    cache = tmp_path / "fresh_cache"
    cache.mkdir()
    text = open(conf).read()
    text, n = re.subn(
        r"pagespeed\s+FileCachePath\s+[^;]+;", f"pagespeed FileCachePath {cache};", text
    )
    if n == 0:
        pytest.skip("the test config carries no FileCachePath (module not loaded)")
    assert n == 1, "expected exactly one FileCachePath in the test config"
    test_conf = os.path.join(os.path.dirname(conf), "nginx_config_test_only.conf")
    with open(test_conf, "w") as f:
        f.write(text)
    try:
        # Same prefix and error-log flags as the harness's own `nginx -t`: the
        # relative temp directories live under the config directory, and the
        # compiled-in log path does not exist on the test host.
        r = subprocess.run(
            [binary, "-t", "-p", os.path.dirname(conf), "-e", "stderr", "-c", test_conf],
            capture_output=True,
            text=True,
            timeout=60,
        )
    finally:
        os.unlink(test_conf)
    assert r.returncode == 0, r.stderr
    assert [p.name for p in cache.rglob("*")] == []


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
