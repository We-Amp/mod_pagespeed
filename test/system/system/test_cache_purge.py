#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Individual-URL and global cache purging with the PURGE method.

Ported from: pagespeed/automatic/system_test_helpers.sh cache_purge_test
(lines 906-1012), the PURGE iteration of its CACHE_PURGE_METHODS loop, as
run by pagespeed/system/system_tests/cache_purge.sh against
http://purge.example.com. The admin GET (cache?purge=) iteration and the
psoff-* vhost runs reuse _run_purge_cycle.
"""

import functools
import os
import shutil
import time
from typing import Callable, Dict

import pytest

from pagespeed_test_framework import (
    Response,
    assert_stat_delta,
    require_match,
    require_status_ok,
)
from pagespeed_test_framework.client import VhostClient
from pagespeed_test_framework.stats import settled_stats

PURGE_VHOST = "purge.example.com"
COMBINE_CSS = "/combine_css.html"
STATS_PATH = "/pagespeed_admin/statistics"
_ADMIN_PURGE_HEADERS = {"X-Requested-With": "XMLHttpRequest"}


def _purge(client: VhostClient, path: str) -> None:
    """cache_purge PURGE <path>: $CURL --request PURGE $PURGE_ROOT/<path>."""
    response = client.purge(path)
    require_status_ok(response, f"PURGE {client.base_url}{path}")


def _admin_purge(
    client: VhostClient, path: str, admin_path: str, global_admin_path: str
) -> None:
    """cache_purge GET <path>: the admin-console purge, gated on POST + XHR.

    Bash: cache_purge's GET-method branch (purge=$path on
    pagespeed_admin/cache). The gated admin (system_test_helpers.sh's
    read_metadata_cache/purge dance) only honours this as a POST carrying
    X-Requested-With: XMLHttpRequest; the whole-cache purge ("*") is
    answered only by the global admin, never the per-vhost one.
    """
    purge_path = path[1:] if path.startswith("/") else path
    if purge_path == "*":
        target = f"{global_admin_path}/cache?purge=*"
    else:
        target = f"{admin_path}/cache?purge={purge_path}"
    response = client.post(target, headers=_ADMIN_PURGE_HEADERS)
    require_status_ok(response, f"admin-console purge of {path}")


def _metadata_cache_entry(client: VhostClient, path: str) -> str:
    """read_metadata_cache: $PURGE_ROOT/pagespeed_admin/cache?url=$PURGE_ROOT/<path>."""
    response = client.get(f"/pagespeed_admin/cache?url={client.base_url}/{path}")
    require_status_ok(response, f"metadata cache lookup of {path}")
    return response.text


def _assert_cache_ok(client: VhostClient, path: str, expected: bool) -> None:
    marker = "cache_ok:true" if expected else "cache_ok:false"
    entry = _metadata_cache_entry(client, path)
    assert marker in entry, (
        f"expected {marker} for {path}; pagespeed_admin/cache answered: {entry[:1000]}"
    )


def _purge_stats(client: VhostClient) -> Dict[str, int]:
    """$WGET_DUMP $PURGE_ROOT/pagespeed_admin/statistics (per-vhost statistics)."""
    return client.get_statistics(stats_path=STATS_PATH, disable_pagespeed=False)


def _rewrite_combine_css(client: VhostClient) -> Response:
    """fetch_until $PURGE_ROOT/combine_css.html 'grep -c pagespeed.cf' 4."""
    return client.fetch_until_count(COMBINE_CSS, r"pagespeed\.cf", 4, timeout=100.0)


def _run_purge_cycle(
    client: VhostClient,
    doc_root,
    purge: Callable[[VhostClient, str], None],
) -> None:
    """One iteration of cache_purge_test's method loop, with `purge` as the method."""
    page = _rewrite_combine_css(client)
    yellow_css = require_match(
        r'href="([^"]*yellow\.css[^"]*)"', page, "rewritten yellow.css URL"
    ).group(1)
    blue_css = require_match(
        r'href="([^"]*blue\.css[^"]*)"', page, "rewritten blue.css URL"
    ).group(1)

    purple_path = f"styles/{os.getpid()}"
    purple_dir = doc_root / "purge" / purple_path
    purple_dir.mkdir(parents=True, exist_ok=True)
    purple_file = purple_dir / "purple.css"
    purple_url = f"/{purple_path}/purple.css"
    try:
        _assert_cache_ok(client, yellow_css, True)
        _assert_cache_ok(client, blue_css, True)
        purple_file.write_text("body { background: MediumPurple; }\n")
        client.fetch_until_contains(purple_url, "9370db", timeout=100.0)
        purple_file.write_text("body { background: black; }\n")

        purge(client, "/*")

        _assert_cache_ok(client, yellow_css, False)
        _assert_cache_ok(client, blue_css, False)
        client.fetch_until_contains(purple_url, "#000", timeout=100.0)
        purge(client, purple_url)

        stats_0 = settled_stats(
            lambda: _purge_stats(client), ["num_resource_fetch_successes"]
        )
        _rewrite_combine_css(client)
        stats_1 = _purge_stats(client)
        # Having rewritten 4 CSS files, we will have done 4 resource fetches.
        assert_stat_delta(stats_0, stats_1, "num_resource_fetch_successes", 4)

        # Sanity check: rewriting the same CSS file results in no new fetches.
        _rewrite_combine_css(client)
        stats_2 = _purge_stats(client)
        assert_stat_delta(stats_1, stats_2, "num_resource_fetch_successes", 0)

        # Purge one file; it is the only one refetched on the next rewrite.
        _assert_cache_ok(client, yellow_css, True)
        _assert_cache_ok(client, blue_css, True)
        purge(client, "/styles/yellow.css")
        _assert_cache_ok(client, yellow_css, False)
        _assert_cache_ok(client, blue_css, True)

        time.sleep(1)
        _rewrite_combine_css(client)
        stats_3 = _purge_stats(client)
        assert_stat_delta(stats_2, stats_3, "num_resource_fetch_successes", 1)
    finally:
        shutil.rmtree(purple_dir, ignore_errors=True)


@pytest.mark.requires_stats
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "doc_root_scratch")
class TestCachePurgeWithPurgeMethod:
    """cache_purge_test http://purge.example.com, CACHE_PURGE_METHODS=PURGE."""

    def test_purge_method_invalidates_urls_and_forces_refetch(self, vhost_client, doc_root):
        _run_purge_cycle(vhost_client(PURGE_VHOST), doc_root, _purge)


@pytest.mark.requires_stats
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "doc_root_scratch")
class TestCachePurgeWithAdminConsole:
    """cache_purge_test http://purge.example.com, CACHE_PURGE_METHODS=GET.

    The admin-console (GET-method) iteration of cache_purge_test; the PURGE
    iteration is TestCachePurgeWithPurgeMethod above.
    """

    def test_admin_console_purge_invalidates_urls_and_forces_refetch(
        self, vhost_client, doc_root, server_config
    ):
        purge = functools.partial(
            _admin_purge,
            admin_path=server_config.admin_path,
            global_admin_path=server_config.global_admin_path,
        )
        _run_purge_cycle(vhost_client(PURGE_VHOST), doc_root, purge)


@pytest.mark.requires_stats
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "doc_root_scratch")
class TestCachePurgeWithPageSpeedOffInVhost:
    """Purging still works when a vhost has PageSpeed off and its document
    root turns it back on, either through .htaccess or a <Directory> block.

    Bash: pagespeed/apache/system_tests/cache_flushing.sh, "Cache purging
    with PageSpeed off in vhost, but on in htaccess file." and "... but on
    in directory." (cache_purge_test against psoff-htaccess-on.example.com
    and psoff-dir-on.example.com).
    """

    @pytest.mark.parametrize(
        "server_name",
        [
            pytest.param("psoff-htaccess-on.example.com", id="on-in-htaccess"),
            pytest.param("psoff-dir-on.example.com", id="on-in-directory"),
        ],
    )
    @pytest.mark.parametrize("method", ["purge_verb", "admin_console"])
    def test_cache_purge(
        self, vhost_client, doc_root, server_config, server_name, method
    ):
        purge = (
            _purge
            if method == "purge_verb"
            else functools.partial(
                _admin_purge,
                admin_path=server_config.admin_path,
                global_admin_path=server_config.global_admin_path,
            )
        )
        _run_purge_cycle(vhost_client(server_name), doc_root, purge)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
