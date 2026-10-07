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

"""ShowCache: metadata-cache lookups through the admin cache page.

Ported from: pagespeed/system/system_tests/show_cache.sh

The metadata cache key depends on the rewrite options: a lookup with the
options the page was rewritten with hits (cache_ok:true), a lookup with any
other option value misses, and flushing the cache turns the hit into a miss.

The bash's two form/bogus-URL cases (show_cache.sh:15-28) -- ShowCache
without a URL rendering an HTML form, and a bogus URL 404ing -- are
deliberately not ported: the admin is JSON now, so neither case applies.
"""

from urllib.parse import urljoin

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    require_match,
    require_status_ok,
)

OPTIONS = "PageSpeedImageInlineMaxBytes=6765"
NEW_OPTIONS = "PageSpeedImageInlineMaxBytes=6766"


def _origin(server_config) -> str:
    """Bash: http://$HOSTNAME (the port only when it is not 80)."""
    if server_config.port == 80:
        return f"http://{server_config.host}"
    return f"http://{server_config.host}:{server_config.port}"


def rewritten_puzzle_url(client: PageSpeedClient, server_config, example_root: str) -> str:
    """Rewrite rewrite_images.html with OPTIONS; return the .pagespeed. Puzzle URL."""
    page_path = f"{example_root}/rewrite_images.html?{OPTIONS}"
    page = client.fetch_until_count(page_path, r"Puzzle\.jpg\.pagespeed\.ic\.", 1)
    src = require_match(
        r'"([^"]*Puzzle\.jpg\.pagespeed\.ic\.[^"]*)"', page, "rewritten Puzzle.jpg URL"
    ).group(1)
    return urljoin(f"{_origin(server_config)}{page_path}", src)


def show_cache(client: PageSpeedClient, server_config, url: str, options: str):
    response = client.get(f"{server_config.admin_path}/cache?url={url}&{options}")
    require_status_ok(response, "ShowCache query")
    return response


class TestShowCache:

    def test_show_cache_hit_with_unique_options(
        self, client: PageSpeedClient, server_config, example_root: str
    ):
        """Bash: ShowCache with valid, present URL, with unique options."""
        url = rewritten_puzzle_url(client, server_config, example_root)
        out = show_cache(client, server_config, url, OPTIONS)
        assert "cache_ok:true" in out.text, f"expected a hit: {out.text[:500]}"
        assert "mod_pagespeed_example/images/Puzzle.jpg" in out.text, (
            f"lookup does not name the resource: {out.text[:500]}"
        )

    def test_show_cache_misses_with_new_options(
        self, client: PageSpeedClient, server_config, example_root: str
    ):
        """Bash: ShowCache with same URL but new options misses."""
        url = rewritten_puzzle_url(client, server_config, example_root)
        out = show_cache(client, server_config, url, NEW_OPTIONS)
        assert "cache_ok:false" in out.text, f"expected a miss: {out.text[:500]}"


@pytest.mark.requires_fixture("cache_flush")
class TestShowCacheAfterFlush:

    def test_show_cache_misses_after_flush(
        self, client: PageSpeedClient, server_config, example_root: str, flush_cache
    ):
        """Bash: ShowCache with same URL and matching options misses after flush."""
        url = rewritten_puzzle_url(client, server_config, example_root)
        before = show_cache(client, server_config, url, OPTIONS)
        assert "cache_ok:true" in before.text, (
            f"expected a hit before the flush: {before.text[:500]}"
        )
        flush_cache()
        client.fetch_until(
            f"{server_config.admin_path}/cache?url={url}&{OPTIONS}",
            lambda r: "cache_ok:false" in r.text,
            timeout=30.0,
            detail_fn=lambda r: f"status={r.status} body={r.text[:200]!r}",
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
