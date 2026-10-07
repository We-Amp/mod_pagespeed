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

"""CustomFetchHeader is sent on resource fetches.

Ported from: pagespeed/apache/system_tests/custom_fetch_headers.sh

/mod_pagespeed_log_request_headers.js is an Apache handler that echoes
the request headers it received, so the headers the module sent on its
own fetch show up in the rewritten script.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines


@pytest.mark.apache_only  # mod_pagespeed_log_request_headers is an Apache handler (bash: apache/)
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestCustomFetchHeaders:
    """Bash: Send custom fetch headers on resource re-fetches / subfetches."""

    def test_custom_headers_on_resource_refetch(self, client: PageSpeedClient):
        url = "/mod_pagespeed_log_request_headers.js.pagespeed.jm.0.js"
        response = client.get(url)
        require_status_ok(response, url)
        assert "header=value" in response.text, response.text[:400]
        assert "x-other=False" in response.text, response.text[:400]

    def test_custom_headers_on_subresource_fetch(self, client: PageSpeedClient, test_root: str):
        page = client.fetch_until(
            f"{test_root}/custom_fetch_headers.html?PageSpeedFilters=inline_javascript",
            condition=lambda r: count_matching_lines(r.text, r"header=value") == 1,
            timeout=100.0,
        )
        assert "x-other=False" in page.text, page.text[:400]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
