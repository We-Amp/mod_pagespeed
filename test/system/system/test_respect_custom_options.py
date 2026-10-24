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

"""Directory options apply when a resource is rewritten on request.

Ported from: pagespeed/system/system_tests/respect_custom_options.sh

The same image is served from two directories; custom_options/ disables
convert_jpeg_to_progressive (its .htaccess), so its rewrite is larger.
The <Files> case scopes the same disable to a single rewritten URL's leaf.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestRespectCustomOptions:
    """Bash: Respect custom options on resources."""

    def test_directory_options_apply_to_resource_rewrites(
        self, client: PageSpeedClient, example_root: str, test_root: str
    ):
        for url, limit in (
            (f"{example_root}/images/xPuzzle.jpg.pagespeed.ic.fakehash.jpg", 98276),
            (f"{test_root}/custom_options/xPuzzle.jpg.pagespeed.ic.fakehash.jpg", 102902),
        ):
            client.fetch_until(
                url,
                condition=lambda r, n=limit: r.status == 200 and len(r.body) <= n,
                timeout=100.0,
                detail_fn=lambda r, n=limit: f"status={r.status} bytes={len(r.body)} limit={n}",
            )


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestFilesSectionOptions:
    """<Files> sections apply to .pagespeed. URLs (module issue #325).

    The module's map-to-storage hook serves a .pagespeed. URL through a
    one-character stand-in leaf for httpd's directory walk; only the file
    walk after it can match a <Files> section against the real resource
    name. files_section/.htaccess scopes ModPagespeedDisableFilters
    convert_jpeg_to_progressive to the exact leaf of one rewritten URL: its
    rewrite must come out non-progressive, larger than the progressive
    baseline of the same image under the default options.
    """

    def test_files_scoped_options_apply_to_a_pagespeed_resource(
        self, client: PageSpeedClient, example_root: str, test_root: str
    ):
        baseline_url = (
            f"{example_root}/images/xPuzzle.jpg.pagespeed.ic.fakehash.jpg"
        )
        client.fetch_until(
            baseline_url,
            condition=lambda r: r.status == 200 and len(r.body) <= 98276,
            timeout=100.0,
            detail_fn=lambda r: f"status={r.status} bytes={len(r.body)} limit=98276",
        )
        scoped_url = (
            f"{test_root}/files_section/xPuzzle.jpg.pagespeed.ic.fakehash.jpg"
        )
        client.fetch_until(
            scoped_url,
            # Larger than the progressive baseline above: without the
            # <Files> section this rewrite would be those same small bytes.
            condition=lambda r: (
                r.status == 200 and 98276 < len(r.body) <= 102902
            ),
            timeout=100.0,
            detail_fn=lambda r: (
                f"status={r.status} bytes={len(r.body)} "
                f"expected 98276 < bytes <= 102902"
            ),
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
