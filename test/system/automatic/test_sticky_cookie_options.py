#!/usr/bin/env python3
# Copyright 2026 We-Amp B.V.
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

"""Sticky query parameters: option cookies granted only with the right token.

Ported from: pagespeed/automatic/system_tests/sticky_cookie_options.sh

options-by-cookies-enabled.example.com has StickyQueryParameters
sticky_secret; options-by-cookies-disabled.example.com allows no option
cookies at all.
"""

import re

import pytest

from pagespeed_test_framework import (
    assert_contains,
    assert_not_contains,
    require_status_ok,
)
from pagespeed_test_framework.client import Response

_PAGE = "/mod_pagespeed_test/forbidden.html"
ENABLED = "options-by-cookies-enabled.example.com"
DISABLED = "options-by-cookies-disabled.example.com"
_COMMENT = re.escape("<!-- This comment should not be deleted -->")
_WRONG_TOKEN = (
    "?PageSpeedStickyQueryParameters=wrong_secret"
    "&PageSpeedFilters=+remove_comments"
)
_RIGHT_TOKEN = (
    "?PageSpeedStickyQueryParameters=sticky_secret"
    "&PageSpeedFilters=+remove_comments"
)
_EXPIRE_TOKEN = "?PageSpeedStickyQueryParameters=off"
_EPOCH = "Expires=Thu, 01 Jan 1970"
# Every option cookie ends with this (response_headers.cc,
# SetQueryParamsAsCookies / ClearOptionCookies).
_COOKIE_SUFFIX = "; Path=/; HttpOnly"


def _wget_dump(response: Response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks grep this whole dump, headers included.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.headers.items()]
    return "\n".join(lines) + "\n\n" + response.text


def _get(client, params="", cookie=None) -> Response:
    headers = {"Cookie": cookie} if cookie else None
    return require_status_ok(client.get(_PAGE + params, headers=headers),
                              "forbidden.html")


def _set_cookie_count(response: Response) -> int:
    """Number of Set-Cookie headers (Response joins repeats with ', ')."""
    return response.header("Set-Cookie").count(_COOKIE_SUFFIX)


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestStickyCookieOptions:

    def test_initially_comments_kept(self, vhost_client):
        """start_test Sticky option cookies: initially remove_comments only"""
        out = _wget_dump(_get(vhost_client(ENABLED)))
        assert_contains(out, _COMMENT)
        assert_not_contains(out, "  ")
        assert_not_contains(out, "Cookie")

    def test_wrong_token_has_no_effect(self, vhost_client):
        """start_test Sticky option cookies: wrong token has no effect"""
        out = _wget_dump(_get(vhost_client(ENABLED), _WRONG_TOKEN))
        assert_not_contains(out, _COMMENT)
        assert_not_contains(out, "  ")
        assert_not_contains(out, "Set-Cookie")

    def test_sticky_cookie_lifecycle(self, vhost_client):
        """right token IS adhesive -> no token leaves option cookies untouched
        -> wrong token expires option cookies -> back to remove_comments only.
        """
        client = vhost_client(ENABLED)

        # start_test Sticky option cookies: right token IS adhesive
        granted = _get(client, _RIGHT_TOKEN)
        out = _wget_dump(granted)
        assert_not_contains(out, _COMMENT)
        assert_not_contains(out, "  ")
        assert_contains(out, re.escape("Set-Cookie: PageSpeedFilters=%2bremove_comments;"))
        assert _set_cookie_count(granted) == 1, (
            f"Expected exactly one Set-Cookie, got: {granted.header('Set-Cookie')!r}"
        )

        # start_test Sticky option cookies: no token leaves option cookies untouched
        cookie = granted.header("Set-Cookie").split(";", 1)[0]  # extract_cookies
        out = _wget_dump(_get(client, cookie=cookie))
        assert_not_contains(out, _COMMENT)
        assert_not_contains(out, "  ")
        assert_not_contains(out, "Set-Cookie")

        # start_test Sticky option cookies: wrong token expires option cookies
        expired = _get(client, _EXPIRE_TOKEN, cookie=cookie)
        out = _wget_dump(expired)
        assert_not_contains(out, _COMMENT)
        assert_not_contains(out, "  ")
        assert_contains(out, re.escape(f"Cookie: PageSpeedFilters; {_EPOCH}"))
        set_cookie = expired.header("Set-Cookie")
        # check [ -z "$COOKIES" ]: every Set-Cookie is an epoch expiry.
        assert set_cookie.count(_COOKIE_SUFFIX) == set_cookie.count(_EPOCH), (
            f"A live option cookie survived the expiry: {set_cookie!r}"
        )

        # start_test Sticky option cookies: back to remove_comments only
        out = _wget_dump(_get(client))
        assert_contains(out, _COMMENT)
        assert_not_contains(out, "  ")
        assert_not_contains(out, "Cookie")

    def test_sticky_query_sets_no_cookie_where_cookies_disabled(self, vhost_client):
        """Guard: no option cookie on a vhost that disallows them."""
        out = _wget_dump(_get(vhost_client(DISABLED), _RIGHT_TOKEN))
        assert_not_contains(out, "Set-Cookie")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
