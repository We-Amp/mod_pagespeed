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

"""Options embedded in rewritten resource URLs, before and after a flush.

Ported from: pagespeed/system/system_tests/embed_config.sh

embed-config-html.example.org sets the image quality; the resources live
on embed-config-resources.example.com, which sets none, so the quality
must travel inside the rewritten URLs -- and still be decoded from them
when the cache is empty. The size ceilings bound the body, where the bash
measured the saved file including its headers.
"""

import re
from urllib.parse import urlsplit

import pytest

from pagespeed_test_framework import assert_file_size, require_match, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines


def _host_and_path(url: str):
    parts = urlsplit(url)
    return parts.hostname, parts.path + (f"?{parts.query}" if parts.query else "")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "cache_flush")
class TestEmbedConfig:
    """Bash: Embed image configuration in rewritten image URL / decoding with
    clear cache."""

    def test_embedded_configuration_survives_a_cache_flush(self, vhost_client, flush_cache):
        page = vhost_client("embed-config-html.example.org").fetch_until(
            "/embed_config.html",
            condition=lambda r: count_matching_lines(r.text, re.escape(".pagespeed.")) == 3,
            timeout=100.0,
        )

        def fetch(url):
            host, path = _host_and_path(url)
            response = vhost_client(host).get(path)
            require_status_ok(response, url)
            return response

        image_url = require_match(
            r'(http://[^"]*256x192xPuz[^"]*\.pagespeed\.[^"]*iq=[^"]*\.ic\.[^"]*)"', page,
            "image URL with embedded options").group(1)
        css_url = require_match(
            r'(http://[^"]*rewrite_css_images\.css\.pagespeed\.[^"]*\+ii\+[^"]*\+iq=[^"]*\.cf\.[^"]*)"',
            page, "CSS URL with embedded options").group(1)
        js_url = require_match(
            r'(http://[^"]*rewrite_javascript\.js\.pagespeed\.jm\.[^"]*\.js)"', page,
            "rewritten JS URL").group(1)

        image, css, js = fetch(image_url), fetch(css_url), fetch(js_url)
        assert_file_size(image, "-lt", 10000, image_url)
        assert_file_size(css, "-lt", 600, css_url)
        assert_file_size(js, "-lt", 500, js_url)

        css_image_url = require_match(
            r"(http://[^)\"' ]*iq=[0-9]*\.ic\.[^)\"' ]*\.jpg)", css, "image URL inside the CSS"
        ).group(1)
        css_image = fetch(css_image_url)
        assert "max-age=31536000" in css_image.header("Cache-Control"), css_image.raw_headers

        expected = {
            image_url: len(image.body),
            css_image_url: len(css_image.body),
            css_url: len(css.body),
        }
        flush_cache()
        for url, length in expected.items():
            host, path = _host_and_path(url)
            vhost_client(host).fetch_until(
                path,
                condition=lambda r, n=length: r.status == 200 and len(r.body) == n,
                timeout=100.0,
                detail_fn=lambda r, n=length: f"status={r.status} bytes={len(r.body)} expected={n}",
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
