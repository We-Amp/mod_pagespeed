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

"""In-place optimization answers the first request with LoadFromFile.

Ported from: pagespeed/system/system_tests/instant_ipro.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestInstantIpro:
    """Bash: instant ipro with InPlaceWaitForOptimized / RewriteDeadline and LoadFromFile."""

    @pytest.mark.parametrize("directory", ["wait", "deadline"])
    def test_first_request_is_optimized(
        self, client: PageSpeedClient, test_root: str, directory
    ):
        url = f"{test_root}/ipro/instant/{directory}/purple.css"
        response = client.get(url)
        require_status_ok(response, url)
        assert "body{background:#9370db}" in response.text, (
            f"{url}: first response not optimized: {response.text[:200]!r}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
