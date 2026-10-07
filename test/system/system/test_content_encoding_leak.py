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

"""A nonsense Content-Encoding from the origin is passed through.

Ported from: pagespeed/apache/system_tests/content_encoding_leak.sh

The vhost replays a recorded response (a read-only slurp directory)
whose Content-Encoding is "nonsense". The bash case guarded a leak that
only showed at process exit; it asserts the pass-through, as here.
"""

import pytest


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestContentEncodingLeak:
    """Bash: Exercising codepath of bad content-encoding."""

    def test_nonsense_content_encoding_is_passed_through(self, vhost_client):
        response = vhost_client("content-encoding.example.com").get("/")
        assert "nonsense" in response.header_values("Content-Encoding"), (
            f"HTTP {response.status}: {response.raw_headers}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
