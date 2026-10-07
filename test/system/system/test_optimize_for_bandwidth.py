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

"""RewriteLevel OptimizeForBandwidth optimizes in place and keeps URLs.

Ported from: pagespeed/system/system_tests/optimize_for_bandwidth.sh
"""

import pytest

from pagespeed_test_framework import require_status_ok

HOST = "optimizeforbandwidth.example.com"
BLOCKING = {"X-PSA-Blocking-Rewrite": "psatest"}
BLUE = ".blue{foreground-color:blue}body{background:url(arrow.png)}"
YELLOW_STYLE = "<style>.yellow{background-color:#ff0}</style>"


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestOptimizeForBandwidth:
    """Bash: OptimizeForBandwidth."""

    @pytest.mark.parametrize(
        "page,expected",
        [
            pytest.param("rewrite_css.html",
                         [BLUE, '<link rel="stylesheet" type="text/css" href="yellow.css">'],
                         id="root"),
            pytest.param("inline_css/rewrite_css.html", [BLUE, YELLOW_STYLE], id="inline-css"),
            pytest.param("css_urls/rewrite_css.html",
                         [BLUE, '<link rel="stylesheet" type="text/css" href="A.yellow.css.pagespeed'],
                         id="css-urls"),
            pytest.param("image_urls/rewrite_image.html", ['<img src="xarrow.png.pagespeed.'],
                         id="image-urls"),
            pytest.param("core_filters/rewrite_css.html",
                         [".blue{foreground-color:blue}body{background:url(xarrow.png.pagespeed.",
                          YELLOW_STYLE], id="core-filters"),
        ],
    )
    def test_optimize_for_bandwidth(self, vhost_client, page, expected):
        path = f"/mod_pagespeed_test/optimize_for_bandwidth/{page}"
        response = vhost_client(HOST).get(path, headers=BLOCKING)
        require_status_ok(response, path)
        for text in expected:
            assert text in response.text, f"{page}: missing {text!r} in {response.text[:600]!r}"

    @pytest.mark.parametrize(
        "page,kept",
        [
            pytest.param("combine_css.html?PageSpeedFilters=+combine_css", "bold.css", id="css"),
            pytest.param("combine_javascript.html?PageSpeedFilters=+combine_javascript",
                         "combine_javascript2", id="js"),
        ],
    )
    def test_combining_keeps_every_url(self, vhost_client, page, kept):
        path = f"/mod_pagespeed_example/{page}"
        response = vhost_client(HOST).get(path, headers=BLOCKING)
        require_status_ok(response, path)
        assert kept in response.text, f"{page}: {kept!r} was eaten: {response.text[:600]!r}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
