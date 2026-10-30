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

"""nginx daemon lane: image variants served from the optimizer's cache.

An image served from the optimizer worker's cache is chosen by the format in
Accept, the device class in User-Agent and the density hints in Sec-CH-DPR and
Save-Data. This checks, on the wire under nginx with the worker running, that

  * each request class (a desktop that accepts WebP, a desktop that does not,
    a phone, a Save-Data client and a Sec-CH-DPR client) gets its own variant;
  * every response served from the cache, the stored original and the variants
    alike, names Accept, Save-Data, User-Agent and Sec-CH-DPR in Vary;
  * the 304 and the HEAD of a variant carry the same Vary as its 200;
  * the second fetch of a variant is answered from the cache again (same
    validator, the cache's Age header, and the daemon-served image counter
    moves while the fall-through counter does not);
  * a stylesheet names none of the device headers.

Needs the lane fixtures rule_packs and daemon_arm (the daemon.example.com
server with its worker running, PAGESPEED_DAEMON_LANE=1 or require); other
lanes skip it.
"""

import os
import time

import pytest

from pagespeed_test_framework import VhostClient, require_status_ok

pytestmark = [
    pytest.mark.nginx_only,
    pytest.mark.requires_fixture("rule_packs", "daemon_arm"),
]

HOST = "daemon.example.com"
IMAGE_PATH = "/daemon_lane/lane.jpg"
CSS_PATH = "/daemon_lane/lane.css"
# The worker searches for the smallest acceptable encoding, which takes a while.
WORKER_DEADLINE_S = 240.0
DESKTOP_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/130.0 Safari/537.36"
)
MOBILE_UA = (
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/130.0 Mobile Safari/537.36"
)
WEBP = "image/webp,*/*"
JPEG_ONLY = "image/jpeg"
SELECTION_VARY = {"accept", "save-data", "user-agent", "sec-ch-dpr"}
DEVICE_HEADERS = {"user-agent", "save-data", "sec-ch-dpr"}
# The ETag of a variant the worker built; the stored original keeps the
# origin's own validator.
VARIANT_ETAG_MARK = 'ps-'

CLIENTS = {
    "desktop": {"Accept": WEBP, "User-Agent": DESKTOP_UA},
    "desktop-no-webp": {"Accept": JPEG_ONLY, "User-Agent": DESKTOP_UA},
    "phone": {"Accept": WEBP, "User-Agent": MOBILE_UA},
    "save-data": {"Accept": WEBP, "User-Agent": DESKTOP_UA, "Save-Data": "on"},
    "sec-ch-dpr": {"Accept": WEBP, "User-Agent": DESKTOP_UA, "Sec-CH-DPR": "2"},
}


def _vary_tokens(response) -> set:
    return {
        t.strip().lower()
        for value in response.header_values("Vary")
        for t in value.split(",")
        if t.strip()
    }


def _is_variant(response) -> bool:
    return response.status == 200 and VARIANT_ETAG_MARK in response.header("ETag")


@pytest.fixture(scope="module")
def site(server_config) -> VhostClient:
    raw = os.environ.get("PAGESPEED_RULE_PACKS_PORT")
    if not raw:
        pytest.fail("lane fixture rule_packs is provided but PAGESPEED_RULE_PACKS_PORT is not set")
    return VhostClient(HOST, proxy_host=server_config.host, proxy_port=int(raw))


@pytest.fixture(scope="module")
def served(site):
    """The settled variant of each request class, and every response on the way
    there that the cache answered (the stored original included).

    The worker builds a general variant first and the class-specific ones
    (phone, Save-Data, Sec-CH-DPR) later, so a class has settled once its
    variant differs from every other class's.
    """
    seen = []
    variants = {}
    deadline = time.time() + WORKER_DEADLINE_S
    while True:
        for name, headers in CLIENTS.items():
            response = site.get(IMAGE_PATH, headers=headers)
            require_status_ok(response, f"{HOST}{IMAGE_PATH} ({name})")
            if response.header("Age"):
                seen.append((name, response))
            if _is_variant(response):
                variants[name] = response
        etags = [r.header("ETag") for r in variants.values()]
        if len(variants) == len(CLIENTS) and len(set(etags)) == len(etags):
            return variants, seen
        if time.time() >= deadline:
            pytest.fail(
                f"the request classes did not settle on distinct variants within {WORKER_DEADLINE_S}s; "
                f"etags: { {n: r.header('ETag') for n, r in variants.items()} }"
            )
        time.sleep(2.0)


def test_each_request_class_gets_its_own_variant(served):
    variants, _ = served
    assert variants["desktop"].header("Content-Type") == "image/webp", dict(variants["desktop"].headers)
    assert variants["phone"].header("Content-Type") == "image/webp", dict(variants["phone"].headers)
    assert variants["desktop-no-webp"].header("Content-Type") == "image/jpeg", dict(
        variants["desktop-no-webp"].headers
    )
    etags = {name: r.header("ETag") for name, r in variants.items()}
    assert len(set(etags.values())) == len(etags), etags
    assert len({r.body for r in variants.values()}) >= 4, {n: len(r.body) for n, r in variants.items()}
    # The device and density hints change the bytes, not only the validator.
    assert len(variants["phone"].body) < len(variants["desktop"].body)
    assert len(variants["save-data"].body) < len(variants["desktop"].body)


def test_every_cache_served_response_names_the_selection_headers(served):
    variants, seen = served
    assert seen, "the cache answered nothing"
    for name, response in seen:
        assert SELECTION_VARY <= _vary_tokens(response), (name, dict(response.headers))
    for name, response in variants.items():
        assert SELECTION_VARY <= _vary_tokens(response), (name, dict(response.headers))


@pytest.mark.parametrize("name", list(CLIENTS))
def test_304_and_head_carry_the_vary_of_the_200(site, served, name):
    variants, _ = served
    headers = CLIENTS[name]
    variant = variants[name]
    tokens = _vary_tokens(variant)
    etag = variant.header("ETag")

    revalidated = site.get(IMAGE_PATH, headers={**headers, "If-None-Match": etag})
    assert revalidated.status == 304, (name, revalidated.status, dict(revalidated.headers))
    assert _vary_tokens(revalidated) == tokens, (name, dict(revalidated.headers))

    head = site.head(IMAGE_PATH, headers=headers)
    assert head.status == 200, (name, head.status, dict(head.headers))
    assert head.body == b"", (name, len(head.body))
    assert _vary_tokens(head) == tokens, (name, dict(head.headers))


@pytest.mark.parametrize("name", list(CLIENTS))
def test_second_fetch_is_served_from_the_cache(site, server_config, served, name):
    variants, _ = served
    first = variants[name]
    stats_path = server_config.stats_path

    def stats():
        return site.get_statistics(stats_path=stats_path, disable_pagespeed=False)

    before = stats()
    again = site.get(IMAGE_PATH, headers=CLIENTS[name])
    after = stats()

    require_status_ok(again, f"{HOST}{IMAGE_PATH} ({name})")
    assert again.header("ETag") == first.header("ETag"), (name, dict(again.headers))
    assert again.body == first.body, name
    assert again.header("Age"), (name, dict(again.headers))
    assert int(again.header("Age")) >= int(first.header("Age")), (name, dict(again.headers))
    assert not again.header("X-Page-Speed") and not again.header("X-Mod-Pagespeed"), dict(again.headers)
    assert after.get("ipro_daemon_served_image", 0) - before.get("ipro_daemon_served_image", 0) == 1, (
        name,
        before,
        after,
    )
    assert after.get("ipro_daemon_fallthrough", 0) == before.get("ipro_daemon_fallthrough", 0), name


def test_a_stylesheet_names_no_device_header(site):
    response = site.fetch_until(
        CSS_PATH,
        lambda r: r.status == 200 and bool(r.header("Age")),
        timeout=60.0,
        interval=1.0,
        detail_fn=lambda r: f"status={r.status} headers={dict(r.headers)}",
    )
    require_status_ok(response, f"{HOST}{CSS_PATH}")
    assert not _vary_tokens(response) & DEVICE_HEADERS, dict(response.headers)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
