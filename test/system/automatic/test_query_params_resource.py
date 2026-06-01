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

"""Query params and headers in resource flow tests.

Ported from: pagespeed/automatic/system_tests/query_params_in_resource_flow.sh

These tests verify that query params and headers can control filter behavior
for resources.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    assert_http_status,
)


class TestQueryParamsInResourceFlow:
    """Tests for query params and headers affecting resource rewriting.

    Bash original::

        start_test Query params and headers are recognized in resource flow.
        URL=$REWRITTEN_ROOT/styles/W.rewrite_css_images.css.pagespeed.cf.Hash.css
        echo "Image gets rewritten by default."
        fetch_until $URL 'fgrep -c BikeCrashIcn.png.pagespeed.ic' 1
    """

    def test_image_rewritten_by_default(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """Images in CSS should be rewritten by default."""
        url = f"{rewritten_root}/styles/W.rewrite_css_images.css.pagespeed.cf.Hash.css"

        response = client.fetch_until_count(
            url,
            pattern=r"BikeCrashIcn\.png\.pagespeed\.ic",
            expected_count=1,
            timeout=30.0,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

    @pytest.mark.not_envoy(reason="Envoy does not apply PageSpeedFilters header to .pagespeed. resource requests")
    def test_headers_can_disable_rewriting(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """Headers can disable image rewriting.

        Uses X-PSA-Blocking-Rewrite to ensure the resource is fully processed
        before checking that filter disabling headers took effect.

        Bash original::

            echo "Image doesn't get rewritten when we turn it off with headers."
            OUT=$($WGET_DUMP --header="X-PSA-Blocking-Rewrite:psatest" \
              --header="PageSpeedFilters:-convert_png_to_jpeg, -recompress_png" $URL)
            check_not_from "$OUT" fgrep -q "BikeCrashIcn.png.pagespeed.ic"
        """
        url = f"{rewritten_root}/styles/W.rewrite_css_images.css.pagespeed.cf.Hash.css"

        response = client.get(
            url,
            headers={
                "X-PSA-Blocking-Rewrite": "psatest",
                "PageSpeedFilters": "-convert_png_to_jpeg, -recompress_png",
            },
        )
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"BikeCrashIcn\.png\.pagespeed\.ic",
            "Image should not be rewritten when disabled by headers",
        )

    def test_query_params_can_disable_rewriting(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """Query params can disable image rewriting.

        Bash original::

            echo "Image doesn't get rewritten when we turn it off with query params."
            OUT=$($WGET_DUMP --header="X-PSA-Blocking-Rewrite:psatest" \
              $URL?PageSpeedFilters=-convert_png_to_jpeg,-recompress_png)
            check_not_from "$OUT" fgrep -q "BikeCrashIcn.png.pagespeed.ic"
        """
        url = (
            f"{rewritten_root}/styles/W.rewrite_css_images.css.pagespeed.cf.Hash.css"
            "?PageSpeedFilters=-convert_png_to_jpeg,-recompress_png"
        )

        response = client.get(
            url,
            headers={"X-PSA-Blocking-Rewrite": "psatest"},
        )
        assert_http_status(response, 200)

        assert_not_contains(
            response,
            r"BikeCrashIcn\.png\.pagespeed\.ic",
            "Image should not be rewritten when disabled by query params",
        )


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
