#!/usr/bin/env python3
# Copyright (c) 2026 We-Amp B.V.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""mod_pagespeed 2.1 ships without a licensing apparatus.

Cross-port GA-gate assertions; this file runs under every port's system-test
job (Apache / nginx / Envoy / IIS):

  * the optimized HTML path never carries an ``X-PageSpeed-Warn`` header and
    admin responses never carry ``x-need-renew`` -- both were license-derived
    signals in earlier releases;
  * the former ``/v1/license/*`` admin API answers 404 (JSON) on both admin
    handlers, and a POST to it no longer meets a CSRF gate (403);
  * drop-in compatibility: the Apache and nginx rigs stage a stale
    ``pagespeed.license`` next to FileCachePath, where earlier releases kept
    the license token (setup_apache_test.sh, run_nginx_tests.sh and
    package-test/run-nginx-tests.sh). The module must ignore it: one INFO
    line -- read from the Apache error log (``PAGESPEED_APACHE_ERROR_LOG``,
    the lane's ``LogLevel warn pagespeed:info`` keeps it there) on Apache and
    from the error log (``PAGESPEED_NGINX_ERROR_LOG``) on nginx --, never a
    warning, and -- the rest of this suite passing with the file present --
    no change in behaviour. The message history (``/mod_pagespeed_message``)
    is not used for this: it is a size-limited ring that a long lane run can
    evict the startup notice from before this test runs (observed: 546
    messages, notice gone).
"""

import json
import os
import subprocess
from pathlib import Path
from typing import Optional

import pytest

from pagespeed_test_framework import PageSpeedClient, assert_http_status

GLOBAL_ADMIN_PATH = "/pagespeed_global_admin"
RETIRED_LEAVES = ("status", "apply", "activate", "consent")
STALE_NOTICE = "this file is no longer read since 2.1"
# License-state log lines earlier releases emitted; none may appear now.
LICENSE_WARNING_TELLS = (
    "UNLICENSED",
    "License loaded",
    "Invalid license",
    "X-PageSpeed-Warn",
)
CSRF_REJECT_MESSAGE = "Missing or invalid CSRF headers"


def _apache_error_log_path() -> str:
    return os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")


def _read_apache_error_log() -> Optional[str]:
    """Return the whole decoded Apache error log, or None when it cannot be
    read.

    Whole, not tailed: the stale-license notice is emitted once, from
    ChildInit at process startup, and a size-bounded tail can push it out
    of the window on a lane that has since logged enough at
    pagespeed:info -- the same reasoning as
    automatic/test_mpm_thread_resolution.py's _read_error_log for its own
    startup line. None -- not "" -- is the unreadable signal on purpose,
    so a broken probe is never mistaken for a clean/absent result.
    """
    path = _apache_error_log_path()
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except (FileNotFoundError, PermissionError):
        try:
            res = subprocess.run(
                ["sudo", "-n", "cat", path],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=30,
            )
            if res.returncode != 0:
                return None
            return res.stdout.decode("utf-8", errors="replace")
        except (subprocess.SubprocessError, OSError):
            return None


def _optimized_html(client: PageSpeedClient, example_root: str):
    """An HTML response that went through the rewriter (rewritten CSS ref)."""
    url = f"{example_root}/rewrite_css_images.html?PageSpeedFilters=rewrite_css"
    response = client.fetch_until(
        url,
        condition=lambda r: "rewrite_css_images.css.pagespeed.cf" in r.text,
        timeout=30.0,
    )
    assert_http_status(response, 200)
    return response


class TestNoLicenseDerivedHeaders:
    """No response carries a license-state header on any port."""

    def test_optimized_html_has_no_warn_header(
        self, client: PageSpeedClient, example_root: str
    ):
        response = _optimized_html(client, example_root)
        warn = response.header("X-PageSpeed-Warn")
        assert warn == "", (
            f"optimized HTML must not carry X-PageSpeed-Warn; got {warn!r}"
        )

    def test_optimized_html_has_no_renew_header(
        self, client: PageSpeedClient, example_root: str
    ):
        response = _optimized_html(client, example_root)
        assert response.header("x-need-renew") == ""

    def test_admin_root_has_no_renew_header(
        self, client: PageSpeedClient, server_config
    ):
        response = client.get(server_config.admin_path.rstrip("/") + "/")
        assert_http_status(response, 200)
        assert response.header("x-need-renew") == "", (
            "admin responses must not carry x-need-renew"
        )


class TestRetiredLicenseRoutes:
    """The /v1/license/* admin API is gone: 404 JSON, never a config dump,
    never a 5xx, never a CSRF 403."""

    @pytest.mark.parametrize("leaf", RETIRED_LEAVES)
    def test_get_answers_404(self, client: PageSpeedClient, server_config, leaf):
        for admin in (server_config.admin_path.rstrip("/"), GLOBAL_ADMIN_PATH):
            response = client.get(f"{admin}/v1/license/{leaf}")
            assert response.status == 404, (
                f"GET {admin}/v1/license/{leaf} must be 404, got "
                f"{response.status} body={response.text[:200]!r}"
            )
            assert "FileCachePath" not in response.text, (
                "a retired license route must not reach the config dump"
            )

    def test_post_answers_404_not_csrf_403(self, client: PageSpeedClient):
        response = client.post(
            f"{GLOBAL_ADMIN_PATH}/v1/license/consent",
            data='{"accepted":true}',
            headers={
                "Content-Type": "application/json",
                "X-Requested-With": "XMLHttpRequest",
            },
        )
        assert response.status == 404, (
            f"POST consent must be 404, got {response.status} "
            f"body={response.text[:200]!r}"
        )
        assert CSRF_REJECT_MESSAGE not in response.text

    def test_post_without_json_headers_is_still_404(self, client: PageSpeedClient):
        response = client.post(
            f"{GLOBAL_ADMIN_PATH}/v1/license/apply",
            data='{"token":"stale"}',
            headers=None,
        )
        assert response.status == 404, (
            f"POST apply must be 404, got {response.status} "
            f"body={response.text[:200]!r}"
        )
        assert CSRF_REJECT_MESSAGE not in response.text


def _message_history(client: PageSpeedClient, server_config) -> list:
    response = client.get(f"{server_config.admin_path.rstrip('/')}/message_history")
    assert_http_status(response, 200)
    return json.loads(response.text)["messages"]


@pytest.mark.not_envoy  # only the Apache and nginx rigs stage the fixture
@pytest.mark.not_iis
class TestStaleLicenseFileIgnored:
    """A leftover pagespeed.license is noticed once at INFO and otherwise
    ignored (the rest of the suite passing with it present is the
    no-behaviour-change half of the assertion).

    Where the notice is read from differs per port, but both now read their
    own error log rather than the admin message history: the message history
    is a size-limited ring (MessageBufferSize) that a long lane run can evict
    the once-per-process startup notice from before this test runs (observed:
    546 messages, notice gone). Apache reads PAGESPEED_APACHE_ERROR_LOG whole
    (the same reader pattern as test_mpm_thread_resolution.py, so a tail
    window cannot push the once-per-process notice out of view); the lane's
    `LogLevel warn pagespeed:info` (setup_apache_test.sh) is what keeps an
    INFO-severity module message in that file at all -- Apache's default
    LogLevel (warn) would otherwise filter it out before it reaches the file.
    nginx reads PAGESPEED_NGINX_ERROR_LOG with error_log at level info.
    """

    def test_stale_file_is_noticed_once_at_info(
        self, client: PageSpeedClient, server_config
    ):
        if server_config.is_nginx:
            log_path = os.environ.get("PAGESPEED_NGINX_ERROR_LOG", "")
            if not log_path:
                pytest.skip(
                    "PAGESPEED_NGINX_ERROR_LOG not set; run via "
                    "run_nginx_tests.sh or package-test/run-nginx-tests.sh"
                )
            lines = Path(log_path).read_text(errors="replace").splitlines()
            notices = [line for line in lines if STALE_NOTICE in line]
            assert notices, (
                f"expected the stale-license INFO notice in {log_path}; "
                f"got {len(lines)} lines, none matching {STALE_NOTICE!r}"
            )
            for notice in notices:
                assert "[info]" in notice, notice
                assert "pagespeed.license" in notice, notice
            return

        log_text = _read_apache_error_log()
        assert log_text is not None, (
            f"could not read {_apache_error_log_path()} "
            f"(PAGESPEED_APACHE_ERROR_LOG); the reader needs either direct "
            f"read access or passwordless sudo"
        )
        lines = log_text.splitlines()
        notices = [line for line in lines if STALE_NOTICE in line]
        assert notices, (
            f"expected the stale-license INFO notice in "
            f"{_apache_error_log_path()}; got {len(lines)} lines in the "
            f"log, none matching {STALE_NOTICE!r}"
        )
        for notice in notices:
            assert "[pagespeed:info]" in notice, notice
            assert "pagespeed.license" in notice, notice

    def test_message_history_has_no_license_warning(
        self, client: PageSpeedClient, server_config
    ):
        """Runs on every port that stages the fixture, whatever the ring holds."""
        for m in _message_history(client, server_config):
            text = m["message"]
            for tell in LICENSE_WARNING_TELLS:
                assert tell not in text, m

if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite --
    # vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
