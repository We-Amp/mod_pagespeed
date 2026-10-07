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

"""MapProxyDomain: optimizing resources proxied from another domain.

Ported from: pagespeed/system/system_tests/map_proxy_domain.sh

The external origins the bash suite reached over the internet
(www.gstatic.com and the proxy test host, selfsigned.modpagespeed.com) are
local stand-ins here (test/system/setup_apache_test.sh's external_origin
fixture): both names resolve to this host and a *:80 vhost answers for
them. The MapProxyDomain mappings and the reverse-proxy vhost are otherwise
unchanged from install/debug.conf.template.
"""

import re
from urllib.parse import urljoin, urlparse

import pytest

from pagespeed_test_framework import require_match, require_status_ok

LEAF = "/mod_pagespeed_example/proxy_external_resource.html?PageSpeedFilters=-inline_images"


def _is_short_private_cache(cache_control: str) -> bool:
    """private, with a short max-age (bash's exact 300 was "fetched just now")."""
    if "private" not in cache_control:
        return False
    match = re.search(r"max-age=(\d+)", cache_control)
    return bool(match) and 0 < int(match.group(1)) <= 300


def rewrite_proxied_gif(client) -> str:
    """Rewrite the page until 1.gif is proxied; return the gif's absolute URL."""
    page = client.fetch_until_count(LEAF, r"1\.gif\.pagespeed", 1)
    src = require_match(
        r'src="([^"]*1\.gif\.pagespeed[^"]*)"', page, "proxied 1.gif URL"
    ).group(1)
    return urljoin(f"http://{client.host}{LEAF}", src)


@pytest.mark.requires_fixture("external_origin")
class TestMapProxyDomain:
    """Bash: MapProxyDomain (the primary-vhost half)."""

    def test_map_proxy_domain_rewrites_and_serves_the_external_image(self, client):
        """Bash: MapProxyDomain."""
        gif_url = rewrite_proxied_gif(client)
        assert gif_url.endswith("gif"), f"proxied image URL {gif_url} does not end in gif"
        parsed = urlparse(gif_url)
        image = client.get(parsed.path)
        require_status_ok(
            image, f"proxied {gif_url} (a 404 is a failed download of the proxied gif)"
        )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "external_origin")
class TestMapProxyDomainDifferentCache:
    """Bash: $PROXIED_IMAGE cases and the Issue 582 reverse proxy.

    These fetch the same proxied gif through a vhost with its own cache
    (secondary.example.com), so a hit here cannot be the primary vhost's
    output-cache entry -- and the Issue 582 reverse proxy, which used to
    403 on a .pagespeed. URL.
    """

    def test_proxied_image_with_its_hash_gets_one_year_cache(self, client, vhost_client):
        """Bash: $PROXIED_IMAGE expecting one year cache."""
        gif_url = rewrite_proxied_gif(client)
        leaf = urlparse(gif_url).path.rsplit("/", 1)[1]
        vhost_client("secondary.example.com").fetch_until(
            f"/gstatic_images/{leaf}",
            lambda r: "max-age=31536000" in r.header("Cache-Control"),
            detail_fn=lambda r: f"status={r.status} Cache-Control={r.header('Cache-Control')!r}",
        )

    def test_proxied_image_with_wrong_hash_gets_short_private_cache(self, client, vhost_client):
        """Bash: Fetching $PROXIED_IMAGE expecting short private cache.

        Bash's exact max-age=300 assumed the input had just been fetched;
        the TTL counts down with the cached input's age, so the invariant is
        a short private window, not the literal value 300.

        The wrong-hash extension (.jpg on what is otherwise a .gif path) is
        the bash's own mismatch (map_proxy_domain.sh:47-48), kept as written.
        """
        gif_url = rewrite_proxied_gif(client)
        leaf = urlparse(gif_url).path.rsplit("/", 1)[1]
        prefix, _, _ = leaf.rpartition(".ce.")
        wrong_hash_leaf = f"{prefix}.ce.0.jpg"
        vhost_client("secondary.example.com").fetch_until(
            f"/gstatic_images/{wrong_hash_leaf}",
            lambda r: _is_short_private_cache(r.header("Cache-Control")),
            detail_fn=lambda r: f"status={r.status} Cache-Control={r.header('Cache-Control')!r}",
        )

    def test_reverse_proxy_serves_pagespeed_url_with_pagespeed_off(self, vhost_client):
        """Bash: Reverse proxy a pagespeed URL (used to 403, Issue 582).

        The .pagespeed. URL is polled briefly rather than fetched once. Its
        reconstruction runs through a chain of loopback requests (the reverse
        proxy, the stand-in origin, and the module's own fetch of yellow.css
        back through the proxy), and when that chain stalls past the module's
        fetch deadline the module remembers the input as failed for minutes
        and answers 404 ("cannot access the original"). The lane runs enough
        prefork workers for the chain; the poll covers the remaining jitter.
        Module issue #1059 records the stall-to-outage behaviour.
        """
        proxy = vhost_client("selfsigned.modpagespeed.com")
        styles = "/mod_pagespeed_example/styles"
        plain = proxy.get(f"{styles}/yellow.css")
        assert plain.status == 200, (
            f"reverse-proxied {styles}/yellow.css: HTTP {plain.status}: {plain.text[:300]}"
        )
        rewritten = f"{styles}/A.yellow.css.pagespeed.cf.KM5K8SbHQL.css"
        proxy.fetch_until(
            rewritten,
            lambda r: r.status == 200,
            timeout=20.0,
            detail_fn=lambda r: f"reverse-proxied {rewritten}: HTTP {r.status}: {r.text[:120]!r}",
        )


PROXIED_PUZZLE = "/modpagespeed_http/Puzzle.jpg"
ORIGIN_HOST_HEADER = {"Host": "selfsigned.modpagespeed.com"}
IPRO_ETAG_PREFIX = 'W/"PSA-aj-'


def _ipro_etag(response) -> bool:
    return response.header("ETag").startswith(IPRO_ETAG_PREFIX)


def _etag_detail(response) -> str:
    return f"status={response.status} ETag={response.header('ETag')!r}"


@pytest.mark.requires_fixture("external_origin")
class TestMapProxyDomainInPlace:
    """Bash: proxying non-.pagespeed. resources from the modpagespeed_http
    mapping (map_proxy_domain.sh, part 2).

    The primary vhost's server-scope MapProxyDomain (install/debug.conf.
    template:69-71) maps /modpagespeed_http to the external-origin stand-in's
    /do_not_modify; the origin is reached directly (client + Host header),
    the same way system/test_lane_fixtures.py::TestExternalOrigin does.
    """

    def test_proxied_external_image_is_optimized_in_place(self, client):
        """Bash: proxying from external domain should optimize images in-place."""
        response = client.fetch_until(
            PROXIED_PUZZLE, _ipro_etag, detail_fn=_etag_detail
        )
        assert response.header("Content-Length"), (
            "optimized in-place response should carry Content-Length"
        )
        assert "chunked" not in response.header("Transfer-Encoding"), (
            "optimized in-place response should not be chunked"
        )

    def test_proxied_image_honours_quality_and_strips_origin_cookies(self, client):
        """Bash: Proxying image from another domain, customizing image compression."""
        response = client.fetch_until(
            f"{PROXIED_PUZZLE}?PageSpeedJpegRecompressionQuality=75",
            lambda r: r.status == 200 and len(r.body) < 90000,
            detail_fn=lambda r: f"status={r.status} bytes={len(r.body)} expected<90000",
        )
        assert _ipro_etag(response), f"ETag {response.header('ETag')!r} is not in-place"
        assert response.header_values("Set-Cookie") == [], (
            f"rewritten image leaks origin cookies: {response.header_values('Set-Cookie')}"
        )
        origin = client.get("/do_not_modify/Puzzle.jpg", headers=ORIGIN_HOST_HEADER)
        require_status_ok(origin, "origin Puzzle.jpg")
        assert origin.header_values("Set-Cookie"), (
            "the origin must set a cookie on Puzzle.jpg for the strip check to mean anything"
        )

    def test_proxying_html_from_external_domain_is_refused(self, client):
        """Bash: proxying HTML from external domain should not work."""
        response = client.get("/modpagespeed_http/evil.html")
        assert response.status >= 400, (
            f"proxied external HTML was served: HTTP {response.status}"
        )
        assert response.header_values("Set-Cookie") == [], (
            f"refused response carries Set-Cookie: {response.header_values('Set-Cookie')}"
        )

    def test_origin_html_fetched_directly_keeps_its_cookie(self, client):
        """Bash: Fetching the HTML directly from the origin is fine including cookie."""
        response = client.get("/do_not_modify/evil.html", headers=ORIGIN_HOST_HEADER)
        require_status_ok(response, "origin evil.html")
        assert any(v.startswith("test-cookie") for v in response.header_values("Set-Cookie")), (
            f"origin cookie missing: {response.header_values('Set-Cookie')}"
        )

    def test_ipro_from_map_proxy_domain_is_request_independent(self, client):
        """Bash: Ipro from MapProxyDomain is request-independent."""
        response = client.fetch_until(
            PROXIED_PUZZLE, _ipro_etag,
            headers={"User-Agent": "webp", "Accept": "image/webp"},
            detail_fn=_etag_detail,
        )
        assert response.header("Content-Type").lower().startswith("image/jpeg"), (
            f"Content-Type [{response.header('Content-Type')}], expected image/jpeg"
        )
        assert response.header_values("Vary") == [], (
            f"in-place response carries Vary: {response.header_values('Vary')}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
