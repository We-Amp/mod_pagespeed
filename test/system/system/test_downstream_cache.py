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

"""Downstream-cache integration: resource headers and keyed rebeaconing.

Ported from: pagespeed/system/system_tests/downstream_cache_integration_headers.sh
and downstream_cache_rebeaconing.sh

The downstreamcacherebeacon vhost marks its HTML "Cache-Control: private,
max-age=3000" (the downstream-cache case) and leaves the page's stylesheet and
image publicly cacheable, since PageSpeed never fetches a private resource for
rewriting and the critical-CSS beacon needs the stylesheet's selectors.

The bash matched these headers with grep -q, a substring test over the raw
dump; PRIVATE/NO_CACHE are checked as substrings of a header line here too,
never as an exact whole-line match.
"""

import re

import pytest

from pagespeed_test_framework import require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

HOST = "downstreamcacherebeacon.example.com"
PAGE = "/mod_pagespeed_test/downstream_caching.html"
BLOCKING = {"X-PSA-Blocking-Rewrite": "psatest"}
PRIVATE = "Cache-Control: private, max-age=3000"
NO_CACHE = "Cache-Control: max-age=0, no-cache"


def _lines(response):
    return [f"{name}: {value}" for name, value in response.raw_headers]


def _has(needle, response):
    return any(needle in line for line in _lines(response))


def _fetch(vhost, filters, extra=None):
    headers = dict(BLOCKING)
    headers.update(extra or {})
    response = vhost.get(f"{PAGE}?PageSpeedFilters={filters}", headers=headers)
    require_status_ok(response, f"{filters} {extra}")
    return response


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestDownstreamCache:
    """Bash: Downstream cache integration caching headers; ... with downstream
    cache rebeaconing (lazyload_images and prioritize_critical_css)."""

    def test_rewritten_resource_has_caching_headers(self, vhost_client):
        url = "/mod_pagespeed_example/images/xCuppa.png.pagespeed.ic.0.png"
        response = vhost_client("downstreamcacheresource.example.com").get(url)
        require_status_ok(response, url)
        for name in ("Cache-Control", "Expires", "Last-Modified"):
            assert response.header_values(name), f"{name} missing: {response.raw_headers}"

    def test_image_rebeaconing_needs_the_key(self, vhost_client):
        vhost = vhost_client(HOST)
        run = r"pagespeed\.CriticalImages\.Run"
        plain = _fetch(vhost, "lazyload_images")
        assert not re.search(run, plain.text) and _has(PRIVATE, plain), _lines(plain)
        keyed = _fetch(vhost, "lazyload_images", {"PS-ShouldBeacon": "random_rebeaconing_key"})
        assert re.search(run, keyed.text) and _has(NO_CACHE, keyed), _lines(keyed)
        wrong = _fetch(vhost, "lazyload_images", {"PS-ShouldBeacon": "wrong_rebeaconing_key"})
        assert not re.search(run, wrong.text) and _has(PRIVATE, wrong), _lines(wrong)

    def test_critical_css_rebeaconing_without_key_is_not_instrumented(self, vhost_client):
        vhost = vhost_client(HOST)
        init = r"pagespeed\.criticalCssBeaconInit"
        plain = _fetch(vhost, "prioritize_critical_css")
        assert not re.search(init, plain.text) and _has(PRIVATE, plain), _lines(plain)
        wrong = _fetch(vhost, "prioritize_critical_css", {"PS-ShouldBeacon": "wrong_rebeaconing_key"})
        assert not re.search(init, wrong.text) and _has(PRIVATE, wrong), _lines(wrong)

    def test_critical_css_rebeaconing_with_key_is_instrumented(self, vhost_client):
        # The beacon needs the stylesheet's selectors, and PageSpeed only
        # fetches a stylesheet it may cache: the vhost's private Cache-Control
        # therefore applies to the HTML alone (issue #1060 -- with it on every
        # response the stylesheet was remembered as uncacheable and the beacon
        # could never be instrumented). The summary is computed off the first
        # request, so poll until the instrumented page comes back.
        vhost = vhost_client(HOST)
        keyed = vhost.fetch_until(
            f"{PAGE}?PageSpeedFilters=prioritize_critical_css",
            condition=lambda r: count_matching_lines(r.text, r"criticalCssBeaconInit") == 2,
            headers={"PS-ShouldBeacon": "random_rebeaconing_key"},
            timeout=100.0,
        )
        assert _has(NO_CACHE, keyed), _lines(keyed)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
