#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""nginx location-inheritance tests.

``pagespeed`` directives set on a location must also apply inside the
location's nested locations and its ``if`` blocks when those carry no
directives of their own: nginx's configuration merge walks nested blocks
with the enclosing location as their parent, and the module now inherits
the parent's settings there instead of falling back to the server's.

The lane's nginx.conf provisions the fixture (run_nginx_tests.sh,
generate_nginx_config): an enclosing location with ``pagespeed off`` at
/mod_pagespeed_test/location_inherit/, a nested regex location covering
its nested/ subtree and an ``if`` block that matches ?ps_if=1, neither
with pagespeed directives. Everything under the enclosing location must
come back unoptimized and without an X-Page-Speed header, while the
server rewrites pages elsewhere.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_not_contains,
    require_status_ok,
)


def _assert_passes_through(response, what: str) -> None:
    """The response must be a pass-through: no module header, no rewriting."""
    page_speed = response.header("X-Page-Speed")
    assert not page_speed, (
        f"{what} must not carry an X-Page-Speed header, got: {page_speed!r}")
    assert_not_contains(response.text, ".pagespeed.",
                        f"{what} must not be rewritten")


@pytest.mark.nginx_only
class TestLocationInherit:
    # Plain gets: with the location settings off the module declines the
    # request early, so there is nothing asynchronous to wait for.
    #
    # The first and last tests are controls: the first proves the fixture
    # itself is off (so the inheritance cases cannot pass vacuously), the
    # last proves the server optimizes outside the location.

    def test_enclosing_location_is_off(self, client: PageSpeedClient,
                                       test_root: str):
        """Control: the enclosing location itself runs pagespeed off."""
        response = require_status_ok(
            client.get(f"{test_root}/location_inherit/page.html"),
            "enclosing location")
        _assert_passes_through(response, "the enclosing location")

    def test_nested_location_inherits(self, client: PageSpeedClient,
                                      test_root: str):
        """A nested location without directives inherits the enclosing off."""
        response = require_status_ok(
            client.get(f"{test_root}/location_inherit/nested/page.html"),
            "nested location")
        _assert_passes_through(response, "the nested location")

    def test_if_block_inherits(self, client: PageSpeedClient,
                               test_root: str):
        """An if block without directives inherits the enclosing off."""
        response = require_status_ok(
            client.get(f"{test_root}/location_inherit/page.html?ps_if=1"),
            "if block")
        _assert_passes_through(response, "the if block")

    def test_server_optimizes_elsewhere(self, client: PageSpeedClient,
                                        example_root: str):
        """Control: outside the location the server still optimizes."""
        # Rewriting a resource URL needs the resource in the cache, so poll
        # instead of trusting the first fetch (earlier tests may or may not
        # have warmed it).
        response = client.fetch_until_contains(
            f"{example_root}/index.html", pattern=r"\.pagespeed\.",
            timeout=30.0)
        require_status_ok(response, "server control page")
        assert response.header("X-Page-Speed"), (
            "the server must answer with an X-Page-Speed header outside "
            "the location")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
