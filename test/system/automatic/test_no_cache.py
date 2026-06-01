#!/usr/bin/env python3
# Copyright 2024 Google LLC
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

"""No-cache resource handling tests.

Ported from: pagespeed/automatic/system_tests/no_cache.sh

These tests verify that resources with Cache-Control: no-cache are handled
correctly.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_header_contains,
)


class TestNoCacheResources:
    """Tests for handling resources with Cache-Control: no-cache.

    Bash original:
        echo Test that we can rewrite resources that are served with
        echo Cache-Control: no-cache with on-the-fly filters.
        test_filter extend_cache with no-cache js origin
    """

    def test_extend_cache_preserves_no_cache(
        self, client: PageSpeedClient, test_root: str
    ):
        """extend_cache should preserve no-cache on rewritten resources.

        Bash original:
            URL="$REWRITTEN_TEST_ROOT/no_cache/hello.js.pagespeed.ce.0.js"
            check fgrep -q "'Hello'" $WGET_DIR/hello.js.pagespeed.ce.0.js
            check fgrep -q "no-cache" $WGET_OUTPUT
        """
        url = f"{test_root}/no_cache/hello.js.pagespeed.ce.0.js"

        response = client.get(url)
        assert_http_status(response, 200)

        # Verify the JS content is correct
        assert_contains(response, r"'Hello'")

        # Verify no-cache header is preserved
        cache_control = response.header("Cache-Control")
        assert "no-cache" in cache_control.lower(), \
            f"Expected no-cache in Cache-Control, got: {cache_control}"

    def test_rewrite_javascript_preserves_no_cache(
        self, client: PageSpeedClient, test_root: str
    ):
        """rewrite_javascript should preserve no-cache on minified resources.

        Bash original:
            test_filter rewrite_javascript with no-cache js origin
            URL="$REWRITTEN_TEST_ROOT/no_cache/hello.js.pagespeed.jm.0.js"
            check fgrep -q "'Hello'" $WGET_DIR/hello.js.pagespeed.jm.0.js
            check fgrep -q "no-cache" $WGET_OUTPUT
        """
        url = f"{test_root}/no_cache/hello.js.pagespeed.jm.0.js"

        response = client.get(url)
        assert_http_status(response, 200)

        # Verify the JS content is correct (minified but functional)
        assert_contains(response, r"'Hello'")

        # Verify no-cache header is preserved
        cache_control = response.header("Cache-Control")
        assert "no-cache" in cache_control.lower(), \
            f"Expected no-cache in Cache-Control, got: {cache_control}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
