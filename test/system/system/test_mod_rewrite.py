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

"""A request rewritten by mod_rewrite is optimized against its own URL.

Ported from: pagespeed/apache/system_tests/mod_rewrite.sh

redirect/php/ is rewritten by the directory's .htaccess to menu_php.html,
whose trim_urls pass must keep both absolute links intact.
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines


@pytest.mark.apache_only  # mod_rewrite is Apache's mechanism (bash: apache/)
@pytest.mark.requires_fixture("debug_conf_dirs")  # depends on redirect/.htaccess under mod_pagespeed_test/
class TestModRewrite:
    """Bash: mod_rewrite."""

    def test_rewritten_request_keeps_its_absolute_links(
        self, client: PageSpeedClient, test_root: str
    ):
        response = client.get(f"{test_root}/redirect/php/")
        require_status_ok(response, "redirect/php/")
        count = count_matching_lines(response.text, re.escape('href="/mod_pagespeed_test/'))
        assert count == 2, f"{count} absolute links, expected 2: {response.text[:600]!r}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
