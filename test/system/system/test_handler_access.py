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

"""Per-handler, per-vhost *Domains allow/deny lists.

Ported from: pagespeed/system/system_tests/handler_access_messages.sh

Top-level MessagesDomains list plus per-vhost lists (CLEAR_INHERITED via
`Disallow *`, wildcards, aliases) on name-based vhosts of the secondary
port. Handler paths are the Apache lane's (pagespeed/apache/system_test.sh).
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_header_contains,
    assert_http_status,
    require_status_ok,
)

MESSAGES = "mod_pagespeed_message"
STATISTICS = "mod_pagespeed_statistics"
GLOBAL_STATISTICS = "mod_pagespeed_global_statistics"
_ALL_HANDLERS = (
    STATISTICS,
    GLOBAL_STATISTICS,
    "pagespeed_admin/",
    "pagespeed_global_admin/",
    "pagespeed_console",
)

_ALLOWED = [
    ("messages-allowed", MESSAGES),             # Listed at top level.
    ("more-messages-allowed", MESSAGES),        # Listed at top level.
    ("but-this-message-allowed", MESSAGES),     # Listed at VHost level.
    ("and-this-one", MESSAGES),                 # Listed at VHost level.
    # Listed at top level, VHost level lists both this and CLEAR_INHERITED.
    ("cleared-inherited-reallowed", MESSAGES),
    # Not listed at top level, VHost level lists both this and CLEAR_INHERITED.
    ("messages-allowed-at-vhost", MESSAGES),
    # Not listed at any level, VHost level lists only CLEAR_INHERITED.
    ("cleared-inherited-unlisted", MESSAGES),
    ("anything-a-wildcard", MESSAGES),          # Listed at top level, via wildcard.
    ("anything-b-wildcard", MESSAGES),          # Listed at top level, via wildcard.
] + [
    # No <Handler>Domains listings for these, default is allow.
    ("nothing-explicitly-allowed", handler) for handler in _ALL_HANDLERS
] + [
    # Listed at VHost level as allowed.
    ("everything-explicitly-allowed", handler) for handler in _ALL_HANDLERS
]

_DENIED = [
    ("messages-still-not-allowed", MESSAGES),   # Not listed at any level.
    # Listed at top level, VHost level lists CLEAR_INHERITED. (The bash
    # checks this one twice, lines 48 and 69; once is the same assertion.)
    ("cleared-inherited", MESSAGES),
    # Not listed at any level, VHost lists CLEAR_INHERITED and other domains.
    ("messages-not-allowed-at-vhost", MESSAGES),
    # Listed at top level, via wildcard, VHost level lists CLEAR_INHERITED.
    ("anything-c-wildcard", MESSAGES),
    ("nothing-allowed", MESSAGES),              # VHost lists deny *
    ("messages-not-allowed", MESSAGES),         # Not listed at any level.
] + [
    # Other domains listed at VHost level as allowed, none of these listed.
    ("everything-explicitly-allowed-but-aliased", handler)
    for handler in _ALL_HANDLERS
]


def _ids(cases):
    return [f"{host}:{handler}" for host, handler in cases]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "admin_handlers")
class TestHandlerAccess:
    """Per-vhost admin/statistics/console/messages handler ACLs.

    handler_access_messages.sh's single `start_test Handler access
    restrictions` label covers every expect_handler/expect_messages call;
    each parametrized case here is one bash assertion.
    """

    @pytest.mark.parametrize("host_prefix,handler", _ALLOWED, ids=_ids(_ALLOWED))
    def test_handler_allowed(self, vhost_client, host_prefix, handler):
        """start_test Handler access restrictions (allow): check [ "$OUT" = "200" ]"""
        response = vhost_client(f"{host_prefix}.example.com").get(f"/{handler}")
        assert_http_status(
            response, 200, f"/{handler} on {host_prefix}.example.com must be allowed")

    @pytest.mark.parametrize("host_prefix,handler", _DENIED, ids=_ids(_DENIED))
    def test_handler_denied(self, vhost_client, host_prefix, handler):
        """start_test Handler access restrictions (deny): check [ "$OUT" = "403" -o "$OUT" = "404" ]"""
        response = vhost_client(f"{host_prefix}.example.com").get(f"/{handler}")
        assert response.status in (403, 404), (
            f"/{handler} on {host_prefix}.example.com must be denied (403/404), "
            f"got {response.status}:\n{response.text[:300]}"
        )


class TestMessagesHandlerLocalhost:
    """The lane's own admin tooling keeps reaching the messages handler.

    Not from the bash suite: install/debug.conf.template's server-scope
    ModPagespeedMessagesDomains list ends in `Allow localhost`
    (setup_apache_test.sh, configure_fixture_server_scope) specifically so
    this port's own Domains restrictions never lock out the lane itself.
    """

    # The restrictive MessagesDomains list is part of the admin_handlers lane
    # fixture; without it this check would pass vacuously.
    @pytest.mark.requires_fixture("admin_handlers")
    def test_messages_still_allowed_for_localhost(self, client: PageSpeedClient):
        """Guard: the top-level allow list keeps `localhost` allowed."""
        require_status_ok(client.get(f"/{MESSAGES}"),
                          "messages handler on the primary host")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestMessagesHandlerResponse:
    """The messages handler's JSON shape and cache-control, on messages-allowed.

    Ported from: pagespeed/system/system_tests/handler_access_messages.sh
    (mpp #1040: the messages handler's JSON contract -- process-scoped
    JSON, no-store/nosniff/no ETag).
    """

    def test_messages_json(self, vhost_client):
        """start_test Messages handler answers JSON on every port"""
        response = vhost_client("messages-allowed.example.com").get(f"/{MESSAGES}")
        require_status_ok(response, "messages handler")
        assert_contains(response, r'"messages":\[')
        assert_contains(response, r'"scope":"process"')

    def test_messages_not_cacheable(self, vhost_client):
        """start_test Messages handler is not cacheable on any port"""
        response = vhost_client("messages-allowed.example.com").get(f"/{MESSAGES}")
        require_status_ok(response, "messages handler")
        assert_header_contains(response, "Cache-Control", r"(?i)^no-store, private")
        assert_header_contains(response, "X-Content-Type-Options", r"(?i)^nosniff")
        assert not response.header("ETag"), (
            f"Messages response must carry no ETag, got {response.header('ETag')!r}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
