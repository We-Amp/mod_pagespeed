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
    line -- read from the message history on Apache and from the error log
    (``PAGESPEED_NGINX_ERROR_LOG``) on nginx --, never a warning, and -- the
    rest of this suite passing with the file present -- no change in
    behaviour.
"""

import json
import os
from pathlib import Path

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

    Where the notice is read from differs per port. It is emitted once per
    process at startup, so it is the oldest entry in the admin message
    history -- a 100 KB ring (MessageBufferSize in every rig) that this suite
    fills shortly after this test runs on a single-worker nginx, evicting it.
    Apache keeps reading the message history (MPM children respawn and
    re-emit); nginx reads the server error log, which both nginx rigs export
    as PAGESPEED_NGINX_ERROR_LOG with error_log at level info.
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

        messages = _message_history(client, server_config)
        notices = [m for m in messages if STALE_NOTICE in m["message"]]
        assert notices, (
            "expected the stale-license INFO notice in the message history; "
            f"got {len(messages)} messages, none matching {STALE_NOTICE!r}"
        )
        for notice in notices:
            assert notice["severity"] == "info", notice
            assert "pagespeed.license" in notice["message"], notice

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
