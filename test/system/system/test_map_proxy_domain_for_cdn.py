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

"""MapProxyDomain for a CDN: cdn -> proxy (PageSpeed) -> origin.

Ported from: pagespeed/apache/system_tests/map_proxy_domain_for_cdn.sh

proxy.pm.example.com runs PageSpeed, fetches an image from
origin.pm.example.com, optimizes it once and rewrites it to
cdn.pm.example.com/external/, a PageSpeed-unplugged reverse proxy back to
proxy.pm. After a cache flush the optimized image is reconstructed. All
three vhosts already exist in test/system/setup_apache_test.sh (from
install/debug.conf.template); nothing new is added here.

This module's first test's exact image_rewrites delta and
test_cache_compression_pre_gzipping.py's first-fetch assertion both hold
only on the first run in a lane session and in declaration order (the
module's tests share the CDN image); this is inherited from the bash.
"""

from urllib.parse import urljoin, urlparse

import pytest

from pagespeed_test_framework import (
    assert_stat_delta,
    require_match,
    require_status_ok,
)
from pagespeed_test_framework.stats import settled_stats

TRANSITIVE = "/transitive_proxy.html"
ORIGINAL_PUZZLE_BYTES = 241260
CDN_IMAGE = r"cdn\.pm\.example\.com/external/xPuzzle\.jpg\.pagespeed\.ic"
PROXY_PM = "proxy.pm.example.com"


def cdn_image_url(proxy_pm) -> str:
    page = proxy_pm.fetch_until_count(TRANSITIVE, CDN_IMAGE, 1)
    src = require_match(
        r'"([^"]*xPuzzle[^"]*\.pagespeed[^"]*)"', page, "CDN image URL"
    ).group(1)
    return urljoin(f"http://{PROXY_PM}{TRANSITIVE}", src)


@pytest.mark.requires_secondary
@pytest.mark.requires_stats
@pytest.mark.requires_fixture("secondary_vhosts")
class TestMapProxyDomainForCdn:
    """Bash: MapProxyDomain for CDN setup."""

    def test_image_is_proxied_optimized_once_and_rewritten_to_the_cdn(
        self, vhost_client, vhost_stats_snapshot
    ):
        """Bash: MapProxyDomain for CDN setup."""
        proxy_pm = vhost_client(PROXY_PM)
        old = settled_stats(lambda: vhost_stats_snapshot(PROXY_PM), ["image_rewrites"])
        image_url = cdn_image_url(proxy_pm)
        parsed = urlparse(image_url)
        image = vhost_client(parsed.hostname).get(parsed.path)
        require_status_ok(image, f"optimized image through the CDN ({image_url})")
        assert len(image.body) < ORIGINAL_PUZZLE_BYTES, (
            f"CDN image is {len(image.body)} bytes, not smaller than the original"
        )
        new = vhost_stats_snapshot(PROXY_PM)
        assert_stat_delta(old, new, "image_rewrites", 1,
                          "the file must be rewritten exactly once")
        proxy_pm.fetch_until_count(TRANSITIVE, r"document\.write", 1)


@pytest.mark.requires_secondary
@pytest.mark.requires_stats
@pytest.mark.requires_fixture("secondary_vhosts", "cache_flush")
class TestMapProxyDomainForCdnReconstruct:
    """Bash: map_proxy_domain_cdn_reconstruct (on_cache_flush)."""

    def test_cdn_image_is_reconstructed_after_cache_flush(
        self, vhost_client, vhost_stats_snapshot, flush_cache
    ):
        """Bash: map_proxy_domain_cdn_reconstruct (on_cache_flush)."""
        proxy_pm = vhost_client(PROXY_PM)
        image_url = cdn_image_url(proxy_pm)
        flush_cache()
        old = settled_stats(lambda: vhost_stats_snapshot(PROXY_PM), ["image_rewrites"])
        parsed = urlparse(image_url)
        vhost_client(parsed.hostname).fetch_until(
            parsed.path,
            lambda r: r.status == 200 and len(r.body) < ORIGINAL_PUZZLE_BYTES,
            detail_fn=lambda r: f"status={r.status} bytes={len(r.body)}",
        )
        new = vhost_stats_snapshot(PROXY_PM)
        assert_stat_delta(old, new, "image_rewrites", 1,
                          "Double check that we actually reconstructed")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
