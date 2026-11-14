#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Deprecated option names load with a warning; unknown names still fail.

An upgraded nginx configuration may still name options that were removed. As
in Apache, `nginx -t` must accept them as no-ops and warn once per name, while a
name that was never an option keeps failing the configuration test.
"""

import os
import re
import subprocess

import pytest

# Mirrors the AddDeprecatedProperty() list in
# net/instaweb/rewriter/rewrite_options.cc.
DEPRECATED = {
    "MaxPrefetchJsElements": "0",
    "DistributeFetches": "on",
    "DistributedRewriteKey": "key",
    "DistributedRewriteServers": "localhost:1",
    "DistributedRewriteTimeoutMs": "100",
    "FileCacheInodeLimit": "500000",
    "FileCacheCleanIntervalMs": "3600000",
}


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
    assert "deprecated" not in r.stderr, r.stderr


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
