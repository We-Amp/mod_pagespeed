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

"""MaxCacheableContentLength with IPRO, JavaScript rewriting and LoadFromFile.

Ported from: pagespeed/system/system_tests/ipro_max_cachable.sh and
pagespeed/system/system_tests/max_cachable.sh
"""

import pytest

from pagespeed_test_framework import require_status_ok


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestMaxCacheableContentLength:
    """Bash: max cacheable content length with ipro / Maximum length of cacheable
    response content / LoadFromFile with length limits."""

    def test_ipro_fetch_over_the_limit_succeeds(self, vhost_client):
        url = "/mod_pagespeed_example/images/BikeCrashIcn.png"
        response = vhost_client("max-cacheable-content-length.example.com").get(url)
        require_status_ok(response, url)

    def test_only_the_small_script_is_rewritten(self, vhost_client):
        url = ("/mod_pagespeed_test/max_cacheable_content_length/"
               "test_max_cacheable_content_length.html")
        response = vhost_client("max-cacheable-content-length.example.com").get(
            url, headers={"X-PSA-Blocking-Rewrite": "psatest"}
        )
        require_status_ok(response, url)
        text = response.text.lower()
        assert "small.js.pagespeed." in text, response.text[:400]
        assert "large.js.pagespeed." not in text, response.text[:400]

    def test_load_from_file_respects_the_limit(self, vhost_client):
        lff = vhost_client("lff-large-files.example.com")
        for leaf in ("bold.css", "big.css"):
            require_status_ok(lff.get(f"/mod_pagespeed_example/styles/{leaf}"), f"lff {leaf}")
        no_fallback = vhost_client("lff-large-files-no-fallback.example.com")
        require_status_ok(no_fallback.get("/bold.css"), "no-fallback bold.css")
        big = no_fallback.get("/big.css")
        assert big.status >= 400, f"no-fallback big.css was served: HTTP {big.status}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
