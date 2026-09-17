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

"""Crash regression tests for IIS PageSpeed module.

These tests verify that HTML rewriting does not cause heap corruption or
crashes in the IIS worker process. The original bug was caused by a memory
allocator mismatch: IisStreamingFetch used IIS pool memory via a custom
allocator but freed it with delete[], causing STATUS_HEAP_CORRUPTION.

The fix removes the custom allocator so IisStreamingFetch uses its built-in
new[]/delete[] allocator which correctly pairs allocations with frees.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
)


@pytest.mark.html_rewrite
class TestHTMLRewriteNoCrash:
    """Tests that HTML rewriting does not crash the worker process."""

    def test_html_rewrite_single_request(
        self, client: PageSpeedClient, example_root: str
    ):
        """Single HTML rewrite request should complete without crash."""
        response = client.get(
            f"{example_root}/combine_css.html?PageSpeedFilters=collapse_whitespace"
        )
        assert_http_status(response, 200)

    def test_html_rewrite_repeated_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """Repeated HTML rewrite requests should not crash the worker.

        This test was added to catch the heap corruption bug where
        IisStreamingFetch used IIS pool memory with delete[], causing
        STATUS_HEAP_CORRUPTION (0xc0000374) after several requests.
        """
        for i in range(10):
            response = client.get(
                f"{example_root}/combine_css.html?PageSpeedFilters=collapse_whitespace"
            )
            assert_http_status(response, 200), f"Request {i+1} failed"

    def test_html_rewrite_multiple_filters(
        self, client: PageSpeedClient, example_root: str
    ):
        """Multiple filters should work without crash."""
        filters = "collapse_whitespace,remove_comments"
        for i in range(5):
            response = client.get(
                f"{example_root}/combine_css.html?PageSpeedFilters={filters}"
            )
            assert_http_status(response, 200), f"Request {i+1} failed"

    def test_html_rewrite_different_pages(
        self, client: PageSpeedClient, example_root: str
    ):
        """Rewriting different pages should not crash."""
        pages = [
            "combine_css.html",
            "rewrite_css.html",
            "combine_javascript.html",
        ]
        for page in pages:
            response = client.get(
                f"{example_root}/{page}?PageSpeedFilters=collapse_whitespace"
            )
            assert_http_status(response, 200), f"Failed on {page}"


@pytest.mark.html_rewrite
@pytest.mark.slow
class TestHTMLRewriteStress:
    """Stress tests for HTML rewriting stability."""

    def test_stress_50_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """50 consecutive requests should complete without crash.

        This is the main regression test for the heap corruption bug.
        If this test passes, the memory allocator fix is working.
        """
        for i in range(50):
            response = client.get(
                f"{example_root}/?PageSpeedFilters=collapse_whitespace"
            )
            assert_http_status(response, 200), f"Request {i+1} failed"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
