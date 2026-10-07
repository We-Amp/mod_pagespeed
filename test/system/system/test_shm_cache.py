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

"""Rewriting works with an explicitly configured shared-memory metadata cache.

Ported from: pagespeed/system/system_tests/shm_cache.sh
"""

import re

import pytest

from pagespeed_test_framework.stats import count_matching_lines


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestShmCache:
    """Bash: Using SHM metadata cache."""

    def test_images_are_rewritten_with_an_shm_metadata_cache(self, vhost_client):
        pattern = re.escape(".pagespeed.ic")
        vhost_client("shmcache.example.com").fetch_until(
            "/mod_pagespeed_example/rewrite_images.html",
            condition=lambda r: count_matching_lines(r.text, pattern) == 2,
            timeout=100.0,
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
