# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""IIS daemon lane: image variants carry the full Vary on the 200 and the 304.

An image served from the optimizer's cache is chosen by the format in Accept,
the device class in User-Agent, and the density hints in Sec-CH-DPR and
Save-Data, so its Vary must name all four. The composition is unit tested;
this checks the wire under IIS with the optimizer worker running.

A desktop, a phone, a Save-Data and a Sec-CH-DPR client each fetch the lane's
image until the worker's cache answers (the first responses come from the
module, not the cache). That 200 and the 304 for its validator must name
exactly Accept, Save-Data, User-Agent and Sec-CH-DPR. IIS's own compression
may add Accept-Encoding to either; nothing else is allowed.

Needs the lane fixtures rule_packs and daemon_arm (the daemon.example.com
virtual host with its worker running); other lanes skip it.
"""

import os

import pytest

from pagespeed_test_framework import VhostClient, require_status_ok

pytestmark = pytest.mark.iis_only

HOST = "daemon.example.com"
IMAGE_PATH = "/daemon_lane/lane.jpg"
# The worker searches for the smallest acceptable encoding, which takes a while.
IMAGE_WORKER_DEADLINE_S = 180.0
DESKTOP_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/130.0 Safari/537.36"
)
MOBILE_UA = (
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/130.0 Mobile Safari/537.36"
)
# WebP: a converted copy is always worth keeping for this photo, while a
# re-encoded JPEG may be declined by the quality check.
ACCEPT = "image/webp,*/*"
REQUIRED_VARY = {"accept", "save-data", "user-agent", "sec-ch-dpr"}
ALLOWED_EXTRA = {"accept-encoding"}

CLIENTS = {
    "desktop": {"Accept": ACCEPT, "User-Agent": DESKTOP_UA},
    "phone": {"Accept": ACCEPT, "User-Agent": MOBILE_UA},
    "save-data": {"Accept": ACCEPT, "User-Agent": DESKTOP_UA, "Save-Data": "on"},
    "sec-ch-dpr": {"Accept": ACCEPT, "User-Agent": DESKTOP_UA, "Sec-CH-DPR": "2"},
}


@pytest.fixture
def daemon_site(server_config) -> VhostClient:
    raw = os.environ.get("PAGESPEED_RULE_PACKS_PORT")
    if not raw:
        pytest.fail("lane fixture rule_packs is provided but PAGESPEED_RULE_PACKS_PORT is not set")
    return VhostClient(HOST, proxy_host=server_config.host, proxy_port=int(raw))


def _vary_tokens(response) -> set:
    return {
        t.strip().lower()
        for value in response.header_values("Vary")
        for t in value.split(",")
        if t.strip()
    }


def _assert_full_vary(tokens: set, what: str, response) -> None:
    assert REQUIRED_VARY <= tokens, (what, "missing", REQUIRED_VARY - tokens, dict(response.headers))
    assert tokens - REQUIRED_VARY <= ALLOWED_EXTRA, (
        what,
        "unexpected",
        tokens - REQUIRED_VARY - ALLOWED_EXTRA,
        dict(response.headers),
    )


@pytest.mark.requires_fixture("rule_packs", "daemon_arm")
@pytest.mark.parametrize("name", list(CLIENTS))
def test_image_variant_vary_on_200_and_304(daemon_site, name):
    headers = CLIENTS[name]
    # Age marks a response answered from the optimizer's cache.
    served = daemon_site.fetch_until(
        IMAGE_PATH,
        lambda r: r.status == 200 and bool(r.header("Age")) and bool(r.header("Vary")),
        timeout=IMAGE_WORKER_DEADLINE_S,
        interval=1.0,
        headers=headers,
        detail_fn=lambda r: f"status={r.status} headers={dict(r.headers)}",
    )
    require_status_ok(served, f"{HOST}{IMAGE_PATH} ({name})")
    tokens = _vary_tokens(served)
    _assert_full_vary(tokens, f"{name} 200", served)

    etag = served.header("ETag")
    assert etag, (name, dict(served.headers))
    revalidated = daemon_site.get(IMAGE_PATH, headers={**headers, "If-None-Match": etag})
    assert revalidated.status == 304, (name, revalidated.status, dict(revalidated.headers))
    _assert_full_vary(_vary_tokens(revalidated), f"{name} 304", revalidated)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
