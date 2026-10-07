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

"""ShardDomain configured for one directory.

Ported from: pagespeed/system/system_tests/shard_domain.sh
"""

import re

import pytest

from pagespeed_test_framework import PageSpeedClient
from pagespeed_test_framework.stats import count_matching_lines


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestShardDomain:
    """Bash: ShardDomain directive in per-directory config."""

    def test_shard_domain_in_directory_config(self, client: PageSpeedClient, test_root: str):
        page = client.fetch_until(
            f"{test_root}/shard/shard.html",
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.ce")) == 4,
            timeout=100.0,
        )
        for shard in ("shard1", "shard2"):
            count = count_matching_lines(page.text, 'href="http://' + shard)
            assert count == 2, f"{shard}: {count} links, expected 2: {page.text[:500]!r}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
