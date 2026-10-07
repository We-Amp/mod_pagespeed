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

"""Cache-Control: public for the Google Cloud CDN cache.

Ported from: pagespeed/automatic/system_tests/gce_public_cache.sh

Google Cloud CDN only caches responses that say "public". The bash suite
asserted that PageSpeed adds "public" only when the request carries "Via:
1.1 google" -- for both in-place-optimized and hash-committed .pagespeed.
resources -- unless the origin itself already said public, which is kept
either way.

That is still the contract for in-place-optimized resources served at
their original URL, and for an origin's own public. It is no longer the
contract for hash-committed .pagespeed. resources: see
test_rewritten_resource_is_public_and_immutable_regardless_of_via for
what superseded it and why.

TestPublicSourceCacheControl goes through secondary_client, the lane's
default secondary vhost with its own cache, where the bash used the
primary, to keep the cache state isolated from the rest of the lane.
"""

from urllib.parse import urljoin, urlparse

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    Response,
    require_match,
    require_status_ok,
)

GOOGLE_VIA = {"Via": "1.1 google"}


def css_link_path(page: Response, page_path: str, pattern: str, what: str) -> str:
    """Path of the first stylesheet link whose href matches pattern.

    Equivalent to: scrape_css_link (absolutifies a relative href).
    """
    href = require_match(r'href="([^"]*' + pattern + r'[^"]*)"', page, what).group(1)
    return urlparse(urljoin(f"http://base.invalid{page_path}", href)).path


def check_public_cc(
    client: PageSpeedClient, path: str, via: bool, expect_public: bool
) -> None:
    """Bash: check_public_cc FILE [-via] public|no."""
    response = client.get(path, headers=GOOGLE_VIA if via else None)
    require_status_ok(response, f"fetch of {path}")
    cache_control = response.header("Cache-Control")
    assert ("public" in cache_control) == expect_public, (
        f"{path} fetched {'with' if via else 'without'} 'Via: 1.1 google': "
        f"Cache-Control [{cache_control}] should "
        f"{'' if expect_public else 'not '}contain 'public'"
    )


def check_public_and_immutable_cc(
    client: PageSpeedClient, path: str, via: bool
) -> None:
    """Current contract for a hash-committed .pagespeed. resource: public
    and immutable, regardless of the Via header (see module docstring)."""
    response = client.get(path, headers=GOOGLE_VIA if via else None)
    require_status_ok(response, f"fetch of {path}")
    cache_control = response.header("Cache-Control")
    for directive in ("public", "immutable"):
        assert directive in cache_control, (
            f"{path} fetched {'with' if via else 'without'} 'Via: 1.1 google': "
            f"Cache-Control [{cache_control}] should contain {directive!r}"
        )


class TestGcePublicCacheVia:
    """public is added iff the request came through the Google cache,

    except a hash-committed .pagespeed. resource, which is now
    unconditionally public and immutable (see
    test_rewritten_resource_is_public_and_immutable_regardless_of_via).
    """

    def test_rewritten_resource_is_public_and_immutable_regardless_of_via(
        self, client: PageSpeedClient, example_root: str
    ):
        """Bash: Cache-Control:public added iff GCE for combined .pagespeed. file.

        The bash case expected "public" on the combined resource only when
        the request carried "Via: 1.1 google". That is no longer the
        contract: a hash-committed .pagespeed. URL commits to its content
        -- changed content mints a new URL -- so the response behind it can
        never change, regardless of who is asking. CHANGELOG.md ("Optimized
        .pagespeed. resources now declare Cache-Control: public,
        immutable") describes the current behavior: such responses are
        unconditionally public and carry the RFC 8246 immutable directive,
        which satisfies Google Cloud CDN's public requirement without
        needing to inspect Via, and lets browsers that honor immutable
        skip revalidating the resource even on a user-triggered reload.
        """
        page_path = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        page = client.fetch_until_count(page_path, r"\.pagespeed\.cc", 1)
        combined = css_link_path(page, page_path, r"\.pagespeed\.cc\.", "combined CSS link")
        for via in (False, True, False, True):
            check_public_and_immutable_cc(client, combined, via)

    def test_ipro_css_is_public_only_behind_google_via(
        self, client: PageSpeedClient, example_root: str
    ):
        """Bash: Cache-Control:public added iff GCE for ipro css file."""
        yellow = f"{example_root}/styles/yellow.css"
        client.fetch_until_count(yellow, r"background-color:#ff0", 1)
        for via, public in ((False, False), (True, True), (False, False), (True, True)):
            check_public_cc(client, yellow, via, public)


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestPublicSourceCacheControl:
    """An origin's own public survives with and without the Google Via."""

    def test_ipro_css_from_public_source_stays_public(
        self, secondary_client: PageSpeedClient, test_root: str
    ):
        """Bash: Cache-Control:public when source has cc:public, for ipro."""
        path = f"{test_root}/public/yellow.css"
        secondary_client.fetch_until_count(path, r"background-color:#ff0", 1)
        for via in (True, False, True, False):
            check_public_cc(secondary_client, path, via, True)

    def test_rewritten_css_from_public_source_stays_public(
        self, secondary_client: PageSpeedClient, test_root: str
    ):
        """Bash: Cache-Control:public when source has cc:public, for .pagespeed."""
        page_path = f"{test_root}/public/rewrite_css.html?PageSpeedFilters=rewrite_css"
        page = secondary_client.fetch_until_count(page_path, r"\.pagespeed\.cf", 1)
        rewritten = css_link_path(page, page_path, r"\.pagespeed\.cf\.", "rewritten CSS link")
        for via in (True, False, True, False):
            check_public_cc(secondary_client, rewritten, via, True)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
