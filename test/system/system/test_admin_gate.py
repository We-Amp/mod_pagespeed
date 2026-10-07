#!/usr/bin/env python3
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

"""Admin method and origin gates.

Ported from: pagespeed/system/system_tests/admin_method_guard.sh
             (the bash suite remains the developer-run equivalent)

The daemon proxy under the admin path is read-only, the cache actions
answer only a same-origin POST, and the whole-cache purge is global-admin
only. These are answered by the module itself, whatever the web server.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_header_contains,
    assert_http_status,
)

_SAME_ORIGIN = {"X-Requested-With": "XMLHttpRequest"}


class TestAdminGate:
    """Method, origin and scope gates on the admin pages."""

    @pytest.mark.apache_only
    def test_daemon_proxy_refuses_post(
        self, client: PageSpeedClient, server_config
    ):
        """Bash original:
            OUT=$($CURL -sS -D - -o /dev/null -X POST "$HEALTH")
            check_from "$OUT" grep -qi '^Allow: GET, HEAD'
        """
        response = client.post(f"{server_config.admin_path}/v1/daemon/health")
        assert_http_status(response, 405)
        assert response.header("Allow") == "GET, HEAD", (
            f"Expected 'Allow: GET, HEAD', got {response.header('Allow')!r}"
        )

    @pytest.mark.apache_only
    def test_purge_with_get_is_405(
        self, client: PageSpeedClient, server_config
    ):
        """A GET never purges: 405 with Allow: POST."""
        response = client.get(f"{server_config.admin_path}/cache?purge=*")
        assert_http_status(response, 405)
        assert response.header("Allow") == "POST", (
            f"Expected 'Allow: POST', got {response.header('Allow')!r}"
        )

    @pytest.mark.apache_only
    def test_purge_post_without_header_is_403(
        self, client: PageSpeedClient, server_config
    ):
        """A POST without the console's X-Requested-With header is refused."""
        response = client.post(f"{server_config.admin_path}/cache?purge=*")
        assert_http_status(response, 403)

    @pytest.mark.apache_only
    def test_whole_cache_purge_is_global_admin_only(
        self, client: PageSpeedClient, server_config
    ):
        """A same-origin purge=* on the per-host admin is refused (403)."""
        response = client.post(
            f"{server_config.admin_path}/cache?purge=*", headers=_SAME_ORIGIN
        )
        assert_http_status(response, 403)
        assert "global admin" in response.text, (
            f"Expected the global-admin refusal, got: {response.text[:200]}"
        )

    @pytest.mark.apache_only
    def test_config_json_is_not_storable(
        self, client: PageSpeedClient, server_config
    ):
        """Admin JSON is no-store and nosniff."""
        response = client.get(f"{server_config.admin_path}/config")
        assert_http_status(response, 200)
        assert_header_contains(response, "Content-Type", "application/json")
        assert_header_contains(response, "Cache-Control", "no-store")
        assert_header_contains(response, "X-Content-Type-Options", "nosniff")

    def test_daemon_proxy_refuses_delete(self, client: PageSpeedClient, server_config):
        """Bash original (admin_method_guard.sh:21-22):
            OUT=$($CURL -sS -o /dev/null -w '%{http_code}' -X DELETE "$HEALTH")
            check [ "$OUT" = "405" ]
        """
        response = client._request("DELETE", f"{server_config.admin_path}/v1/daemon/health")
        assert_http_status(response, 405)

    def test_daemon_proxy_answers_head(self, client: PageSpeedClient, server_config):
        """Bash original (admin_method_guard.sh:23-25):
            # HEAD keeps working and carries no body.
            OUT=$($CURL -sS -I "$HEALTH" | head -1)
            check_from "$OUT" grep -qE ' (200|502) '
        """
        response = client._request("HEAD", f"{server_config.admin_path}/v1/daemon/health")
        assert response.status in (200, 502), (
            f"HEAD on the daemon proxy: expected 200 or 502, got {response.status}"
        )

    @pytest.mark.apache_only  # asserts the Apache directive spelling (bash: apache/)
    def test_whole_cache_purge_names_the_enable_directive(
        self, client: PageSpeedClient, server_config
    ):
        """Bash original (purging_disabled.sh:15-18): with purging disabled in the
        base config, the refusal names the directive that enables it."""
        response = client.post(
            f"{server_config.global_admin_path}/cache?purge=*", headers=_SAME_ORIGIN
        )
        assert "ModPagespeedEnableCachePurge on" in response.text, (
            f"expected the enable-directive hint, got HTTP {response.status}: "
            f"{response.text[:300]}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
