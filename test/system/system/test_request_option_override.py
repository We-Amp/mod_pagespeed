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

"""Request options honoured only with the RequestOptionOverride secret.

Ported from: pagespeed/system/system_tests/request_option_override.sh

request-option-override.example.com: RequestOptionOverride abc,
PassThrough + collapse_whitespace, remove_comments disabled.
"""

import pytest

from pagespeed_test_framework import (
    assert_contains,
    assert_not_contains,
    require_status_ok,
)
from pagespeed_test_framework.client import Response

HOST = "request-option-override.example.com"
_PAGE = "/mod_pagespeed_test/forbidden.html"
_QUERY = (
    "?ModPagespeed=on"
    "&ModPagespeedFilters=+collapse_whitespace,+remove_comments"
    "&PageSpeedRequestOptionOverride="
)


def _wget_dump(response: Response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks grep this whole dump, headers included.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.headers.items()]
    return "\n".join(lines) + "\n\n" + response.text


def _option_headers(secret: str) -> dict:
    return {
        "ModPagespeed": "on",
        "ModPagespeedFilters": "+collapse_whitespace,+remove_comments",
        "PageSpeedRequestOptionOverride": secret,
    }


def _dump(vhost_client, path, headers=None) -> str:
    response = vhost_client(HOST).get(path, headers=headers)
    return _wget_dump(require_status_ok(response, "forbidden.html"))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestRequestOptionOverride:

    def test_correct_secret_in_query(self, vhost_client):
        """start_test Request Option Override : Correct values are passed"""
        assert_not_contains(_dump(vhost_client, _PAGE + _QUERY + "abc"), "<!--")

    def test_incorrect_secret_in_query(self, vhost_client):
        """start_test Request Option Override : Incorrect values are passed"""
        assert_contains(_dump(vhost_client, _PAGE + _QUERY + "notabc"), "<!--")

    def test_correct_secret_in_headers(self, vhost_client):
        """start_test Request Option Override : Correct values are passed as headers"""
        assert_not_contains(
            _dump(vhost_client, _PAGE, _option_headers("abc")), "<!--")

    def test_incorrect_secret_in_headers(self, vhost_client):
        """start_test Request Option Override : Incorrect values are passed as headers"""
        assert_contains(
            _dump(vhost_client, _PAGE, _option_headers("notabc")), "<!--")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
