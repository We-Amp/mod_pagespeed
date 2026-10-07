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

"""JSON keeps its content type through in-place optimization.

Ported from: pagespeed/system/system_tests/json_content_type.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines


def _assert_json_type(response, what: str) -> None:
    require_status_ok(response, what)
    assert response.header("Content-Type").startswith("application/json"), (
        f"{what}: Content-Type {response.header('Content-Type')!r}"
    )


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestJsonContentType:
    """Bash: json keeps its content type (json_content_type.sh:15-28).

    The module does not treat JSON as heuristically cacheable, so the lane
    gives example.json an explicit Cache-Control lifetime, letting in-place
    minification happen the way the bash suite's origin let it happen.
    """

    def test_json_keeps_its_content_type(self, client: PageSpeedClient, test_root: str):
        url = f"{test_root}/example.json"
        _assert_json_type(client.get(f"{url}?PageSpeed=off"), "PageSpeed=off")
        _assert_json_type(client.get(url), "first load")
        optimized = client.fetch_until(
            url,
            condition=lambda r: count_matching_lines(r.text, r".title.:.example.json") == 1,
            timeout=100.0,
            detail_fn=lambda r: f"body={r.text[:120]!r}",
        )
        _assert_json_type(optimized, "in-place optimized")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
