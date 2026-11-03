#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""nginx: a URL purge through the optimizer worker's management API still
finds a site's entry after the worker restarts.

The case itself is system/test_daemon_site.py's; on nginx it runs from this
file, after the other nginx cases that use the worker.
Needs the lane fixture daemon_arm.
"""

import pytest

from system.test_daemon_site import (  # noqa: F401  (the module's fixtures)
    a,
    api,
    check_a_url_purge_finds_the_entry_after_a_worker_restart,
)

pytestmark = [
    pytest.mark.nginx_only,
    pytest.mark.requires_fixture("daemon_arm"),
]


def test_a_url_purge_finds_the_entry_after_a_worker_restart(a, api):  # noqa: F811
    check_a_url_purge_finds_the_entry_after_a_worker_restart(a, api)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
