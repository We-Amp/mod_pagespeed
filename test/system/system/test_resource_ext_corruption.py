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

"""Noise after a .pagespeed. URL's extension is ignored, not propagated.

Ported from: pagespeed/system/system_tests/resource_ext_corruption.sh

Fetching <rewritten resource>broken still succeeds, and the page's
rewritten URL for that resource never picks up the "broken" suffix.
"""

import re
from urllib.parse import urljoin, urlparse

import pytest

from pagespeed_test_framework import PageSpeedClient, require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

REQUISITE = re.compile(r'<(?:link|img|script)\b[^>]*?\b(?:href|src)="([^"]+)"')


def fetch_page_with_requisites(client: PageSpeedClient, page_path: str):
    """Bash: run_wget_with_args $URL (wget -p: the page and its requisites)."""
    page = client.get(page_path)
    require_status_ok(page, f"page {page_path}")
    for ref in REQUISITE.findall(page.text):
        parsed = urlparse(urljoin(f"http://base.invalid{page_path}", ref))
        path = f"{parsed.path}?{parsed.query}" if parsed.query else parsed.path
        require_status_ok(client.get(path), f"page requisite {ref}")
    return page


def check_resource_ext_corruption(
    client: PageSpeedClient, example_root: str, page_path: str, resource: str
) -> None:
    """Bash: test_resource_ext_corruption $URL $RESOURCE."""
    page = client.get(page_path)
    require_status_ok(page, f"page {page_path}")
    assert resource.lower() in page.text.lower(), (
        f"{resource} is not referenced by {page_path}; the test is broken"
    )
    broken = client.get(f"{example_root}/{resource}broken")
    require_status_ok(broken, f"{resource}broken (the noise should be ignored)")
    page = client.get(page_path)
    require_status_ok(page, f"page {page_path}")
    assert "broken" not in page.text, (
        f"rewritten URL picked up the extension noise: {page.text[:500]}"
    )


class TestResourceExtCorruption:

    def test_combined_css_url_ignores_extension_noise(
        self, client: PageSpeedClient, example_root: str
    ):
        """Bash: test_filter combine_css combines 4 CSS files into 1."""
        page_path = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        client.fetch_until(
            page_path,
            lambda r: count_matching_lines(r.text, "text/css") == 1,
            detail_fn=lambda r: f"lines with text/css={count_matching_lines(r.text, 'text/css')}",
        )
        page = fetch_page_with_requisites(client, page_path)
        resource = require_match(
            r"(styles/yellow\.css\+blue\.css\+big\.css\+bold\.css\.pagespeed\.cc\.[^.\"]+\.css)",
            page, "combined CSS URL",
        ).group(1)
        check_resource_ext_corruption(client, example_root, page_path, resource)

    def test_cache_extended_image_url_ignores_extension_noise(
        self, client: PageSpeedClient, example_root: str
    ):
        """Bash: test_filter extend_cache rewrites an image tag."""
        page_path = f"{example_root}/extend_cache.html?PageSpeedFilters=extend_cache"
        client.fetch_until(
            page_path,
            lambda r: count_matching_lines(r.text, r"src.*Puzzle\.jpg\.pagespeed\.ce\.") == 1,
            detail_fn=lambda r: "cache-extended Puzzle.jpg not in the page yet",
        )
        page = fetch_page_with_requisites(client, page_path)
        resource = require_match(
            r"(images/Puzzle\.jpg\.pagespeed\.ce\.[^.\"]+\.jpg)", page,
            "cache-extended image URL",
        ).group(1)
        check_resource_ext_corruption(client, example_root, page_path, resource)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
