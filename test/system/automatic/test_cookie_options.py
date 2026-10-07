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

"""Options set by cookies, gated per vhost.

Ported from: pagespeed/automatic/system_tests/cookie_options.sh

Name-based vhosts on the secondary port (setup_apache_test.sh,
configure_fixture_vhosts; blocks from install/debug.conf.template):
  options-by-cookies-enabled.example.com   AllowOptionsToBeSetByCookies true,
      PassThrough + collapse_whitespace, remove_comments disabled.
  options-by-cookies-disabled.example.com  AllowOptionsToBeSetByCookies false.
"""

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
_ADD_REMOVE_COMMENTS = {"Cookie": "PageSpeedFilters=%2bremove_comments"}
# The '+' must be encoded as %2b for the cookie parsing code to accept it.
_UNENCODED_PLUS = {"Cookie": "PageSpeedFilters=+remove_comments"}


def _wget_dump(response: Response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks grep this whole dump, headers included.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.headers.items()]
    return "\n".join(lines) + "\n\n" + response.text


def _dump(client, headers=None) -> str:
    response = client.get(_PAGE, headers=headers)
    return _wget_dump(require_status_ok(response, "forbidden.html"))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestCookieOptionsEnabled:
    """options-by-cookies-enabled.example.com: AllowOptionsToBeSetByCookies true."""

    def test_default_keeps_comments_collapses_whitespace(self, vhost_client):
        """start_test Cookie options on: by default comments not removed, whitespace is"""
        out = _dump(vhost_client(ENABLED))
        assert_contains(out, "<!--")
        assert_not_contains(out, "  ")

    def test_option_cookie_takes_effect(self, vhost_client):
        """start_test Cookie options on: set option by cookie takes effect"""
        out = _dump(vhost_client(ENABLED), _ADD_REMOVE_COMMENTS)
        assert_not_contains(out, "<!--")
        assert_not_contains(out, "  ")

    def test_invalid_cookie_has_no_effect(self, vhost_client):
        """start_test Cookie options on: invalid cookie does not take effect"""
        out = _dump(vhost_client(ENABLED), _UNENCODED_PLUS)
        assert_contains(out, "<!--")
        assert_not_contains(out, "  ")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestCookieOptionsDisabled:
    """options-by-cookies-disabled.example.com: AllowOptionsToBeSetByCookies false."""

    def test_default_keeps_comments_and_whitespace(self, vhost_client):
        """start_test Cookie options off: by default comments nor whitespace removed"""
        out = _dump(vhost_client(DISABLED))
        assert_contains(out, "<!--")
        assert_contains(out, "  ")

    def test_option_cookie_has_no_effect(self, vhost_client):
        """start_test Cookie options off: set option by cookie has no effect"""
        out = _dump(vhost_client(DISABLED), _ADD_REMOVE_COMMENTS)
        assert_contains(out, "<!--")
        assert_contains(out, "  ")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
