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

"""A PageSpeedFilters query parameter does not switch on the core filters.

Ported from: pagespeed/system/system_tests/query_params_dont_enable_core_filters.sh
(https://github.com/apache/incubator-pagespeed-ngx/issues/1190)

debug-filters.example.com: PassThrough + debug.
"""

import pytest

from pagespeed_test_framework import (
    assert_contains,
    assert_not_contains,
    require_match,
    require_status_ok,
)

HOST = "debug-filters.example.com"
_URL = "/mod_pagespeed_example/rewrite_javascript.html?PageSpeedFilters=-rewrite_css"


def _filters_from_debug_html(text: str) -> str:
    """Lines between the last 'Filters:' line and the next 'Options:' line.

    Bash: extract_filters_from_debug_html (system_test_helpers.sh).
    """
    require_match(r"(?m)^Filters:$", text, "debug 'Filters:' line")
    require_match(r"(?m)^Options:$", text, "debug 'Options:' line")
    after_filters = text.rsplit("\nFilters:\n", 1)[1]
    return after_filters.split("\nOptions:", 1)[0]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestQueryParamsDontEnableCoreFilters:

    def test_query_params_dont_turn_on_core_filters(self, vhost_client):
        """start_test query params dont turn on core filters"""
        response = require_status_ok(vhost_client(HOST).get(_URL),
                                     "rewrite_javascript.html")
        filters = _filters_from_debug_html(response.text)
        assert_contains(filters, r"(?m)^db.*Debug$")
        assert_contains(filters, r"(?m)^hw.*Flushes html$")
        assert_not_contains(filters, r"(?m)^jm.*Rewrite External Javascript$")
        assert_not_contains(filters, r"(?m)^jj.*Rewrite Inline Javascript$")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
