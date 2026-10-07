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

"""Directory options apply when a resource is rewritten on request.

Ported from: pagespeed/system/system_tests/respect_custom_options.sh

The same image is served from two directories; custom_options/ disables
convert_jpeg_to_progressive (its .htaccess), so its rewrite is larger.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestRespectCustomOptions:
    """Bash: Respect custom options on resources."""

    def test_directory_options_apply_to_resource_rewrites(
        self, client: PageSpeedClient, example_root: str, test_root: str
    ):
        for url, limit in (
            (f"{example_root}/images/xPuzzle.jpg.pagespeed.ic.fakehash.jpg", 98276),
            (f"{test_root}/custom_options/xPuzzle.jpg.pagespeed.ic.fakehash.jpg", 102902),
        ):
            client.fetch_until(
                url,
                condition=lambda r, n=limit: r.status == 200 and len(r.body) <= n,
                timeout=100.0,
                detail_fn=lambda r, n=limit: f"status={r.status} bytes={len(r.body)} limit={n}",
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
