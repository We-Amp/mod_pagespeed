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

"""A lying Host header does not turn a resource fetch into a cross-site fetch.

Ported from: pagespeed/system/system_tests/cross_site_fetch.sh

A .pagespeed. resource requested with Host: www.google.com is reconstructed
from this server's own copy of the input, so the request succeeds instead of
fetching (or failing to fetch) from the named site.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_status_ok


class TestCrossSiteFetch:

    def test_lying_host_header_fetches_from_this_server(
        self, client: PageSpeedClient, example_root: str
    ):
        """Bash: Lying host headers for cross-site fetch."""
        path = f"{example_root}/styles/big.css.pagespeed.ce.8CfGBvwDhH.css"
        response = client.get(path, headers={"Host": "www.google.com"})
        require_status_ok(response, "cache-extended big.css with a lying Host header")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
