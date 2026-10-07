#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""pagespeed_admin pages on the default, global and alternate admin paths.

Ported from: pagespeed/apache/system_tests/pagespeed_admin.sh

The bash case greps each page for "<title>PageSpeed <Title></title>", a
banner the JSON admin no longer emits. The port keeps the bash loop --
every sub-page on every admin path must be served -- and checks the current
contract (pagespeed/system/admin_site.cc::AdminPage): the data pages are
application/json objects, the console is the HTML shell of the console app.

The bash not-found case asserted "Unknown admin page" in the response body;
the admin now answers a JSON 404 (admin_site.cc::WriteJsonError) instead of
an HTML page carrying that phrase, so the port checks the current contract:
status 404, a JSON body, Content-Type: application/json, and a Cache-Control
that still says no-store.
"""

import json

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    require_no_auth_gate,
    require_status_ok,
)

# Sentinel for the global admin path: server_config is a fixture and isn't
# resolved yet when this module-level table is built, so ADMIN_PATHS carries
# this marker and _admin_path() resolves it against
# server_config.global_admin_path inside each test.
GLOBAL_ADMIN_SENTINEL = object()
ALTERNATE_ADMIN_PATH = "/alt/admin/path"

# for admin_path in pagespeed_admin pagespeed_global_admin alt/admin/path
# (None = this lane's admin path, server_config.admin_path)
ADMIN_PATHS = [
    pytest.param(None, id="pagespeed_admin"),
    pytest.param(GLOBAL_ADMIN_SENTINEL, id="pagespeed_global_admin"),
    pytest.param(
        ALTERNATE_ADMIN_PATH,
        id="alt_admin_path",
        marks=pytest.mark.requires_fixture("admin_handlers"),
    ),
]


def _check_config_value(data, path):
    """config: the value is a non-empty string (admin_site.cc::PrintConfig)."""
    value = data["config"]
    assert isinstance(value, str) and value, (
        f"{path}: expected config to be a non-empty string, got {value!r}"
    )


def _check_caches_value(data, path):
    """cache: a non-empty list whose items include "HTTP Cache".

    admin_site.cc::PrintCaches always emits an "HTTP Cache" entry alongside
    the metadata/property/filesystem caches.
    """
    caches = data["caches"]
    assert isinstance(caches, list) and caches, (
        f"{path}: expected a non-empty caches list, got {caches!r}"
    )
    names = [entry.get("name") for entry in caches if isinstance(entry, dict)]
    assert "HTTP Cache" in names, (
        f"{path}: expected caches to include HTTP Cache, got names {names}"
    )


def _check_messages_value(data, path):
    """message_history: messages is a list (admin_site.cc::MessageHistoryHandler)."""
    assert isinstance(data["messages"], list), (
        f"{path}: expected messages to be a list, got {data['messages']!r}"
    )


# (sub-page, top-level JSON keys it must carry, optional per-entry value
# check). statistics is a flat map of statistic names, so it only has to be
# a non-empty object.
JSON_SUBPAGES = [
    ("statistics", [], None),
    ("config", ["config"], _check_config_value),
    ("histograms", ["histograms"], None),
    ("cache", ["caches"], _check_caches_value),
    ("message_history", ["messages"], _check_messages_value),
]


def _admin_path(server_config, admin_path):
    if admin_path is None:
        return server_config.admin_path
    if admin_path is GLOBAL_ADMIN_SENTINEL:
        return server_config.global_admin_path
    return admin_path


class TestAdminPages:
    """Every admin sub-page is served on every admin path."""

    @pytest.mark.parametrize("admin_path", ADMIN_PATHS)
    @pytest.mark.parametrize("subpage,keys,check", JSON_SUBPAGES)
    def test_json_subpage(
        self, client: PageSpeedClient, server_config, admin_path, subpage, keys, check
    ):
        path = f"{_admin_path(server_config, admin_path)}/{subpage}"
        response = client.get(path)
        require_no_auth_gate(response, f"Admin page {path}")
        require_status_ok(response, f"Admin page {path}")
        content_type = response.header("Content-Type").lower()
        assert content_type.startswith("application/json"), (
            f"{path}: expected application/json, got {content_type!r}"
        )
        data = json.loads(response.text)
        assert isinstance(data, dict) and data, f"{path}: expected a non-empty JSON object"
        missing = [key for key in keys if key not in data]
        assert not missing, f"{path}: JSON lacks {missing}; keys: {sorted(data)}"
        if check is not None:
            check(data, path)

    @pytest.mark.parametrize("admin_path", ADMIN_PATHS)
    def test_admin_root_responds(self, client: PageSpeedClient, server_config, admin_path):
        """GET <admin>/ answers 200, 301 or 302 (the IIS test's contract)."""
        path = f"{_admin_path(server_config, admin_path)}/"
        response = client.get(path)
        assert response.status in (200, 301, 302), (
            f"{path}: expected 200, 301 or 302, got {response.status}"
        )

    @pytest.mark.parametrize("admin_path", ADMIN_PATHS)
    def test_console_subpage(self, client: PageSpeedClient, server_config, admin_path):
        path = f"{_admin_path(server_config, admin_path)}/console"
        response = client.get(path)
        require_no_auth_gate(response, f"Admin page {path}")
        require_status_ok(response, f"Admin page {path}")
        content_type = response.header("Content-Type").lower()
        assert content_type.startswith("text/html"), (
            f"{path}: expected text/html, got {content_type!r}"
        )
        assert len(response.text) > 100, f"{path}: the console shell is empty"


class TestAdminNotFoundQuoting:
    """pagespeed_admin quoting on not-found page.

    Bash asserted 'Unknown admin page' in the response text (a
    fgrep -q "<title>PageSpeed ...` peer of the pagespeed_admin.sh not-found
    case); the admin now answers a JSON 404
    (pagespeed/system/admin_site.cc::WriteJsonError) instead, so the port
    checks that current contract as well as the quoting/stripping behavior
    the bash case cared about.
    """

    def test_not_found_leaf_is_a_json_404(
        self, client: PageSpeedClient, server_config
    ):
        response = client.get(f"{server_config.admin_path}/a/nosuchpage")
        assert response.status == 404, (
            f"expected 404 for an unknown admin leaf, got {response.status}"
        )
        content_type = response.header("Content-Type").lower()
        assert content_type.startswith("application/json"), (
            f"expected application/json, got {content_type!r}"
        )
        cache_control = response.header("Cache-Control").lower()
        assert "no-store" in cache_control, (
            f"expected Cache-Control to contain no-store, got {cache_control!r}"
        )
        data = json.loads(response.text)
        assert data.get("success") is False, (
            f"expected a JSON error body, got: {data}"
        )

    def test_path_is_quoted_on_the_not_found_page(
        self, client: PageSpeedClient, server_config
    ):
        response = client.get(f"{server_config.admin_path}/a/<boo>")
        assert "<boo>" not in response.text, "raw <boo> reflected on the not-found page"
        if server_config.is_iis:
            # IIS's http.sys refuses a raw '<' in the request path before the
            # module sees it, so the escaped path cannot appear on a not-found
            # page there; the request is rejected outright (issue #1046).
            assert response.status == 400, (
                f"expected http.sys to reject the raw '<' with 400; "
                f"got {response.status}: {response.text[:300]}"
            )
        else:
            assert "%3Cboo%3E" in response.text, (
                f"expected the escaped path %3Cboo%3E; body: {response.text[:500]}"
            )
            # Bash: fgrep -q "Unknown admin page" (pagespeed_admin.sh:40); the
            # admin now answers an unknown leaf with a JSON 404.
            assert response.status == 404 and response.header(
                "Content-Type").lower().startswith("application/json"), (
                f"expected the JSON 404 for an unknown admin leaf; got "
                f"{response.status} {response.header('Content-Type')!r}"
            )

    def test_query_is_stripped_from_the_not_found_page(
        self, client: PageSpeedClient, server_config
    ):
        response = client.get(f"{server_config.admin_path}/a?<boo>")
        assert "boo" not in response.text, (
            f"query text reflected on the not-found page; body: {response.text[:500]}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
