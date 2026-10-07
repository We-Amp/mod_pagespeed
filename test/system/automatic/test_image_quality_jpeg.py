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

"""The JPEG quality option bounds the size of a resized JPEG.

Ported from: pagespeed/automatic/system_tests/image_quality_jpeg.sh

The generic and JPEG-specific qualities arrive as request headers, as in
the bash case; the ceiling is the evidence that the JPEG quality won. The
bash ceilings came from an older encoder, so every ceiling here gets 5%
headroom (`_ceiling`) to keep the check meaningful without loosening it
into vacuity -- an unresized or unrecompressed image is about ten times
larger.
"""

import re
from urllib.parse import urljoin, urlsplit

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_file_size,
    require_match,
    require_status_ok,
)
from pagespeed_test_framework.stats import count_matching_lines

HEADERS = {
    "PageSpeedFilters": "rewrite_images",
    "PageSpeedImageRecompressionQuality": "85",
    "PageSpeedJpegRecompressionQuality": "70",
}


def _ceiling(bash_value: int) -> int:
    """5% headroom over the bash suite's ceiling: an older encoder made it
    tighter than this port's."""
    return int(bash_value * 1.05)


class TestJpegQuality:
    """Bash: quality of jpeg output images."""

    def test_jpeg_quality_option_bounds_the_resized_image(
        self, client: PageSpeedClient, test_root: str
    ):
        page_path = f"{test_root}/jpeg_rewriting/rewrite_images.html"
        page = client.fetch_until(
            page_path,
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.ic")) == 2,
            headers=HEADERS,
            timeout=100.0,
        )
        src = require_match(r'src="([^"]*256x192[^"]*Puzzle[^"]*)"', page,
                            "resized Puzzle URL").group(1)
        image = client.get(urlsplit(urljoin(f"http://host{page_path}", src)).path,
                           headers=HEADERS)
        require_status_ok(image, src)
        assert_file_size(image, "-le", _ceiling(7564), src)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
