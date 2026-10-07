#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""AddResourceHeader applies to .pagespeed. resources.

Ported from: pagespeed/system/system_tests/add_resource_headers.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestAddResourceHeaders:
    """AddResourceHeaders works for pagespeed resources."""

    def test_add_resource_header_on_cache_extended_resource(
        self, client: PageSpeedClient, test_root: str
    ):
        url = f"{test_root}/compressed/hello_js.custom_ext.pagespeed.ce.HdziXmtLIV.txt"
        response = client.fetch_until(
            url,
            condition=lambda r: "text/javascript" in r.header("Content-Type"),
            timeout=100.0,
        )
        assert response.header("X-Foo") == "Bar", (
            f"X-Foo: {response.header('X-Foo')!r} on {url}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
