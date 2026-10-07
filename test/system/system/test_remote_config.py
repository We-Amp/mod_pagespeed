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

"""Options fetched from a remote configuration server (ModPagespeedRemote-
ConfigurationUrl): applied, skipped when invalid, timed out, cached across a
failed re-fetch, and rejected when out of scope.

Ported from: pagespeed/system/remote_config_test.sh

Three of the bash's start_test blocks are not ported, each for its own
declared reason:
  * "htaccess references a remote configuration file." and "htaccess is
    overridded by remote configuration file." (remote_config_test.sh:60-91)
    write a .htaccess file under the doc root; the lane's test runner user
    does not own that tree (only doc_root_scratch's purge/ and cache_flush/
    subtrees are runner-writable), so these are left out rather than run
    against a filesystem the process cannot write.
  * "config is expired, but is still applied." (remote_config_test.sh:170-186)
    carries no live assertion in the bash -- its own fetch_until check is
    commented out with a TODO -- so there is nothing to port.

"config is forbidden only on first request" (remote_config_test.sh:194-200)
targeted remote-config-forbidden.example.com, the same vhost as the preceding
"config is forbidden, and so never applied"; here it targets
remote-config-initially-forbidden.example.com, the vhost the fixture
provisions for it (matching debug.conf.template's #REMOTE_CONFIG block).

The two "config is forbidden" cases assert what their titles say, where the
bash assertions checked the opposite: a configuration served with a
non-success status (403) is never applied (issue #1064).
"""

import http.client
import os
import subprocess
import time

import pytest

from pagespeed_test_framework import assert_contains, assert_not_contains

FORBIDDEN = "/mod_pagespeed_test/forbidden.html"


def _read_error_log() -> str:
    path = os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        out = subprocess.run(["sudo", "-n", "cat", path], capture_output=True)
        if out.returncode != 0:
            pytest.fail(f"cannot read {path}: {out.stderr!r}")
        return out.stdout.decode("utf-8", "replace")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "remote_config")
class TestRemoteConfig:
    """Bash: remote_config_test.sh (fetched options applied, invalid/expired/
    forbidden/out-of-scope, cached across a failed re-fetch, an experiment)."""

    @pytest.mark.apache_only  # reads the Apache error log (bash: $APACHE_LOG)
    def test_remote_config_will_not_apply_server_scoped_options(self, vhost_client):
        vhost = vhost_client("remote-config-out-of-scope.example.com")
        vhost.get(FORBIDDEN)
        log = _read_error_log()
        # UrlSigningKey (directory scope) is the positive control for the
        # RequestOptionOverride (server scope) absence check below -- an
        # empty or unreadable log now fails the test instead of passing it
        # vacuously (the bash's own gate, [ -s $APACHE_LOG ], is dropped).
        assert (
            "Setting option UrlSigningKey with value secretkey failed" in log
        ), log[-2000:]
        assert (
            "Setting option RequestOptionOverride with value secretkey failed"
            not in log
        ), log[-2000:]

    def test_remote_configuration_on_by_default_comments_and_whitespace_removed(
        self, vhost_client
    ):
        vhost = vhost_client("remote-config.example.com")
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)

    def test_remote_configuration_on_file_missing_end_token(self, vhost_client):
        # /invalid lacks the EndRemoteConfig terminator, so the fetched
        # config is rejected outright; fetch a few times to be satisfied it
        # never gets applied.
        vhost = vhost_client("remote-config-invalid.example.com")
        for _ in range(3):
            response = vhost.get(FORBIDDEN)
            assert_contains(response.text, r"<!--")

    def test_remote_configuration_on_some_invalid_options(self, vhost_client):
        # /partly-invalid has unparseable lines around the valid
        # EnableFilters line; the invalid lines are skipped and the rest
        # still applies.
        vhost = vhost_client("remote-config-partially-invalid.example.com")
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)

    def test_remote_configuration_on_overridden_by_query_parameter(self, vhost_client):
        vhost = vhost_client("remote-config.example.com")
        # First check that the remote config is applied.
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)
        # Then that the query parameter overrides it.
        vhost.fetch_until_count(f"{FORBIDDEN}?PageSpeedFilters=-remove_comments", "<!--", 2)

    def test_second_remote_config_fetch_fails_cached_value_still_applies(self, vhost_client):
        """Bash: second remote config fetch fails, cached value still applies.

        The bash flipped the server to 410 on the first /fail-future fetch;
        here the test arms the failure (GET /fail-future/arm on the server)
        once the config is known to be applied, so that every worker sees the
        same server state.

        The helper serves /fail-future with a five-second lifetime; the test
        then waits past it, so the requests after the wait refetch the
        configuration, get the 410, and must keep applying the last good copy
        (issue #1067).

        The arm is one-way; a rerun against the same live helper server
        needs a fresh server.
        """
        vhost = vhost_client("remote-config-failed-fetch.example.com")
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)
        arm = http.client.HTTPConnection(
            "127.0.0.1",
            int(os.environ.get("PAGESPEED_REMOTE_CONFIG_PORT", "8090")),
            timeout=10,
        )
        try:
            arm.request("GET", "/fail-future/arm")
            armed = arm.getresponse()
            assert armed.status == 200, (
                f"arming /fail-future: HTTP {armed.status} {armed.read()[:120]!r}"
            )
        finally:
            arm.close()
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)
        time.sleep(6)  # past the cached copy's five-second lifetime
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)
        for _ in range(3):
            response = vhost.get(FORBIDDEN)
            assert_not_contains(response.text, r"<!--")

    def test_config_takes_too_long_to_fetch_is_not_applied(self, vhost_client):
        # /timeout waits 10s to respond, past ModPagespeedRemoteConfiguration-
        # TimeoutMs 1500; fetch a few times to be satisfied it never arrives.
        vhost = vhost_client("remote-config-slow-fetch.example.com")
        for _ in range(2):
            response = vhost.get(FORBIDDEN)
            assert_contains(response.text, r"<!--")

    def test_remote_configuration_specify_an_experiment(self, vhost_client):
        # /experiment explicitly enables insert_ga (experiments no longer
        # auto-enable it), so the AnalyticsID it sets shows up in the body.
        vhost = vhost_client("remote-config-experiment.example.com")
        vhost.fetch_until_count(FORBIDDEN, "MyExperimentID", 1)

    def test_config_takes_a_long_time_to_fetch_but_is_still_applied(self, vhost_client):
        # /slightly-slow waits 2s, past the 1500ms timeout, but a background
        # refresh eventually caches it; by the time this test runs (well
        # after the sleep(2) above) that background fetch has completed.
        vhost = vhost_client("remote-config-slightly-slow-fetch.example.com")
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)

    def test_config_is_forbidden_and_so_never_applied(self, vhost_client):
        """/forbidden answers 403 with a valid configuration body; a
        non-success response is never applied (issue #1064). Fetch a few
        times to be satisfied it never is."""
        vhost = vhost_client("remote-config-forbidden.example.com")
        for _ in range(3):
            response = vhost.get(FORBIDDEN)
            assert_contains(response.text, r"<!--")

    def test_config_is_forbidden_only_on_first_request(self, vhost_client):
        """/forbidden-once answers 403 on its first fetch and the standard
        configuration after that; the 403 is not applied and the later 200
        is (issue #1064). Which request sees the 403 depends on the server's
        startup fetch, so this polls for the configuration to apply."""
        vhost = vhost_client("remote-config-initially-forbidden.example.com")
        vhost.fetch_until_count(FORBIDDEN, "<!--", 0)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
