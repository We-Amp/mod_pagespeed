#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""nginx: the sizes of optimized responses are counted from the cache after
the server was stopped completely and started again with its cache kept.

The case itself is nginx/test_nginx_served_savings.py's. It runs from this
file because it restarts the suite's own nginx: a restart drops everything
the server holds in memory, statistics included, so it must not happen
between other tests. Test modules run in the order of their names, the nginx
directory is the last one of a full run, and this name sorts after every
other module in it. Keep it last when adding a module.

Environment: as for test_nginx_served_savings.py, and also
PAGESPEED_NGINX_BINARY, PAGESPEED_NGINX_CONFIG_FILE and
PAGESPEED_NGINX_ERROR_LOG (run_nginx_tests.sh exports them).
"""

from pathlib import Path
from typing import Callable, Dict

import pytest

from nginx.test_nginx_served_savings import (
    check_served_sizes_across,
    restart_nginx,
)
from pagespeed_test_framework import PageSpeedClient


@pytest.mark.nginx_only
@pytest.mark.requires_module
@pytest.mark.requires_stats
def test_served_sizes_are_counted_from_the_cache_across_a_restart(
    client: PageSpeedClient,
    example_root: str,
    test_root: str,
    doc_root: Path,
    stats_snapshot: Callable[[], Dict[str, int]],
):
    check_served_sizes_across(
        restart_nginx, client, example_root, test_root, doc_root, stats_snapshot
    )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
