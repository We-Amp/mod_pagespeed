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

"""A JavaScript origin served gzipped is decoded, minified and inlined.

Ported from: pagespeed/automatic/system_tests/rewrite_compressed_js.sh

compressed/hello_js.custom_ext is stored gzipped and served with
Content-Encoding: gzip (the compressed/ directory block); the page
inlines and minifies it only if the fetch is decoded.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestRewriteCompressedJs:
    """Bash: rewrite_javascript,inline_javascript with gzipped js origin."""

    def test_gzipped_js_origin_is_inlined_and_minified(
        self, client: PageSpeedClient, test_root: str
    ):
        url = (f"{test_root}/rewrite_compressed_js.html"
               "?PageSpeedFilters=rewrite_javascript,inline_javascript")
        client.fetch_until_count(url, pattern=r"Hello'", expected_count=1, timeout=100.0)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
