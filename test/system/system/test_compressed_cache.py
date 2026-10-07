#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Metadata-cache compression saves space on the root vhost.

Ported from: pagespeed/apache/system_tests/compressed_cache.sh
"""

import pytest

from pagespeed_test_framework import PageSpeedClient


@pytest.mark.requires_stats
@pytest.mark.requires_fixture("compressed_metadata_cache")
class TestCompressedCache:
    """CompressedCache is racking up savings on the root vhost."""

    def test_compressed_cache_saves_space(
        self, client: PageSpeedClient, example_root: str, server_config, stats_snapshot
    ):
        # Put entries into the metadata cache (the bash case ran after the suite).
        client.fetch_until_contains(
            f"{example_root}/combine_css.html?PageSpeedFilters=combine_css",
            r"\.pagespeed\.cc\.",
            timeout=100.0,
        )
        stats = stats_snapshot()
        for name in ("compressed_cache_original_size", "compressed_cache_compressed_size"):
            assert name in stats, f"statistic {name} missing from {server_config.stats_path}"
        original = stats["compressed_cache_original_size"]
        compressed = stats["compressed_cache_compressed_size"]
        assert compressed < original, f"compressed={compressed} original={original}"
        assert compressed > 0, f"compressed={compressed}"
        assert original > 0, f"original={original}"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
