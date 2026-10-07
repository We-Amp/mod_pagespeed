#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""nginx handler access-rule tests.

The lane runs one server block (PAGESPEED_HANDLER_ACL_PORT, see
run_nginx_tests.sh) that restricts the admin-style handler paths in the
documented form: `location = /path` for a handler's own page, plus
`location ^~ /path/` for the pages below the two admin paths, each with
`allow 127.0.0.1; deny all;`.

The module selects these handlers the way nginx selects those `location`
blocks, so the rules apply to every page a handler answers for, and a request
is served by a handler only when the server and the module select the same
one for it.

Requests are sent from two loopback addresses: 127.0.0.1 is the allowed
client, 127.0.0.2 is a client the rules refuse.
"""

import http.client
import os
from typing import Dict, Optional, Tuple

import pytest

ALLOWED_CLIENT = "127.0.0.1"
REFUSED_CLIENT = "127.0.0.2"

ADMIN = "/pagespeed_admin"
GLOBAL_ADMIN = "/pagespeed_global_admin"
STATISTICS = "/pagespeed_statistics"
GLOBAL_STATISTICS = "/pagespeed_global_statistics"
CONSOLE = "/pagespeed_console"
MESSAGES = "/ngx_pagespeed_message"

# Every page the restricted handlers answer for on this server block whose
# location answers a refused client with a plain 403.
RESTRICTED_PAGES = (
    ADMIN,
    f"{ADMIN}/",
    f"{ADMIN}/statistics",
    f"{ADMIN}/config",
    f"{ADMIN}/cache",
    f"{ADMIN}/message_history",
    f"{ADMIN}/console",
    f"{ADMIN}/statistics?json",
    f"{ADMIN}/v1/daemon/health",
    STATISTICS,
    f"{STATISTICS}?json",
    CONSOLE,
    MESSAGES,
)

# Request targets for which the server and the module do not select the same
# handler. None of them names a file under the document root.
DISAGREEING_TARGETS = (
    f"/x\\..\\{ADMIN[1:]}",
    f"/x\\..\\{ADMIN[1:]}/statistics",
    f"/x\\..\\{ADMIN[1:]}/v1/daemon/health",
    f"/static/..\\{ADMIN[1:]}/config",
    f"/x\\..\\{GLOBAL_ADMIN[1:]}",
    f"/x\\..\\{STATISTICS[1:]}",
    f"/x\\..\\{GLOBAL_STATISTICS[1:]}",
    f"/x\\..\\{CONSOLE[1:]}",
    f"/x\\..\\{MESSAGES[1:]}",
)

# Paths that start like a handler path but are not below it.
SIBLING_PATHS = (
    f"{ADMIN}X",
    f"{ADMIN}2/statistics",
    f"{ADMIN}.txt",
    f"{GLOBAL_ADMIN}X",
    f"{STATISTICS}/",
    f"{CONSOLE}/x",
)


def _fetch(
    target: str,
    source: str,
    method: str = "GET",
    headers: Optional[Dict[str, str]] = None,
    port: Optional[int] = None,
) -> Tuple[int, str, str]:
    """Sends one request from `source`; returns (status, content type, body).

    The request target goes on the wire exactly as given. `port` defaults to
    the restricted server block.
    """
    if port is None:
        port = int(os.environ["PAGESPEED_HANDLER_ACL_PORT"])
    connection = http.client.HTTPConnection(
        "127.0.0.1", port, timeout=30, source_address=(source, 0)
    )
    try:
        connection.putrequest(method, target, skip_accept_encoding=True)
        for name, value in (headers or {}).items():
            connection.putheader(name, value)
        connection.endheaders()
        response = connection.getresponse()
        body = response.read().decode("utf-8", errors="replace")
        return response.status, response.getheader("Content-Type", ""), body
    finally:
        connection.close()


@pytest.mark.nginx_only
class TestHandlerAccessRules:
    """Access rules in the documented form cover what the handlers serve."""

    def test_rules_apply_to_every_handler_page(self):
        """The handler path, its sub-pages and an API route are refused."""
        for page in RESTRICTED_PAGES:
            status, _, _ = _fetch(page, REFUSED_CLIENT)
            assert status == 403, (
                f"{page} from a client the rules refuse: expected 403, got "
                f"{status}"
            )

    def test_rules_apply_to_post(self):
        status, _, _ = _fetch(
            f"{ADMIN}/cache?purge=*",
            REFUSED_CLIENT,
            method="POST",
            headers={
                "X-Requested-With": "XMLHttpRequest",
                "Content-Length": "0",
            },
        )
        assert status == 403, f"expected 403, got {status}"

    def test_request_the_server_and_module_read_differently_is_not_served(self):
        """Such a request is left to the server, whoever sends it."""
        for source in (REFUSED_CLIENT, ALLOWED_CLIENT):
            for target in DISAGREEING_TARGETS:
                status, _, body = _fetch(target, source)
                assert status == 404, (
                    f"request {DISAGREEING_TARGETS.index(target)} from "
                    f"{source} must be left to the server (404), got "
                    f"{status}: {body[:200]!r}"
                )

    def test_paths_next_to_a_handler_path_are_not_served(self):
        """A handler answers for its own path and the pages below it only."""
        for source in (REFUSED_CLIENT, ALLOWED_CLIENT):
            for path in SIBLING_PATHS:
                status, _, body = _fetch(path, source)
                assert status == 404, (
                    f"{path} from {source} is not a handler page: expected "
                    f"404, got {status}: {body[:200]!r}"
                )

    def test_internally_redirected_request_is_left_to_the_server(self):
        """A refused request the server hands to a named location is answered
        by the server, not by a handler."""
        for page in (
            GLOBAL_STATISTICS,
            GLOBAL_ADMIN,
            f"{GLOBAL_ADMIN}/",
            f"{GLOBAL_ADMIN}/statistics",
            f"{GLOBAL_ADMIN}/v1/daemon/health",
        ):
            status, _, body = _fetch(page, REFUSED_CLIENT)
            assert status == 404, (
                f"{page} from a client the rules refuse: expected the "
                f"server's own 404, got {status}: {body[:200]!r}"
            )

    def test_console_works_for_an_allowed_client(self):
        """The console page, the data it loads and its API answer."""
        # The handler answers its own path with a redirect to the console
        # page one level down.
        for page in (ADMIN, GLOBAL_ADMIN):
            status, _, _ = _fetch(page, ALLOWED_CLIENT)
            assert status in (200, 301, 302), (
                f"{page}: expected the handler's answer, got {status}"
            )

        status, content_type, body = _fetch(f"{ADMIN}/", ALLOWED_CLIENT)
        assert status == 200, f"{ADMIN}/: expected 200, got {status}"
        assert "text/html" in content_type.lower(), content_type
        assert "<html" in body.lower(), body[:200]

        status, content_type, _ = _fetch(f"{GLOBAL_ADMIN}/", ALLOWED_CLIENT)
        assert status == 200, f"{GLOBAL_ADMIN}/: expected 200, got {status}"
        assert "text/html" in content_type.lower(), content_type

        for page in (
            f"{ADMIN}/statistics",
            f"{ADMIN}/statistics?json",
            f"{ADMIN}/config",
            f"{ADMIN}/cache",
            f"{ADMIN}/message_history",
            f"{ADMIN}/console?json&granularity=0",
            f"{GLOBAL_ADMIN}/statistics",
            STATISTICS,
            GLOBAL_STATISTICS,
            CONSOLE,
            MESSAGES,
        ):
            status, _, body = _fetch(page, ALLOWED_CLIENT)
            assert status == 200, (
                f"{page}: expected 200 for an allowed client, got {status}: "
                f"{body[:200]!r}"
            )

        # An API route below the admin path reaches the handler: it answers
        # in JSON whatever the state of the optimizer behind it.
        status, content_type, body = _fetch(
            f"{ADMIN}/v1/daemon/health", ALLOWED_CLIENT
        )
        assert status != 403, body[:200]
        assert "application/json" in content_type.lower(), (
            f"{ADMIN}/v1/daemon/health should be answered by the handler "
            f"(JSON), got {status} {content_type!r}: {body[:200]!r}"
        )

    def test_spellings_the_server_normalizes_still_reach_the_handler(self):
        """Repeated slashes below the admin path and a query string do not
        change which handler the server and the module select."""
        for page in (
            f"{ADMIN}//statistics",
            f"{ADMIN}/./statistics",
            f"{ADMIN}/x/../statistics",
            f"{ADMIN}/statistics?a=b%2Fc&d=..%5Ce",
        ):
            status, _, body = _fetch(page, ALLOWED_CLIENT)
            assert status == 200, (
                f"{page}: expected 200 for an allowed client, got {status}: "
                f"{body[:200]!r}"
            )
            refused, _, _ = _fetch(page, REFUSED_CLIENT)
            assert refused == 403, (
                f"{page} from a client the rules refuse: expected 403, got "
                f"{refused}"
            )

    def test_regex_location_does_not_take_pages_below_an_admin_path(self):
        """The server block has a regular-expression location that answers
        418 for .js and .css; pages below the admin path stay under the
        admin path's own rule."""
        status, _, _ = _fetch("/not_a_handler.js", ALLOWED_CLIENT)
        assert status == 418, f"regex location should answer, got {status}"

        for page in (f"{ADMIN}/x.js", f"{ADMIN}/v1/x.css"):
            refused, _, _ = _fetch(page, REFUSED_CLIENT)
            assert refused == 403, (
                f"{page} from a client the rules refuse: expected 403, got "
                f"{refused}"
            )
            status, _, body = _fetch(page, ALLOWED_CLIENT)
            assert status not in (403, 418), (
                f"{page} for an allowed client should reach the handler, "
                f"got {status}: {body[:200]!r}"
            )

    def test_server_block_without_rules_serves_http_level_paths(self):
        """A handler path set in the http block is served by every server
        block: the lane's main server block carries no access rules and
        answers a client the restricted server block refuses."""
        main_port = int(os.environ["PAGESPEED_PORT"])
        for page in (f"{ADMIN}/statistics", f"{GLOBAL_ADMIN}/statistics"):
            status, _, body = _fetch(page, REFUSED_CLIENT, port=main_port)
            assert status == 200, (
                f"{page} on the server block without rules: expected 200, "
                f"got {status}: {body[:200]!r}"
            )
            refused, _, _ = _fetch(page, REFUSED_CLIENT)
            assert refused in (403, 404), (
                f"{page} on the restricted server block must not be served "
                f"to that client, got {refused}"
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
