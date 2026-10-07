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

"""UrlValuedAttribute: configured attributes are rewritten like built-in ones.

Ported from: pagespeed/system/system_tests/url_valued_attributes.sh

url-attribute.example.com maps src.example.com -> dst.example.com and
declares extra url-valued attributes (span src, hr imgsrc, custom a/b/c,
img alt-src, video alt-a/alt-b, link/span data-stylesheet*, blockquote cite).
"""

import re

import pytest

from pagespeed_test_framework import assert_contains, require_status_ok
from pagespeed_test_framework.client import Response

HOST = "url-attribute.example.com"
_TEST = "/mod_pagespeed_test"
_REWRITE_DOMAINS = f"{_TEST}/rewrite_domains.html"
_UVA_EXTEND_CACHE = (
    f"{_TEST}/url_valued_attribute_extend_cache.html"
    "?PageSpeedFilters=core,+left_trim_urls"
)
_UVA_CSS = (
    f"{_TEST}/url_valued_attribute_css.html"
    "?PageSpeedFilters=debug,combine_css,rewrite_css,inline_css"
)


def _wget_dump(response: Response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks grep this whole dump, headers included.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.headers.items()]
    return "\n".join(lines) + "\n\n" + response.text


def _lines_containing(text: str, needle: str) -> int:
    """Bash: fgrep -c (counts lines)."""
    return sum(1 for line in text.splitlines() if needle in line)


def _lines_matching(text: str, pattern: str) -> int:
    """Bash: grep -c (counts lines)."""
    return sum(1 for line in text.splitlines() if re.search(pattern, line))


def _fetch_until(client, path, measure, expected, what) -> Response:
    return client.fetch_until(
        path,
        lambda r: measure(r.text) == expected,
        detail_fn=lambda r: f"{what}={measure(r.text)} expected={expected}",
    )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestUrlValuedAttributes:

    def test_rewrite_domains_in_dynamic_attributes(self, vhost_client):
        """start_test Rewrite domains in dynamically defined url-valued attributes."""
        out = _wget_dump(require_status_ok(
            vhost_client(HOST).get(_REWRITE_DOMAINS), "rewrite_domains.html"))
        assert _lines_containing(out, "http://dst.example.com") == 6, out
        assert _lines_containing(
            out, "<hr src=http://src.example.com/hr-image>") == 1, out

    def test_additional_attributes_fully_respected(self, vhost_client):
        """start_test Additional url-valued attributes are fully respected."""
        client = vhost_client(HOST)
        # There are ten resources that should be optimized.
        _fetch_until(client, _UVA_EXTEND_CACHE,
                     lambda t: t.count(".pagespeed."), 10, "'.pagespeed.' count")
        # <custom d=...> isn't modified at all; everything else is rewritten
        # from ../foo to /foo, so one reference to ../mod_pagespeed remains.
        _fetch_until(client, _UVA_EXTEND_CACHE,
                     lambda t: _lines_matching(t, r"d=.[.][.]/mod_pa"), 1,
                     "lines matching d=.[.][.]/mod_pa")
        _fetch_until(client, _UVA_EXTEND_CACHE,
                     lambda t: _lines_containing(t, "../mod_pa"), 1,
                     "lines containing ../mod_pa")
        # There are ten images that should be optimized.
        _fetch_until(client, _UVA_EXTEND_CACHE,
                     lambda t: t.count(".pagespeed.ic"), 10, "'.pagespeed.ic' count")

    def test_stylesheet_attributes_handled(self, vhost_client):
        """start_test url-valued stylesheet attributes are properly handled"""

        def complete(text: str) -> bool:
            return (text.count(".pagespeed.cf.") == 7
                    and "<style>.bold{font-weight:bold}</style>" in text)

        response = vhost_client(HOST).fetch_until(
            _UVA_CSS, lambda r: complete(r.text),
            detail_fn=lambda r: (
                f".pagespeed.cf. count={r.text.count('.pagespeed.cf.')} expected=7, "
                f"bold.css inlined={'<style>.bold{font-weight:bold}</style>' in r.text}"),
        )
        out = response.text
        assert_contains(
            out, "Could not combine over barrier: custom or alternate stylesheet attribute")
        assert_contains(out, r"link data-stylesheet=[^<]*.pagespeed.cf")
        assert_contains(
            out,
            r"<span data-stylesheet-a=[^<]*.pagespeed.cf"
            r"[^<]*data-stylesheet-b=[^<]*.pagespeed.cf"
            r"[^<]*data-stylesheet-c=[^<]*.pagespeed.cf")
        assert_contains(out, r"<link rel=invalid data-stylesheet=[^<]*.pagespeed.cf")
        assert_contains(
            out, r"<style data-stylesheet=[^<]*.pagespeed.cf[^>]*>.bold\{font-weight:bold\}")
        assert "blue.css+yellow.css" not in out, "blue.css and yellow.css were combined"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
