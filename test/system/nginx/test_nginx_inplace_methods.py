#!/usr/bin/env python3
# Copyright 2024 Google LLC
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

"""nginx in-place method tests.

In-place optimization considers GET and HEAD requests only: those are the
methods whose responses may be looked up in, and recorded into, the cache.
A POST to a resource URL must take the path a request takes when in-place
rewriting is disabled -- no lookup (it is never answered with a cached GET
response), no recorder (its own response is never stored under the URL) --
and nginx answers the POST itself.
"""

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
    assert_not_contains,
)


@pytest.mark.nginx_only
class TestInPlaceMethods:
    """POSTs are neither answered from nor recorded into the in-place cache."""

    @pytest.mark.requires_module
    def test_post_not_answered_from_the_in_place_cache(
        self, client: PageSpeedClient, example_root: str
    ):
        """A POST to a warm in-place URL is not answered with the cached body.

        nginx answers a POST to a static file itself: the static handler
        refuses it with 405 (ngx_http_static_module.c).
        """
        css_url = f"{example_root}/styles/yellow.css"

        # Warm the classic in-place cache: the optimized (whitespace-stripped)
        # form is being served.
        client.fetch_until(
            css_url,
            condition=lambda r: "background-color: yellow" not in r.text
            and "yellow" in r.text,
            timeout=60.0,
        )

        # The POST must not be answered with that cached GET response; nginx
        # serves the POST itself and the static handler says 405.
        response = client.post(css_url, data="x=1")
        assert_http_status(response, 405)

    @pytest.mark.requires_module
    def test_post_records_nothing(
        self, client: PageSpeedClient, test_root: str
    ):
        """POSTs to a cacheable URL store nothing: a later GET gets the GET body.

        The echo origin answers POST with a different cacheable body than GET
        (a proxied location, so the response passes the module's filters with
        no internal redirect). The POSTs come first, before any GET warms the
        URL: this ordering is what the gate has to hold.
        """
        url = f"{test_root}/inplace_methods/echo.css"

        # The origin mechanism works: the POST reaches the origin and its own
        # body comes back.
        response = client.post(url, data="x=1")
        assert_http_status(response, 200)
        assert_contains(response, "post-body-marker")
        for _ in range(2):
            assert_http_status(client.post(url, data="x=1"), 200)

        # The GET must still return the GET body -- in its optimized
        # (whitespace-stripped) form -- never the POST body.
        response = client.fetch_until(
            url,
            condition=lambda r: "get-body-marker" in r.text
            and "color: #00ff00" not in r.text,
            timeout=60.0,
        )
        assert_http_status(response, 200)
        assert_not_contains(response, "post-body-marker")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
