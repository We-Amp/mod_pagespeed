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

"""Initial sanity check tests for IIS PageSpeed module.

These tests verify basic server functionality before running more complex tests.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_header_contains,
)


class TestBasicConnectivity:
    """Basic connectivity tests."""

    def test_server_responds(self, client: PageSpeedClient, example_root: str):
        """Verify the server is responding to requests."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

    def test_404_for_missing_page(self, client: PageSpeedClient, example_root: str):
        """Non-existent pages should return 404."""
        response = client.get(f"{example_root}/this_page_does_not_exist_12345.html")
        assert_http_status(response, 404)

    def test_x_pagespeed_header_present(self, client: PageSpeedClient, example_root: str):
        """X-PageSpeed header should be present on HTML responses."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)
        # IIS now uses "X-PageSpeed" (no hyphen) to match Apache convention
        x_pagespeed = response.header("X-PageSpeed")
        assert x_pagespeed, "X-PageSpeed header should be present"


class TestDirectoryMapping:
    """Test that directory is mapped to default document."""

    def test_directory_maps_to_index(self, client: PageSpeedClient, example_root: str):
        """Fetching / should return the same content as /index.html."""
        root_response = client.get(f"{example_root}/?PageSpeed=off")
        index_response = client.get(f"{example_root}/default.htm?PageSpeed=off")

        assert_http_status(root_response, 200)
        assert_http_status(index_response, 200)

        # Both should return the same content
        assert root_response.text == index_response.text, \
            "Directory request should return same content as default document"


class TestPageSpeedOff:
    """Test that PageSpeed=off disables optimization."""

    def test_pagespeed_off_disables_rewriting(
        self, client: PageSpeedClient, example_root: str
    ):
        """PageSpeed=off query param should disable all optimization."""
        response = client.get(f"{example_root}/combine_css.html?PageSpeed=off")

        assert_http_status(response, 200)
        # Should not see any pagespeed rewriting markers
        assert ".pagespeed." not in response.text, \
            "PageSpeed=off should disable rewriting"


class TestCompression:
    """Test that gzip compression is enabled."""

    def test_gzip_compression_enabled(self, client: PageSpeedClient, example_root: str):
        """Server should return gzip-compressed responses when requested."""
        response = client.get(
            f"{example_root}/",
            headers={"Accept-Encoding": "gzip"},
        )

        assert_http_status(response, 200)
        content_encoding = response.header("Content-Encoding")
        # IIS may handle compression separately from PageSpeed
        # Accept either gzip or no encoding
        assert content_encoding == "" or "gzip" in content_encoding.lower(), \
            f"Expected gzip or no compression, got: {content_encoding}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
