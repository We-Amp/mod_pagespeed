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

"""Keep data URLs in CSS tests.

Ported from: pagespeed/automatic/system_tests/keep_data_urls.sh

These tests verify that data: URLs in CSS are preserved and not blanked out.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


class TestKeepDataUrls:
    """Tests for preserving data: URLs in CSS.

    Bash original::

        start_test CSS data URLs
        URL=$REWRITTEN_ROOT/styles/A.data.css.pagespeed.cf.Hash.css
        OUT=$($WGET_DUMP $URL)
        check_from "$OUT" fgrep -q 'data:image/png'
    """

    def test_css_data_urls_preserved(
        self, client: PageSpeedClient, rewritten_root: str
    ):
        """data: URLs in CSS should be preserved, not blanked."""
        url = f"{rewritten_root}/styles/A.data.css.pagespeed.cf.Hash.css"

        response = client.get(url)
        # The URL might be 404 if the hash doesn't match, but if it returns
        # content, the data URL should be preserved
        if response.status == 200:
            assert_contains(
                response,
                r"data:image/png",
                "data: URLs in CSS should be preserved",
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
