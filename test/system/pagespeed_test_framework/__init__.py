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

"""PageSpeed system test framework for Python.

This package provides utilities for writing system tests that verify
PageSpeed optimization behavior. It mirrors the functionality of the
bash-based system_test_helpers.sh.

Example usage::

    from pagespeed_test_framework import PageSpeedClient
    from pagespeed_test_framework.assertions import assert_contains

    def test_combine_css(client):
        response = client.fetch_until(
            "/combine_css.html?PageSpeedFilters=+combine_css",
            condition=lambda r: ".pagespeed.cc." in r.text
        )
        assert_contains(response.text, r"\\.pagespeed\\.cc\\.")
"""

from pagespeed_test_framework.client import PageSpeedClient, Response
from pagespeed_test_framework.assertions import (
    assert_contains,
    assert_not_contains,
    assert_http_status,
    assert_header_equals,
    assert_header_contains,
    assert_stat_delta,
    assert_stat_increased,
    assert_file_size,
)
from pagespeed_test_framework.stats import (
    parse_statistics,
    get_stat,
    scrape_header,
)

__all__ = [
    # Client
    "PageSpeedClient",
    "Response",
    # Assertions
    "assert_contains",
    "assert_not_contains",
    "assert_http_status",
    "assert_header_equals",
    "assert_header_contains",
    "assert_stat_delta",
    "assert_stat_increased",
    "assert_file_size",
    # Stats
    "parse_statistics",
    "get_stat",
    "scrape_header",
]
