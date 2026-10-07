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

"""Resources from unauthorized domains: inlined only when allowed, never rewritten.

Ported from: pagespeed/system/system_tests/inline_unauth.sh

The unauthorized origin is https://www.modpagespeed.com, served inside the
lane by a local stand-in (setup_apache_test.sh, configure_external_origin_https).
unauthorizedresources.example.com sets
InlineResourcesWithoutExplicitAuthorization Script,Stylesheet.
"""

import re

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_not_contains,
    require_status_ok,
)
from pagespeed_test_framework.client import Response

_BLOCKING = {"X-PSA-Blocking-Rewrite": "psatest"}
_NOT_AUTHORIZED = (
    "<!--The preceding resource was not rewritten because its domain "
    "(www.modpagespeed.com) is not authorized-->"
)
UNAUTHORIZED_HOST = "unauthorizedresources.example.com"
_JS_PAGE = "/unauthorized/inline_unauthorized_javascript.html"
_CSS_PAGE = "/unauthorized/inline_css.html"
_EXTERNAL_JS_SRC = 'src="https://www.modpagespeed.com/examples/inline_javascript.js"'
_EXTERNAL_CSS_HREF = 'href="https://www.modpagespeed.com/testfiles/google-cse-default.css"'
# Content markers of the stand-in files (test/system/external_origin/).
_JS_STAND_IN = "unauthorizedOriginStandIn"
_CSS_STAND_IN = r"\.gsc-completion-selected"


def _wget_dump(response: Response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks grep this whole dump, headers included.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.headers.items()]
    return "\n".join(lines) + "\n\n" + response.text


def _blocking_dump(client, path: str) -> str:
    """Bash: $WGET_DUMP --header 'X-PSA-Blocking-Rewrite: psatest' $URL > $OUTFILE"""
    return _wget_dump(require_status_ok(client.get(path, headers=_BLOCKING), path))


def _fetch_until_no_tag(client, path: str, tag: str) -> Response:
    """Bash: fetch_until $URL 'grep -c <tag>' 0 -- on a 200 page."""
    pattern = re.compile(tag)

    def done(response: Response) -> bool:
        return response.status == 200 and pattern.search(response.text) is None

    def detail(response: Response) -> str:
        return (f"status={response.status} "
                f"{tag} matches={len(pattern.findall(response.text))} expected=0")

    return client.fetch_until(path, done, detail_fn=detail)


class TestUnauthorizedResourceNotInlined:
    """Primary host: unauthorized resources are never inlined, only flagged.

    No lane fixture needed: this touches no <Directory>/<Location> block,
    only the primary host's blocking-rewrite key (set unconditionally in
    setup_apache_test.sh) and the debug filter.
    """

    def test_unauthorized_js_not_inlined(self, client: PageSpeedClient, test_root: str):
        """start_test no inlining of unauthorized resources"""
        out = _blocking_dump(
            client,
            f"{test_root}{_JS_PAGE}?PageSpeedFilters=inline_javascript,debug")
        assert_contains(out, r"script\ssrc=")
        assert out.count(_NOT_AUTHORIZED) == 1, (
            f"Expected the not-authorized comment exactly once, "
            f"got {out.count(_NOT_AUTHORIZED)}:\n{out[:1500]}")

    def test_unauthorized_css_not_inlined(self, client: PageSpeedClient, test_root: str):
        """start_test no inlining of unauthorized resources"""
        out = _blocking_dump(
            client, f"{test_root}{_CSS_PAGE}?PageSpeedFilters=inline_css,debug")
        assert_contains(out, r"link\srel=")
        assert out.count(_NOT_AUTHORIZED) == 1, (
            f"Expected the not-authorized comment exactly once, "
            f"got {out.count(_NOT_AUTHORIZED)}:\n{out[:1500]}")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "external_origin")
class TestUnauthorizedResourcesOnAllowingVhost:
    """unauthorizedresources.example.com: allows inlining, never rewriting.

    The inlining fetch reaches the www.modpagespeed.com stand-in
    (lane fixture "external_origin"), so both fixtures are required.
    """

    def test_inline_unauthorized_resources_allows_inlining_js(self, vhost_client):
        """start_test inline_unauthorized_resources allows inlining"""
        response = _fetch_until_no_tag(
            vhost_client(UNAUTHORIZED_HOST),
            f"/mod_pagespeed_test{_JS_PAGE}?PageSpeedFilters=inline_javascript",
            r"script\ssrc=")
        assert_contains(response, _JS_STAND_IN)

    def test_inline_unauthorized_resources_does_not_allow_rewriting_js(self, vhost_client):
        """start_test inline_unauthorized_resources does not allow rewriting"""
        out = _blocking_dump(
            vhost_client(UNAUTHORIZED_HOST),
            f"/mod_pagespeed_test{_JS_PAGE}?PageSpeedFilters=rewrite_javascript")
        assert_contains(out, r"script\ssrc=")
        assert_contains(out, re.escape(_EXTERNAL_JS_SRC))

    def test_inline_unauthorized_resources_allows_inlining_css(self, vhost_client):
        """start_test inline_unauthorized_resources allows inlining"""
        response = _fetch_until_no_tag(
            vhost_client(UNAUTHORIZED_HOST),
            f"/mod_pagespeed_test{_CSS_PAGE}?PageSpeedFilters=inline_css",
            r"link\srel=")
        assert_contains(response, _CSS_STAND_IN)

    def test_inline_unauthorized_resources_does_not_allow_rewriting_css(self, vhost_client):
        """start_test inline_unauthorized_resources does not allow rewriting"""
        out = _blocking_dump(
            vhost_client(UNAUTHORIZED_HOST),
            f"/mod_pagespeed_test{_CSS_PAGE}?PageSpeedFilters=rewrite_css")
        assert_contains(out, r"link\srel=")
        assert_contains(out, re.escape(_EXTERNAL_CSS_HREF))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestOriginResponseHeaderDirectives:
    """rproxy./origin.rmcomments.example.com: origin configures the proxy."""

    def test_honor_response_header_directives_from_origin(self, vhost_client):
        """start_test Honor response header direcives from origin"""
        response = vhost_client("rproxy.rmcomments.example.com").get(
            "/mod_pagespeed_example/remove_comments.html")
        out = _wget_dump(require_status_ok(response, "proxied remove_comments.html"))
        assert_contains(out, "remove_comments example")
        assert_not_contains(out, "This comment will be removed")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
