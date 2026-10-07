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

"""RespectXForwardedProto: the base URL follows the proxy's scheme.

Ported from: pagespeed/apache/system_tests/x_forwarded_proto.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestXForwardedProto:
    """Bash: Respect X-Forwarded-Proto when told to."""

    def test_base_tag_follows_x_forwarded_proto(self, client: PageSpeedClient, test_root: str):
        response = client.get(
            f"{test_root}/?PageSpeedFilters=add_base_tag",
            headers={"X-Forwarded-Proto": "https"},
        )
        require_status_ok(response, "mod_pagespeed_test/ with add_base_tag")
        assert '<base href="https://' in response.text, response.text[:400]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
