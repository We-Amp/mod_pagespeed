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

"""Pre-gzipped origin responses are not destroyed by the HTTP cache.

Ported from: pagespeed/system/system_tests/cache_compression_pre_gzipping.sh

The chunked, slow-flushing test origin
(test/system/pagespeed_test_framework/flush_origin.py) stands in for
php_gzip.php at /mod_pagespeed_test/flush_origin/gzip.css: CSS that is
already gzip-encoded. In-place optimization must decode it, serve the
optimized CSS gzip-encoded to gzip clients (better compressed when the
cache itself compresses at level 9) and plain to the others, with a
Content-Encoding that always matches the bytes actually sent.
"""

import operator

import pytest

from pagespeed_test_framework import require_status_ok

PATH = "/mod_pagespeed_test/flush_origin/gzip.css"
GZIP = {"Accept-Encoding": "gzip"}


def php_ipro_record(vhost, max_cache_bytes: int, cache_bytes_op, cache_bytes_cmp: int) -> None:
    """Bash: php_ipro_record url max_cache_bytes cache_bytes_op cache_bytes_cmp."""
    first = vhost.get(PATH)
    require_status_ok(first, "first (recording) fetch")
    assert first.header_values("Content-Encoding") == ["gzip"], (
        f"origin gzip lost on the first fetch: Content-Encoding "
        f"{first.header_values('Content-Encoding')}"
    )

    # Now check that we receive the optimized content from cache (not
    # double-zipped). When the resource is optimized, "peachpuff" is
    # rewritten to its hex equivalent (ffdab9).
    vhost.fetch_until_count(PATH, r"ffdab9", 1)

    from_cache = vhost.get(PATH, headers=GZIP)
    require_status_ok(from_cache, "gzip fetch from cache")
    size = len(from_cache.body)
    assert size < max_cache_bytes, (
        f"gzip body from cache is {size} bytes, expected < {max_cache_bytes}"
    )
    assert cache_bytes_op(size, cache_bytes_cmp), (
        f"gzip body from cache is {size} bytes, expected {cache_bytes_op.__name__} "
        f"{cache_bytes_cmp}"
    )

    # Ensure that the Content-Encoding header matches the data that is sent.
    again = vhost.get(PATH, headers=GZIP)
    require_status_ok(again, "second gzip fetch")
    assert again.header_values("Content-Encoding") == ["gzip"], (
        f"gzip bytes without Content-Encoding: gzip "
        f"({again.header_values('Content-Encoding')})"
    )

    # The data should be uncompressed, but minified, at this point.
    plain = vhost.get(PATH)
    require_status_ok(plain, "plain fetch")
    assert len(plain.body) > 10000, (
        f"plain body is {len(plain.body)} bytes, expected > 10000"
    )
    assert "ffdab9" in plain.text, "plain body is not the optimized CSS"

    # We did not send the Accept-Encoding header, so we don't expect the
    # data to carry any Content-Encoding header.
    assert plain.header_values("Content-Encoding") == [], (
        f"plain response carries Content-Encoding: {plain.header_values('Content-Encoding')}"
    )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "flush_origin")
class TestCacheCompressionPreGzipping:

    def test_pre_gzipping_survives_a_compressed_cache(self, vhost_client):
        """Bash: Cache Compression On: PHP Pre-Gzipping does not get destroyed by the cache."""
        php_ipro_record(vhost_client("compressedcache.example.com"), 150, operator.lt, 175)

    def test_pre_gzipping_survives_an_uncompressed_cache(self, vhost_client):
        """Bash: Cache Compression Off: PHP Pre-Gzipping does not get destroyed by the cache."""
        php_ipro_record(vhost_client("uncompressedcache.example.com"), 200, operator.gt, 150)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
