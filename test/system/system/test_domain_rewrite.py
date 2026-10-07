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

"""Domain rewriting: client-side rewriter, hyperlinks, static assets.

Ported from: pagespeed/system/system_tests/client_domain_rewrite.sh,
domain_rewrite_hyperlinks.sh and static_asset_domain_rewrite.sh

The static library URL uses the product's default /pagespeed_static/
prefix, where the bash config set /mod_pagespeed_static/.
"""

import re

import pytest

from pagespeed_test_framework import require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

PAGE = "/mod_pagespeed_test/rewrite_domains.html"
DST = re.escape("http://dst.example.com")


def _page(vhost_client, host, path=PAGE):
    response = vhost_client(host).get(path)
    require_status_ok(response, f"{host}{path}")
    return response


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestDomainRewrite:
    """Bash: ClientDomainRewrite on / RewriteHyperlinks off and on / static
    asset urls are mapped."""

    def test_client_domain_rewrite_on(self, vhost_client):
        text = _page(vhost_client, "client-domain-rewrite.example.com").text
        assert count_matching_lines(text, r"pagespeed\.clientDomainRewriterInit") == 1, text[:600]

    def test_rewrite_hyperlinks_off(self, vhost_client):
        text = _page(vhost_client, "domain-hyperlinks-off.example.com").text
        assert count_matching_lines(text, DST) == 1, text[:600]

    def test_rewrite_hyperlinks_on(self, vhost_client):
        text = _page(vhost_client, "domain-hyperlinks-on.example.com").text
        assert count_matching_lines(text, DST) == 4, text[:600]

    def test_static_asset_urls_are_mapped(self, vhost_client):
        text = _page(vhost_client, "map-static-domain.example.com",
                     "/mod_pagespeed_example/rewrite_javascript.html").text
        assert "http://static-cdn.example.com/pagespeed_static/js_defer" in text, text[:800]


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
