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

"""In-place responses are request-independent, down to the byte.

Ported from: pagespeed/automatic/system_tests/image_quality_and_response.sh

The same image is fetched with a range of user agents, Accept, Save-Data and
Via headers; the content type, Content-Length and body must never move, and
no response may carry Vary: or Cache-Control: private. Animated GIFs stay
untouched in place but must be just as request-independent. The three hosts
differ in their quality settings, so responses are compared within a host.
"""

import hashlib
import re
import time
from typing import Dict

import pytest

from pagespeed_test_framework import require_status_ok
from pagespeed_test_framework.client import _read_timeout_multiplier

IPRO_ETAG_PREFIX = 'W/"PSA-aj-'

HOSTS = [
    "ipro-for-browser-vary-on-auto.example.com",
    "ipro-for-browser.example.com",
    "ipro-for-browser-vary-on-none.example.com",
]
# (path, expected Content-Type, optimized in place)
IMAGES = [
    ("/images/Puzzle.jpg", "image/jpeg", True),          # JPEG, recompressed as JPEG
    ("/images/Cuppa.png", "image/png", True),            # synthetic PNG, stays PNG
    ("/images/BikeCrashIcn.png", "image/jpeg", True),    # photographic PNG -> JPEG for everyone
    ("/images/PageSpeedAnimationSmall.gif", "image/gif", False),  # animated GIF, left alone
]
# (user agent, sends Accept: image/webp)
AGENTS = [
    ("Mozilla*Android*Mobile*Chrome/44.*", True),
    ("iPhone*Safari/8536.25", False),
    ("Firefox/1.5", False),
]
CASES = [
    pytest.param(host, path, content_type, optimized,
                 id=f"{host.split('.')[0]}-{path.rsplit('/', 1)[1]}")
    for host in HOSTS
    for path, content_type, optimized in IMAGES
]


def poll_never_optimized(vhost, path: str, headers: Dict[str, str], seconds: float = 5.0):
    """Bash: fetch_until -expect_time_out ... 'grep -c W/"PSA-aj-' 1."""
    deadline = time.monotonic() + seconds * _read_timeout_multiplier()
    while True:
        response = vhost.get(path, headers=headers)
        require_status_ok(response, f"in-place fetch of {path}")
        assert not response.header("ETag").startswith(IPRO_ETAG_PREFIX), (
            f"{path} was optimized in place (ETag {response.header('ETag')!r}) "
            f"but must be left alone"
        )
        if time.monotonic() > deadline:
            return response
        time.sleep(0.1)


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
@pytest.mark.slow
@pytest.mark.timeout(600)
class TestIproRequestIndependence:
    """Bash original: image_quality_and_response.sh ipro_response_is_request_independent."""

    @pytest.mark.parametrize("host, path, content_type, expect_optimized", CASES)
    def test_every_client_gets_the_same_bytes(
        self, vhost_client, host, path, content_type, expect_optimized
    ):
        baseline = None
        for user_agent, accept_webp in AGENTS:
            vhost = vhost_client(host).with_user_agent(user_agent)
            for save_data in (True, False):
                for via in (True, False):
                    headers = {}
                    if accept_webp:
                        headers["Accept"] = "image/webp"
                    if save_data:
                        headers["Save-Data"] = "on"
                    if via:
                        headers["Via"] = "proxy"
                    label = f"{host}{path} UA={user_agent} {headers}"
                    if expect_optimized:
                        response = vhost.fetch_until(
                            path,
                            lambda r: r.header("ETag").startswith(IPRO_ETAG_PREFIX),
                            headers=headers,
                            detail_fn=lambda r: f"status={r.status} ETag={r.header('ETag')!r}",
                        )
                    else:
                        response = poll_never_optimized(vhost, path, headers)
                    assert response.header_values("Vary") == [], (
                        f"{label}: in-place response carries Vary: "
                        f"{response.header_values('Vary')}"
                    )
                    cache_control = " ".join(response.header_values("Cache-Control"))
                    assert not re.search("private", cache_control, re.IGNORECASE), (
                        f"{label}: Cache-Control [{cache_control}] is private"
                    )
                    result = (
                        response.header("Content-Type"),
                        response.header("Content-Length"),
                        hashlib.md5(response.body).hexdigest(),
                    )
                    assert result[0] == content_type, (
                        f"{label}: IPRO expected type {content_type}, got {result[0]}"
                    )
                    if baseline is None:
                        baseline = result
                    assert result == baseline, (
                        f"{label}: IPRO bytes not identical: {result} != first "
                        f"client's {baseline}"
                    )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
