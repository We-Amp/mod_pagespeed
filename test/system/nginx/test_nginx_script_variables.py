#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Script-variable option evaluation on the request path.

The lane enables ProcessScriptVariables and serves a page through a location
whose EnableFilters value comes from the request's own query argument
(run_nginx_tests.sh). A value the configuration rejects must leave the
request served without optimization, not failed; a valid value must reach
the rewrite pipeline.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient

# Served through the scripted location (run_nginx_tests.sh):
#     location ^~ /mod_pagespeed_test/script_variables/ {
#         pagespeed EnableFilters $arg_filters;
#     }
PAGE = "/mod_pagespeed_test/script_variables/remove_comments.html"


@pytest.mark.nginx_only
@pytest.mark.requires_module
class TestNginxScriptVariables:
    """A request-time option value the configuration rejects is survivable."""

    def test_invalid_value_served_without_optimization(
        self, client: PageSpeedClient
    ):
        """An invalid filter name answers 200 with the page untouched.

        The scripted option's value is not a valid option value for this
        request; the module must serve the page without optimization
        instead of failing the request.
        """
        response = client.get(f"{PAGE}?filters=not_a_filter")

        assert response.status == 200, (
            f"an invalid script-variable value must be answered with the "
            f"unoptimized page (200), got status {response.status}: "
            f"{response.text[:200]!r}"
        )
        assert ".pagespeed." not in response.text, (
            f"an invalid script-variable value must leave the page "
            f"unoptimized, got: {response.text[:200]!r}"
        )

    def test_valid_value_optimizes(self, client: PageSpeedClient):
        """A valid filter name reaches the rewrite pipeline (control)."""
        marker = "script-variable-marker"
        assert marker in client.get(f"{PAGE}?filters=not_a_filter").text

        response = client.fetch_until(
            f"{PAGE}?filters=remove_comments",
            lambda r: r.status == 200 and marker not in r.text,
            timeout=60.0,
        )

        assert response.status == 200
        assert marker not in response.text, (
            f"remove_comments from a script variable must strip the page's "
            f"comment, got: {response.text[:200]!r}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
