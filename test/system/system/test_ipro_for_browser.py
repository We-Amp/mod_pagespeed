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

"""In-place optimization is request-independent.

Ported from: pagespeed/system/system_tests/ipro_for_browser.sh

Whatever the User-Agent and whatever the Accept header, an in-place optimized
image comes back the same for every client -- a JPEG recompressed as JPEG, a
synthetic PNG as PNG -- with no Vary: header and a public max-age, so a shared
cache can never hand one browser's variant to another. Conversion to WebP or
AVIF happens only on rewritten URLs, where the format is part of the URL.
"""

import re
from typing import Optional

import pytest

PHOTO = "/images/Puzzle.jpg"
SYNTHETIC = "/images/Cuppa.png"
IPRO_ETAG_PREFIX = 'W/"PSA-aj-'

IE9_UA = "Mozilla/5.0 (Windows; U; MSIE 9.0; WIndows NT 9.0; en-US))"
IE11_UA = "Mozilla/5.0 (Windows NT 6.1; WOW64; ***********; rv:11.0) like Gecko"
OLD_OPERA_UA = "Opera/9.80 (Windows NT 5.2; U; en) Presto/2.7.62 Version/11.01"
NEWER_OPERA_UA = "Opera/9.80 (Windows NT 6.0; U; en) Presto/2.8.99 Version/11.10"
CHROME_UA = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_9_1) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/32.0.1700.102 Safari/537.36"
)
FIREFOX_UA = (
    "Mozilla/5.0 (X11; U; Linux x86_64; zh-CN; rv:1.9.2.10) "
    "Gecko/20100922 Ubuntu/10.10 (maverick) Firefox/3.6.10"
)
NEW_OPERA_UA = "Opera/9.80 (Windows NT 6.0) Presto/2.12.388 Version/12.14"


def _case(ua_name: str, user_agent: Optional[str], accept: Optional[str],
          image: str, content_type: str):
    kind = "photo" if image == PHOTO else "synthetic"
    accept_name = f"accept-{accept}" if accept else "no-accept"
    return pytest.param(user_agent, accept, image, content_type,
                        id=f"{ua_name}-{accept_name}-{kind}")


CASES = [
    # Testing-only user agents ("None" = the client's default user agent).
    # "webp" / "webp-la" are literal User-Agent header values the module
    # recognizes as testing-only capability tokens (bash: --user-agent webp).
    _case("default-ua", None, None, PHOTO, "jpeg"),
    _case("webp", "webp", None, PHOTO, "jpeg"),
    _case("webp-la", "webp-la", None, PHOTO, "jpeg"),
    _case("default-ua", None, "webp", PHOTO, "jpeg"),
    _case("webp", "webp", "webp", PHOTO, "jpeg"),
    _case("webp-la", "webp-la", "webp", PHOTO, "jpeg"),
    _case("default-ua", None, None, SYNTHETIC, "png"),
    _case("webp", "webp", None, SYNTHETIC, "png"),
    _case("webp-la", "webp-la", None, SYNTHETIC, "png"),
    _case("default-ua", None, "webp", SYNTHETIC, "png"),
    _case("webp", "webp", "webp", SYNTHETIC, "png"),
    _case("webp-la", "webp-la", "webp", SYNTHETIC, "png"),
    # IE used to get Cache-Control: private instead of Vary: Accept. In-place
    # responses no longer vary at all, so IE gets the same public response.
    _case("ie9", IE9_UA, None, PHOTO, "jpeg"),
    _case("ie9", IE9_UA, None, SYNTHETIC, "png"),
    _case("ie11", IE11_UA, None, PHOTO, "jpeg"),
    _case("ie11", IE11_UA, None, SYNTHETIC, "png"),
    # Older Opera did not support webp; slightly newer sends the header.
    _case("old-opera", OLD_OPERA_UA, None, PHOTO, "jpeg"),
    _case("old-opera", OLD_OPERA_UA, None, SYNTHETIC, "png"),
    _case("newer-opera", NEWER_OPERA_UA, "webp", PHOTO, "jpeg"),
    _case("newer-opera", NEWER_OPERA_UA, "webp", SYNTHETIC, "png"),
] + [
    _case(name, ua, accept, image, content_type)
    for name, ua in (("chrome", CHROME_UA), ("firefox", FIREFOX_UA),
                     ("new-opera", NEW_OPERA_UA))
    for accept, image, content_type in ((None, PHOTO, "jpeg"),
                                        (None, SYNTHETIC, "png"),
                                        ("webp", PHOTO, "jpeg"),
                                        ("webp", SYNTHETIC, "png"))
]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestIproForBrowser:
    """In-place image responses never vary by client.

    Bash original: ipro_for_browser.sh test_ipro_for_browser_webp, whose
    dynamically built start_test title reads "In-place optimize for
    User-Agent:<ua>, [no accept|Accept:<accept>], <photo|synth>. Expect
    image/<type>, no vary, cacheable."
    """

    @pytest.mark.parametrize("user_agent, accept, image, content_type", CASES)
    def test_in_place_image_is_the_same_for_every_browser(
        self, vhost_client, user_agent, accept, image, content_type
    ):
        vhost = vhost_client("ipro-for-browser.example.com")
        if user_agent:
            vhost = vhost.with_user_agent(user_agent)
        headers = {"Accept": f"image/{accept}"} if accept else None
        response = vhost.fetch_until(
            image,
            lambda r: r.header("ETag").startswith(IPRO_ETAG_PREFIX),
            headers=headers,
            detail_fn=lambda r: f"status={r.status} ETag={r.header('ETag')!r}",
        )
        assert response.header("Content-Type").startswith(f"image/{content_type}"), (
            f"Content-Type [{response.header('Content-Type')}], expected image/{content_type}"
        )
        assert response.header_values("Vary") == [], (
            f"in-place response carries Vary: {response.header_values('Vary')}"
        )
        assert re.fullmatch(r"max-age=[0-9]*", response.header("Cache-Control")), (
            f"Cache-Control [{response.header('Cache-Control')}], expected max-age=N"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
