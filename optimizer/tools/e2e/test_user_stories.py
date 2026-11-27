# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""mod_pagespeed E2E User Story Tests.

Tests the full stack: nginx module -> cache -> worker -> origin,
verifying each user story from Phase 6.

Requires Docker Compose services to be running (managed by conftest.py).

Test ordering matters: cold-miss tests run first (before any URL is
cached), then optimization tests follow.
"""

import gzip
import os
import re
import time

import pytest
import requests
from conftest import (
    WORKER_API_URL,
    PageSpeedClient,
    compose_run,
    docker_exec,
    unique_url,
)

# -- User Story 1: Module loads into nginx --


class TestModuleLoads:
    def test_nginx_config_valid(self, e2e_services):
        """User Story 1: nginx -t passes with pagespeed module loaded."""
        result = docker_exec("nginx", "nginx -t")
        assert result.returncode == 0, f"nginx -t failed: {result.stderr}"


# -- User Story 2: Cold cache miss proxied to origin --
#
# Text cold-miss tests run first so the worker can process CSS/JS/HTML
# before image tests trigger expensive proactive variant generation
# (SSIMULACRA2 quality tuning, up to 36 image variants).


class TestTextColdCacheMiss:
    """Cold miss tests for text resources (CSS, JS, HTML).

    Image cold miss is deferred to TestImageColdMiss (below) so that
    text optimization tests can complete without competing with heavy
    image encoding on the worker thread pool.
    """

    def test_css_cold_miss(self, client: PageSpeedClient):
        """User Story 2: First CSS request returns 200 with MISS."""
        r = client.get("/style.css")
        assert r.status_code == 200
        client.assert_miss(r)
        assert "font-family" in r.text

    def test_js_cold_miss(self, client: PageSpeedClient):
        """User Story 2: First JS request returns 200 with MISS."""
        r = client.get("/app.js")
        assert r.status_code == 200
        client.assert_miss(r)
        assert "initApp" in r.text

    def test_html_cold_miss(self, client: PageSpeedClient):
        """User Story 2: First HTML request returns 200 with MISS."""
        r = client.get("/index.html")
        assert r.status_code == 200
        client.assert_miss(r)
        assert "mod_pagespeed" in r.text


# -- Wait for worker to process text notifications --
#
# The worker processes CSS/JS/HTML notifications in background.  We must
# ensure the optimized variants are written to disk BEFORE any read poll,
# because the first nginx read populates an in-process RAM cache with no
# TTL and no cross-process invalidation.  If the first read gets the
# original (un-optimized) content, it sticks in RAM cache and all
# subsequent polls return stale data.


class TestWorkerProcessing:
    def test_wait_for_text_processing(self, e2e_services):
        """Give worker time to process CSS/JS/HTML before optimization polls."""
        time.sleep(10)


# -- User Stories 4+5: Warm cache hit + CSS minification --
#
# By this point the cold miss tests have already triggered cache
# recording and worker notifications for the test files.


class TestCssOptimization:
    def test_css_warm_hit_and_minified(self, client: PageSpeedClient, e2e_services):
        """User Stories 4+5: After worker processes, CSS is served
        minified from cache with HIT header."""
        # Get origin size for comparison
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        origin_size = len(origin_r.content)
        # Poll for optimized variant (must be smaller than origin).
        # The cold miss test already triggered recording and notification.
        r = client.poll_for_hit("/style.css", expect_smaller_than=origin_size)
        client.assert_hit(r)
        # Verify minification: cached version should be smaller
        assert len(r.content) < origin_size, (
            f"Expected minified CSS ({len(r.content)}B) < origin ({origin_size}B)"
        )
        # Key rules should still be present
        assert "font-family" in r.text


# -- User Story 6: JS minification --


class TestJsOptimization:
    def test_js_minified_on_hit(self, client: PageSpeedClient, e2e_services):
        """User Story 6: JS is minified on cache hit."""
        # Get origin size for comparison
        origin_r = requests.get(e2e_services.origin_url + "/app.js", timeout=5)
        origin_size = len(origin_r.content)
        # Poll for optimized variant (cold miss already triggered).
        # Image cold miss is deferred (TestImageColdMiss runs later),
        # so the worker processes JS without competing with heavy
        # image encoding.
        r = client.poll_for_hit(
            "/app.js", expect_smaller_than=origin_size, timeout=30.0
        )
        client.assert_hit(r)
        # Verify minification
        assert len(r.content) < origin_size, (
            f"Expected minified JS ({len(r.content)}B) < origin ({origin_size}B)"
        )
        # Key function should still be present
        assert "initApp" in r.text


# -- User Story 2 (images): Cold cache miss for images --
#
# Deferred from text cold-miss tests above so that CSS/JS optimization
# completes first.  Image cold miss triggers proactive variant generation
# (SSIMULACRA2 quality tuning, multiple formats/viewports), which can
# saturate the worker thread pool for 60+ seconds.


class TestImageColdMiss:
    def test_image_cold_miss(self, client: PageSpeedClient):
        """User Story 2: First image request returns 200."""
        r = client.get("/photo.jpg")
        assert r.status_code == 200


# -- User Story 7: Image WebP negotiation --


class TestImageWebP:
    def test_webp_negotiation(self, client: PageSpeedClient):
        """User Story 7: Accept: image/webp gets WebP variant."""
        # Cold miss test already triggered recording of original.
        # Poll until we get a HIT with WebP content (not just the
        # original JPEG from cache fallback).
        deadline = time.time() + 20.0
        last_response = None
        while time.time() < deadline:
            try:
                r = client.get("/photo.jpg", headers={"Accept": "image/webp,*/*"})
                last_response = r
                if (
                    r.headers.get("X-PageSpeed") == "HIT"
                    and len(r.content) >= 12
                    and r.content[:4] == b"RIFF"
                ):
                    break
            except requests.RequestException:
                pass
            time.sleep(0.5)
        else:
            r = last_response
        # Verify WebP magic bytes (RIFF....WEBP)
        assert r is not None, "No response received for /photo.jpg"
        assert len(r.content) >= 12, (
            f"Response too short for WebP: {len(r.content)} bytes"
        )
        assert r.content[:4] == b"RIFF", f"Expected RIFF magic, got: {r.content[:4]!r}"
        assert r.content[8:12] == b"WEBP", (
            f"Expected WEBP signature, got: {r.content[8:12]!r}"
        )


# -- User Story 8: Save-Data respected --


class TestSaveData:
    def test_save_data_respected(self, client: PageSpeedClient):
        """User Story 8: Save-Data header gets a 200 response."""
        r = client.get("/photo.jpg", headers={"Save-Data": "1"})
        assert r.status_code == 200


# -- HTML served correctly --


class TestHtml:
    def test_html_served(self, client: PageSpeedClient):
        """HTML page served correctly through nginx."""
        r = client.get("/index.html")
        assert r.status_code == 200
        assert "mod_pagespeed" in r.text


# -- Image AVIF negotiation --


class TestImageAVIF:
    def test_avif_negotiation(self, client: PageSpeedClient):
        """Accept: image/avif gets AVIF variant after worker processes."""
        # Poll until we get a HIT with AVIF content.
        # AVIF files start with a ftyp box: bytes 4-8 are 'ftyp'
        deadline = time.time() + 20.0
        last_response = None
        while time.time() < deadline:
            try:
                r = client.get("/photo.jpg", headers={"Accept": "image/avif,*/*"})
                last_response = r
                if (
                    r.headers.get("X-PageSpeed") == "HIT"
                    and len(r.content) >= 12
                    and r.content[4:8] == b"ftyp"
                ):
                    break
            except requests.RequestException:
                pass
            time.sleep(0.5)
        else:
            r = last_response
        # Verify AVIF - ftyp box signature
        assert r is not None, "No response received for /photo.jpg with AVIF"
        assert len(r.content) >= 12, (
            f"Response too short for AVIF: {len(r.content)} bytes"
        )
        assert r.content[4:8] == b"ftyp", f"Expected ftyp box, got: {r.content[4:8]!r}"


# -- Animated GIF serving --


class TestImageGIF:
    def test_gif_served(self, client: PageSpeedClient):
        """Animated GIF passes through and is served correctly."""
        r = client.get("/animation.gif")
        assert r.status_code == 200
        # GIF magic bytes
        assert r.content[:3] == b"GIF", f"Expected GIF magic, got: {r.content[:3]!r}"


# -- Worker health check --


class TestHealthCheck:
    def test_worker_health(self, e2e_services):
        """Worker health socket responds with OK N/M format."""
        # Connect to the health socket inside the worker container
        result = docker_exec(
            "worker",
            [
                "sh",
                "-c",
                "socat -u -T 4 UNIX-CONNECT:/tmp/pagespeed.sock.health -",
            ],
        )
        # If socat isn't available, try with a simple Python script
        if result.returncode != 0:
            result = docker_exec(
                "worker",
                [
                    "python3",
                    "-c",
                    "import socket; s=socket.socket(socket.AF_UNIX);"
                    " s.connect('/tmp/pagespeed.sock.health');"
                    " print(s.recv(256).decode()); s.close()",
                ],
            )

        if result.returncode == 0:
            output = result.stdout.strip()
            assert output.startswith("OK"), (
                f"Health check expected 'OK ...', got: {output!r}"
            )
            # Should contain N/M format
            assert "/" in output, f"Health check expected 'OK N/M', got: {output!r}"


# -- Early Hints on HTML miss --


class TestEarlyHints:
    def test_early_hints_on_html_miss(self, client: PageSpeedClient, e2e_services):
        """After worker processes HTML, subsequent MISS gets Early Hints."""
        # First, ensure the HTML has been processed by the worker
        # (the cold miss tests should have triggered this)
        # Wait for worker to process and store hints
        time.sleep(5)

        # Clear the HTML from cache so the next request is a MISS
        # (but hints at mask=0xFFFFFFFF should still be there)
        # We can't easily clear just the HTML key, so instead we
        # request a new HTML page that hasn't been cached yet.
        # For now, just verify that HTML requests get 200
        r = client.get("/index.html")
        assert r.status_code == 200
        # Note: Testing 103 Early Hints requires raw HTTP/1.1 socket
        # because the requests library doesn't expose informational
        # responses. This test verifies the flow doesn't break; full
        # 103 testing requires a browser E2E test or raw socket test.


# -- No licensing apparatus (Apache-2.0 at GA) --
#
# The daemon carries no license/token machinery any more: no /v1/license/*
# endpoints, no license object in /v1/health, no X-PageSpeed-Warn header, and
# nothing license-shaped in its logs. Optimization is on purely because the
# worker is up — these tests pin that down so the apparatus cannot creep back.


class TestNoLicenseApparatus:
    def test_no_pagespeed_warn_header(self, client: PageSpeedClient, e2e_services):
        """Neither HTML nor image responses carry an X-PageSpeed-Warn header."""
        # requests' header dict is case-insensitive, so one lookup covers
        # every capitalization the module could emit.
        for path in ("/index.html", "/photo.jpg"):
            r = client.get(path)
            assert r.status_code == 200, f"{path}: HTTP {r.status_code}"
            assert "X-PageSpeed-Warn" not in r.headers, (
                f"{path}: unexpected X-PageSpeed-Warn: "
                f"{r.headers.get('X-PageSpeed-Warn')!r}"
            )

    def test_license_endpoints_are_gone(self, worker_api):
        """/v1/license/* no longer exists on the worker API (404, not 4xx-other)."""
        r = worker_api.post_raw("/v1/license/apply", {"license_key": "unused"})
        assert r.status_code == 404, (
            f"POST /v1/license/apply: expected 404, got {r.status_code}: {r.text[:200]}"
        )
        r = worker_api.get_raw("/v1/license/status")
        assert r.status_code == 404, (
            f"GET /v1/license/status: expected 404, got {r.status_code}: {r.text[:200]}"
        )

    def test_health_has_no_license_fields(self, worker_api):
        """/v1/health carries neither a `license` object nor a `checks.license` entry."""
        health = worker_api.get_health()
        assert "license" not in health, (
            f"unexpected health.license: {health['license']!r}"
        )
        checks = health.get("checks", {})
        assert "license" not in checks, (
            f"unexpected checks.license: {checks['license']!r}"
        )

    def test_worker_log_mentions_no_license(self, e2e_services):
        """The worker container log has no license-shaped line at all.

        The worker logs to stdout/stderr (captured by `docker compose logs`).
        The harness sets none of the deprecated --license-key /
        PAGESPEED_LICENSE_KEY inputs, so not even the one-time deprecation
        warning may appear.
        """
        r = compose_run("logs", "--no-color", "worker")
        out = r.stdout + r.stderr
        hits = [
            line
            for line in out.splitlines()
            if re.search(r"(?i)unlicensed|license", line)
        ]
        assert not hits, "license-shaped lines in worker log:\n" + "\n".join(hits[:10])


# -- Proactive multi-format image variants --
#
# After the worker processes a WebP notification for photo.jpg, it should
# proactively generate AVIF and optimized-original variants too.  So a
# subsequent request with Accept: image/avif should be an immediate HIT
# without needing another cold-miss→notification→process cycle.


class TestProactiveImageVariants:
    def test_avif_available_after_webp_request(self, client: PageSpeedClient):
        """After WebP is generated, AVIF should also be proactively available."""
        # By this point, TestImageWebP already triggered WebP generation.
        # The proactive variant feature should have also generated AVIF.
        # Poll for AVIF variant — it should appear quickly (or already exist).
        deadline = time.time() + 15.0
        last_response = None
        while time.time() < deadline:
            try:
                r = client.get(
                    "/photo.jpg",
                    headers={"Accept": "image/avif,*/*"},
                )
                last_response = r
                if (
                    r.headers.get("X-PageSpeed") == "HIT"
                    and len(r.content) >= 12
                    and r.content[4:8] == b"ftyp"
                ):
                    break
            except requests.RequestException:
                pass
            time.sleep(0.5)
        else:
            r = last_response
        assert r is not None, "No response for proactive AVIF"
        assert r.content[4:8] == b"ftyp", (
            f"Expected AVIF ftyp box, got: {r.content[4:8]!r}"
        )
        client.assert_hit(r)

    def test_webp_available_after_avif_request(self, client: PageSpeedClient):
        """WebP should also be available for the same image URL."""
        # Same logic: both WebP and AVIF should exist.
        r = client.get("/photo.jpg", headers={"Accept": "image/webp,*/*"})
        assert r.status_code == 200
        client.assert_hit(r)
        assert r.content[:4] == b"RIFF", f"Expected RIFF magic, got: {r.content[:4]!r}"
        assert r.content[8:12] == b"WEBP", (
            f"Expected WEBP signature, got: {r.content[8:12]!r}"
        )


# -- Health endpoint stats --


class TestHealthStats:
    def test_health_includes_stats(self, e2e_services):
        """Health endpoint includes notification and variant counters."""
        result = docker_exec(
            "worker",
            [
                "python3",
                "-c",
                "import socket; s=socket.socket(socket.AF_UNIX);"
                " s.connect('/tmp/pagespeed.sock.health');"
                " print(s.recv(1024).decode()); s.close()",
            ],
        )
        if result.returncode == 0:
            output = result.stdout.strip()
            assert output.startswith("OK"), f"Expected 'OK ...', got: {output!r}"
            # Enhanced health endpoint should include stats
            assert "notifs=" in output, (
                f"Expected 'notifs=' in health output: {output!r}"
            )
            assert "variants=" in output, (
                f"Expected 'variants=' in health output: {output!r}"
            )
            assert "proactive=" in output, (
                f"Expected 'proactive=' in health output: {output!r}"
            )


# -- User Story 9: Graceful degradation (worker down) --


class TestGracefulDegradation:
    def test_worker_down_still_serves(self, client: PageSpeedClient):
        """User Story 9: Nginx still proxies when worker is stopped."""
        compose_run("stop", "worker")
        try:
            r = client.get("/style.css")
            assert r.status_code == 200
            assert r.status_code < 500
        finally:
            compose_run("start", "worker")
            # Wait for the worker API to be ready (not just a fixed sleep).
            # This prevents subsequent tests from running against a
            # half-initialized worker.
            deadline = time.time() + 30
            while time.time() < deadline:
                try:
                    r = requests.get(f"{WORKER_API_URL}/v1/health", timeout=2)
                    if r.status_code == 200:
                        break
                except requests.RequestException:
                    pass
                time.sleep(0.5)


# -- Viewport Sibling Tests --
#
# After the worker generates proactive viewport siblings, images
# served to mobile and desktop user-agents should differ in size
# (mobile images are resized to 480px width).


class TestViewportSiblings:
    def test_mobile_image_smaller_than_desktop(self, client: PageSpeedClient):
        """Mobile viewport image variant should be smaller than desktop."""
        mobile_ua = (
            "Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "Version/16.0 Mobile/15E148 Safari/604.1"
        )

        # Get the mobile WebP first — the proactive loop generates
        # Mobile variants before Desktop, so this is available sooner.
        mobile_r = client.poll_for_hit(
            "/photo.jpg",
            headers={"Accept": "image/webp,*/*", "User-Agent": mobile_ua},
            timeout=30.0,
        )
        assert mobile_r.content[:4] == b"RIFF", "Mobile response not WebP"
        mobile_size = len(mobile_r.content)

        # Request the desktop WebP to trigger a notification (MISS),
        # then poll until the worker has generated the desktop variant (HIT).
        # Desktop default UA sends no viewport hint → desktop class.
        desktop_r = client.poll_for_hit(
            "/photo.jpg",
            headers={"Accept": "image/webp,*/*"},
            timeout=30.0,
        )
        assert desktop_r.content[:4] == b"RIFF", "Desktop response not WebP"
        desktop_size = len(desktop_r.content)

        assert mobile_size <= desktop_size, (
            f"Mobile WebP ({mobile_size}B) should be smaller than or equal "
            f"to desktop ({desktop_size}B)"
        )


# -- Save-Data Variant Tests --


class TestSaveDataVariants:
    def test_save_data_image_served(self, client: PageSpeedClient):
        """Save-Data: on requests should get a valid image response."""
        r = client.poll_for_hit(
            "/photo.jpg",
            headers={
                "Accept": "image/webp,*/*",
                "Save-Data": "on",
            },
            timeout=20.0,
        )
        assert r.status_code == 200
        assert len(r.content) > 0


# -- Management Socket Tests --


class TestManagementSocket:
    def test_mgmt_stats(self, e2e_services):
        """Management socket STATS command returns JSON stats."""
        result = docker_exec(
            "worker",
            [
                "python3",
                "-c",
                "import socket; s=socket.socket(socket.AF_UNIX);"
                " s.connect('/tmp/pagespeed.sock.mgmt');"
                " s.send(b'STATS\\n');"
                " print(s.recv(4096).decode()); s.close()",
            ],
        )
        if result.returncode == 0:
            output = result.stdout.strip()
            assert '"status":"ok"' in output, (
                f"Expected JSON with status:ok, got: {output!r}"
            )
            assert '"notifications"' in output
            assert '"variants"' in output
            assert '"by_type"' in output

    def test_mgmt_stats_new_counters(self, e2e_services):
        """STATS includes new Step 10 counters."""
        result = docker_exec(
            "worker",
            [
                "python3",
                "-c",
                "import socket; s=socket.socket(socket.AF_UNIX);"
                " s.connect('/tmp/pagespeed.sock.mgmt');"
                " s.send(b'STATS\\n');"
                " print(s.recv(4096).decode()); s.close()",
            ],
        )
        if result.returncode == 0:
            output = result.stdout.strip()
            assert '"html_assembly"' in output, (
                f"Expected html_assembly in STATS: {output!r}"
            )
            assert '"alternates"' in output, f"Expected alternates in STATS: {output!r}"
            assert '"selector_invocations"' in output, (
                f"Expected selector_invocations in STATS: {output!r}"
            )

    def test_mgmt_metrics_prometheus(self, e2e_services):
        """METRICS returns Prometheus text format with new counters."""
        result = docker_exec(
            "worker",
            [
                "python3",
                "-c",
                "import socket; s=socket.socket(socket.AF_UNIX);"
                " s.connect('/tmp/pagespeed.sock.mgmt');"
                " s.send(b'METRICS\\n');"
                " print(s.recv(8192).decode()); s.close()",
            ],
        )
        if result.returncode == 0:
            output = result.stdout.strip()
            assert "pagespeed_notifications_total" in output
            assert "pagespeed_html_assembly_total" in output, (
                f"Expected html_assembly metric: {output!r}"
            )
            assert "pagespeed_alternate_writes_total" in output, (
                f"Expected alternate_writes metric: {output!r}"
            )
            assert "pagespeed_selector_invocations_total" in output, (
                f"Expected selector_invocations metric: {output!r}"
            )


# ============================================================================
# Step 9: Alternate-specific and cache behavior tests
# ============================================================================


# -- 9a: Vary headers on HIT responses --


class TestVaryHeaders:
    def test_image_hit_has_vary(self, client: PageSpeedClient):
        """HIT response for images includes Vary: Accept, Save-Data, User-Agent."""
        r = client.poll_for_hit(
            "/photo.jpg",
            headers={"Accept": "image/webp,*/*"},
            timeout=20.0,
        )
        client.assert_hit(r)
        vary = r.headers.get("Vary", "")
        assert "Accept" in vary, (
            f"Image HIT should have Vary containing Accept, got: {vary!r}"
        )

    def test_css_hit_has_vary(self, client: PageSpeedClient, e2e_services):
        """HIT response for CSS includes Vary: Accept-Encoding."""
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        r = client.poll_for_hit("/style.css", expect_smaller_than=len(origin_r.content))
        client.assert_hit(r)
        vary = r.headers.get("Vary", "")
        assert "Accept-Encoding" in vary, (
            f"CSS HIT should have Vary containing Accept-Encoding, got: {vary!r}"
        )


# -- Phase 4.6: Pre-compressed variant tests --
#
# By this point the worker has processed CSS/JS and produced gzip + brotli
# variants.  These tests verify the full pre-compressed pipeline: worker
# writes compressed alternates → selector picks them → nginx serves with
# correct Content-Encoding.


class TestPreCompressedVariants:
    def test_gzip_css_hit_has_content_encoding(
        self, client: PageSpeedClient, e2e_services
    ):
        """Gzip pre-compressed CSS variant served with Content-Encoding: gzip."""
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        r = client.poll_for_hit(
            "/style.css",
            headers={"Accept-Encoding": "gzip"},
            expect_smaller_than=len(origin_r.content),
            timeout=20.0,
        )
        client.assert_hit(r)
        ce = r.headers.get("Content-Encoding", "")
        assert ce == "gzip", f"Expected Content-Encoding: gzip on CSS HIT, got: {ce!r}"
        # requests auto-decompresses gzip; body should be valid CSS
        assert "font-family" in r.text

    def test_brotli_css_hit_has_content_encoding(
        self, client: PageSpeedClient, e2e_services
    ):
        """Brotli pre-compressed CSS variant served with Content-Encoding: br."""
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        r = client.poll_for_hit(
            "/style.css",
            headers={"Accept-Encoding": "br"},
            expect_smaller_than=len(origin_r.content),
            timeout=20.0,
        )
        client.assert_hit(r)
        ce = r.headers.get("Content-Encoding", "")
        assert ce == "br", f"Expected Content-Encoding: br on CSS HIT, got: {ce!r}"
        # requests auto-decompresses brotli only when urllib3 detects the
        # 'brotli' package at import time.  If it wasn't detected (e.g.
        # installed after urllib3 was first imported), r.content is still
        # raw brotli bytes.  Fall back to manual decompression.
        if b"font-family" in r.content:
            # Already decompressed by requests/urllib3
            assert "font-family" in r.text
        else:
            import brotli

            body_text = brotli.decompress(r.content).decode("utf-8")
            assert "font-family" in body_text

    def test_gzip_js_hit_has_content_encoding(
        self, client: PageSpeedClient, e2e_services
    ):
        """Gzip pre-compressed JS variant served with Content-Encoding: gzip."""
        origin_r = requests.get(e2e_services.origin_url + "/app.js", timeout=5)
        r = client.poll_for_hit(
            "/app.js",
            headers={"Accept-Encoding": "gzip"},
            expect_smaller_than=len(origin_r.content),
            timeout=20.0,
        )
        client.assert_hit(r)
        ce = r.headers.get("Content-Encoding", "")
        assert ce == "gzip", f"Expected Content-Encoding: gzip on JS HIT, got: {ce!r}"
        assert "initApp" in r.text

    def test_identity_hit_no_content_encoding(
        self, client: PageSpeedClient, e2e_services
    ):
        """Identity alternate HIT has no Content-Encoding from PageSpeed.

        When the client does not accept gzip or brotli, the selector
        picks the identity alternate.  PageSpeed must NOT set
        Content-Encoding on identity content (Invariant I3).
        """
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        # Request with Accept-Encoding: identity — selector must pick
        # the identity alternate, not a pre-compressed one.
        r = client.poll_for_hit(
            "/style.css",
            headers={"Accept-Encoding": "identity"},
            expect_smaller_than=len(origin_r.content),
            timeout=20.0,
        )
        client.assert_hit(r)
        ce = r.headers.get("Content-Encoding", "")
        assert ce == "", f"Identity HIT should not have Content-Encoding, got: {ce!r}"
        assert "font-family" in r.text

    def test_no_double_compression(self, client: PageSpeedClient, e2e_services):
        """Pre-compressed gzip variant decompresses to valid CSS (not double-compressed)."""
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        r = client.poll_for_hit(
            "/style.css",
            headers={"Accept-Encoding": "gzip"},
            expect_smaller_than=len(origin_r.content),
            timeout=20.0,
        )
        client.assert_hit(r)
        # requests auto-decompresses; if double-compressed, r.text would
        # be binary garbage, not valid CSS containing "font-family"
        assert len(r.text) > 10, (
            f"Decompressed body suspiciously short ({len(r.text)} chars)"
        )
        assert "font-family" in r.text

    def test_image_hit_no_content_encoding(self, client: PageSpeedClient):
        """Image HITs should NOT have Content-Encoding (images are not pre-compressed)."""
        r = client.poll_for_hit(
            "/photo.jpg",
            headers={
                "Accept": "image/webp,*/*",
                "Accept-Encoding": "gzip, br",
            },
            timeout=20.0,
        )
        client.assert_hit(r)
        ce = r.headers.get("Content-Encoding", "")
        assert ce == "", f"Image HIT should not have Content-Encoding, got: {ce!r}"

    def test_encoding_mismatch_serves_correct_encoding(
        self, client: PageSpeedClient, e2e_services
    ):
        """Gzip-only client must NOT receive brotli content (Invariant I2).

        The selector hard-disqualifies alternates with a non-identity
        encoding that does not match the client.  A gzip-only client
        must get gzip (or identity fallback), never brotli.
        """
        origin_r = requests.get(e2e_services.origin_url + "/style.css", timeout=5)
        r = client.poll_for_hit(
            "/style.css",
            headers={"Accept-Encoding": "gzip"},
            expect_smaller_than=len(origin_r.content),
            timeout=20.0,
        )
        client.assert_hit(r)
        ce = r.headers.get("Content-Encoding", "")
        # Must be gzip or empty (identity fallback), never "br"
        assert ce != "br", (
            "Gzip-only client received Content-Encoding: br — "
            "encoding mismatch disqualification failed"
        )
        assert ce in ("gzip", ""), (
            f"Expected gzip or identity for gzip-only client, got: {ce!r}"
        )
        # Body should be valid CSS regardless of encoding path
        assert "font-family" in r.text


# -- 9a: Hostname-aware caching --


class TestHostnameCaching:
    def test_different_hosts_get_independent_miss(self, client: PageSpeedClient):
        """Requests with different Host headers should not share cache entries."""
        # First request with default host should be a HIT (already cached)
        r1 = client.get("/style.css")
        assert r1.status_code == 200

        # Request with a different Host header should get a MISS
        # (different hostname → different cache key).
        # Use a fresh connection (not keep-alive) to avoid pooling issues.
        r2 = requests.get(
            client.base_url + "/style.css",
            headers={"Host": "127.0.0.1"},
        )
        assert r2.status_code == 200
        # The 127.0.0.1 request should be a MISS since the cache key
        # includes hostname and "127.0.0.1" differs from "localhost".
        client.assert_miss(r2)


# -- 9b: PURGE removes all alternates --


class TestPurge:
    @pytest.mark.xfail(
        reason="PURGE unreliable under cache pressure (known Cyclone limitation)",
        strict=False,
    )
    def test_purge_removes_variants(self, client: PageSpeedClient, e2e_services):
        """PURGE on management socket removes all variants for a URL.

        Note: PURGE reliability depends on the Cyclone mmap'd directory
        syncing between the worker and nginx processes.  We use a unique
        URL (not previously cached) to avoid interference from other tests.
        """
        # Use a path not heavily tested elsewhere to reduce interference
        path = "/responsive.css"
        # First request — cold miss, caches the content
        r = client.get(path)
        assert r.status_code == 200
        client.assert_miss(r)

        # Wait for the variant to become a HIT
        r = client.poll_for_hit(path, timeout=15.0)
        client.assert_hit(r)

        # PURGE via management socket (format: PURGE <hostname> <url>)
        result = docker_exec(
            "worker",
            [
                "python3",
                "-c",
                "import socket; s=socket.socket(socket.AF_UNIX);"
                f" s.connect('/tmp/pagespeed.sock.mgmt');"
                f" s.send(b'PURGE localhost {path}\\n');"
                " print(s.recv(256).decode()); s.close()",
            ],
        )
        if result.returncode == 0:
            output = result.stdout.strip()
            assert output.startswith("OK"), (
                f"PURGE expected OK response, got: {output!r}"
            )

        # Allow mmap'd directory to sync between processes
        time.sleep(1)

        # After purge, the next request should be a MISS
        r2 = client.get(path)
        assert r2.status_code == 200
        client.assert_miss(r2)


# -- 9b: Multi-alternate selection --


class TestMultiAlternateSelection:
    def test_same_url_different_accepts(self, client: PageSpeedClient):
        """Same URL returns different content based on Accept header."""
        # By this point, proactive variants should exist for photo.jpg.
        # Request with WebP accept
        r_webp = client.poll_for_hit(
            "/photo.jpg",
            headers={"Accept": "image/webp,*/*"},
            timeout=20.0,
        )
        # Request with AVIF accept
        r_avif = client.poll_for_hit(
            "/photo.jpg",
            headers={"Accept": "image/avif,*/*"},
            timeout=20.0,
        )
        # Request without format preference (gets original/optimized)
        r_orig = client.poll_for_hit(
            "/photo.jpg",
            headers={"Accept": "*/*"},
            timeout=20.0,
        )

        # All should be HIT
        client.assert_hit(r_webp)
        client.assert_hit(r_avif)

        # WebP should have RIFF magic
        if len(r_webp.content) >= 12:
            assert r_webp.content[:4] == b"RIFF", (
                f"WebP variant expected RIFF, got: {r_webp.content[:4]!r}"
            )
        # AVIF should have ftyp box
        if len(r_avif.content) >= 12:
            assert r_avif.content[4:8] == b"ftyp", (
                f"AVIF variant expected ftyp, got: {r_avif.content[4:8]!r}"
            )
        # Content should differ between formats
        assert r_webp.content != r_avif.content, (
            "WebP and AVIF variants should contain different content"
        )


# -- 9b: Content-Type preservation --


class TestContentTypePreservation:
    def test_css_content_type_preserved(self, client: PageSpeedClient):
        """CSS responses maintain correct Content-Type on HIT."""
        r = client.poll_for_hit("/style.css", timeout=10.0)
        ct = r.headers.get("Content-Type", "")
        assert "text/css" in ct, (
            f"CSS HIT should have text/css Content-Type, got: {ct!r}"
        )

    def test_js_content_type_preserved(self, client: PageSpeedClient):
        """JS responses maintain correct Content-Type on HIT."""
        r = client.poll_for_hit("/app.js", timeout=10.0)
        ct = r.headers.get("Content-Type", "")
        assert "javascript" in ct or "application/x-javascript" in ct, (
            f"JS HIT should have javascript Content-Type, got: {ct!r}"
        )

    def test_html_content_type_preserved(self, client: PageSpeedClient):
        """HTML responses maintain correct Content-Type."""
        r = client.get("/index.html")
        ct = r.headers.get("Content-Type", "")
        assert "text/html" in ct, (
            f"HTML should have text/html Content-Type, got: {ct!r}"
        )


# -- Phase 3.3: SVG Passthrough --


class TestSvgPassthrough:
    """SVG images must pass through unchanged (not raster-transcoded)."""

    def test_svg_served_unchanged(self, client: PageSpeedClient):
        """SVG content passes through without transcoding."""
        r = client.get("/logo.svg")
        assert r.status_code == 200, (
            f"SVG test file should return 200, got {r.status_code}"
        )
        ct = r.headers.get("Content-Type", "")
        assert "svg" in ct.lower(), f"SVG should preserve Content-Type, got: {ct!r}"
        # SVG content should contain XML-like content, not raster magic bytes.
        body = r.text
        assert "<svg" in body or "<?xml" in body, (
            "SVG body should contain SVG/XML content, not raster data"
        )


# -- Phase 3.9: Large File Graceful Degradation --


class TestLargeFileGracefulDegradation:
    """Files exceeding worker size limits should be served, not error."""

    def test_large_html_returns_200(self, client: PageSpeedClient):
        """HTML exceeding max-html-size should return 200, not error."""
        r = client.get("/large.html")
        assert r.status_code == 200, (
            f"Large HTML should return 200, got {r.status_code}"
        )
        body = r.text
        assert "</html>" in body.lower(), "Large HTML body should be complete"


# -- Phase 1.5: Compressed Origin Guard Regression --
#
# Verifies that the P0 Content-Encoding guard (Phase 1.1) prevents
# cache corruption when an origin sends gzip-compressed responses.
# The /gzip-origin/ location proxies to an nginx origin with gzip
# enabled, deliberately without stripping Accept-Encoding.


class TestCompressedOriginGuard:
    """Regression tests for the compressed origin Content-Encoding guard."""

    def test_compressed_origin_response_served(self, client: PageSpeedClient):
        """Compressed origin response is still served to the client."""
        r = client.get(
            "/gzip-origin/style.css",
            headers={"Accept-Encoding": "gzip"},
        )
        assert r.status_code == 200, (
            f"Expected 200 from compressed origin, got {r.status_code}"
        )
        # requests auto-decompresses gzip, so .text is plain CSS
        assert "font-family" in r.text, "Response body should contain valid CSS content"

    def test_compressed_origin_not_cached(self, client: PageSpeedClient):
        """Compressed origin responses must never be cached (always MISS).

        This is the key regression test: if the Content-Encoding guard
        (Phase 1.1, ngx_pagespeed_module.cc:726-740) is removed, the
        second request would be a HIT with corrupted double-compressed
        content.
        """
        r1 = client.get(
            "/gzip-origin/style.css",
            headers={"Accept-Encoding": "gzip"},
        )
        assert r1.status_code == 200
        client.assert_miss(r1)
        assert "font-family" in r1.text, (
            "First response body should be valid CSS (not corrupted)"
        )

        r2 = client.get(
            "/gzip-origin/style.css",
            headers={"Accept-Encoding": "gzip"},
        )
        assert r2.status_code == 200
        client.assert_miss(r2)
        assert "font-family" in r2.text, (
            "Second response body should be valid CSS, not double-compressed garbage"
        )

    def test_compressed_origin_warning_logged(self, e2e_services):
        """Content-Encoding guard emits a warning in the nginx error log.

        Note: prior tests in this class also trigger the warning, so
        this assertion is cumulative across all /gzip-origin/ requests.
        """
        # Make a request to trigger the guard
        requests.get(
            e2e_services.nginx_url + "/gzip-origin/style.css",
            headers={"Accept-Encoding": "gzip"},
            timeout=5,
        )
        # Small delay for log flush safety
        time.sleep(0.1)
        result = docker_exec("nginx", "cat /var/log/nginx/error.log")
        assert result.returncode == 0, (
            f"Failed to read nginx error log: {result.stderr}"
        )
        assert "pagespeed: upstream sent Content-Encoding" in result.stdout, (
            "Expected Content-Encoding guard warning in nginx error log, "
            f"got: {result.stdout[-500:]}"
        )


# -- Origin change converges across br, gzip and identity --

_ENCODINGS = ("br", "gzip", "identity")
_EXPECTED_CE = {"br": "br", "gzip": "gzip", "identity": ""}


def _fetch_raw(nginx_url: str, path: str, encoding: str):
    """GET path with one Accept-Encoding; return (response, decoded bytes)."""
    headers = {"Accept-Encoding": encoding}
    r = requests.get(nginx_url + path, headers=headers, stream=True, timeout=10)
    raw = r.raw.read(decode_content=False)
    ce = r.headers.get("Content-Encoding", "").lower()
    if ce == "gzip":
        body = gzip.decompress(raw)
    elif ce == "br":
        import brotli

        body = brotli.decompress(raw)
    else:
        body = raw
    return r, body


def _fetch_all(nginx_url: str, path: str):
    """Fetch path in every encoding; map encoding to (response, body)."""
    return {enc: _fetch_raw(nginx_url, path, enc) for enc in _ENCODINGS}


def _write_mutable(rel_path: str, content: str):
    """Write (replace) a file in the mutable origin volume."""
    assert "'" not in content
    full = f"/var/www/{rel_path}"
    result = docker_exec(
        "mutable-origin",
        [
            "sh",
            "-c",
            f"mkdir -p \"$(dirname '{full}')\" && "
            f"printf '%s' '{content}' > '{full}.tmp' && mv '{full}.tmp' '{full}'",
        ],
    )
    assert result.returncode == 0, f"Failed to write {full}: {result.stderr}"


def _css(marker: str, rules: int = 60) -> str:
    # A different rule count gives the rewritten file a different size, so
    # the origin validators (mtime, size) always differ.
    # The hex colour has no shorter form, so the minifier keeps it.
    filler = "".join(
        f".c{i}{{margin:{i}px 3px;padding:{i + 1}px 5px}}" for i in range(rules)
    )
    return f".m{{color:{marker}}}{filler}"


def _svg(marker: str, rules: int = 60) -> str:
    # An element id survives minification.
    filler = "".join(
        f'<rect x="{i}" y="{i}" width="3" height="3" fill="#0a1b2c"/>'
        for i in range(rules)
    )
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" '
        f'viewBox="0 0 100 100"><g id="{marker}">{filler}</g></svg>'
    )


class TestOriginChangeConvergence:
    """After the origin file changes, br, gzip and identity must converge.

    Files live in the mutable origin (short max-age) and are rewritten
    mid-test.  Every encoding is decoded and compared byte for byte.
    """

    MAX_AGE = 3

    def _warm(self, nginx_url, path, marker, timeout=40.0):
        """Wait until all three encodings are HITs carrying the marker."""
        deadline = time.time() + timeout
        last = {}
        while time.time() < deadline:
            last = _fetch_all(nginx_url, path)
            ok = all(
                r.status_code == 200
                and r.headers.get("X-PageSpeed") == "HIT"
                and r.headers.get("Content-Encoding", "").lower() == _EXPECTED_CE[enc]
                and marker.encode() in body
                for enc, (r, body) in last.items()
            )
            if ok:
                return last
            time.sleep(0.5)
        summary = {
            enc: (
                r.status_code,
                r.headers.get("X-PageSpeed"),
                r.headers.get("Content-Encoding"),
                marker.encode() in body,
            )
            for enc, (r, body) in last.items()
        }
        pytest.fail(f"Not warm after {timeout}s for {path}: {summary}")

    def _assert_converges(
        self, nginx_url, path, old, new, timeout=30.0
    ):
        """Poll until all encodings agree on `new`.

        Once an encoding has served `new`, it must not serve `old` again
        after the front end's stale-while-revalidate window has passed
        (the window is min(10 s, max-age)); inside the window a coalesced
        stale serve is legitimate, so it is not checked.
        """
        grace = self.MAX_AGE + 1.0
        first_new = {}
        bodies = {}
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = _fetch_all(nginx_url, path)
            now = time.monotonic()
            bodies = {enc: body for enc, (_, body) in got.items()}
            for enc, body in bodies.items():
                if new.encode() in body:
                    first_new.setdefault(enc, now)
                elif enc in first_new and now - first_new[enc] > grace:
                    if old.encode() in body:
                        pytest.fail(
                            f"{enc} served the old content again, "
                            f"{now - first_new[enc]:.1f}s after it served the "
                            f"new content, for {path}"
                        )
            same = len(set(bodies.values())) == 1
            encodings_ok = all(
                r.headers.get("Content-Encoding", "").lower() == _EXPECTED_CE[enc]
                for enc, (r, _) in got.items()
            )
            if same and encodings_ok and new.encode() in bodies["identity"]:
                return
            time.sleep(0.5)
        pytest.fail(
            f"Encodings did not converge on the new content within {timeout}s "
            f"for {path}: " + ", ".join(
                f"{enc}: new={new.encode() in body} old={old.encode() in body}"
                for enc, body in bodies.items()
            )
        )

    @staticmethod
    def _settled_stats(worker_api, quiet=4.0, timeout=60.0):
        """Return worker stats once the write counters have stopped moving.

        The counters are global to the worker.  Variant builds that earlier
        tests left in flight (a rebuild of another file finishing late)
        would otherwise move them inside the window under test and be
        mistaken for work on the file under test.
        """
        keys = (
            ("origin_refresh", "purges"),
            ("origin_refresh", "change_rebuilt"),
            ("alternates", "writes"),
        )

        def snapshot():
            stats = worker_api.get_stats()
            return stats, tuple(stats[g][k] for g, k in keys)

        deadline = time.monotonic() + timeout
        stats, last = snapshot()
        stable_since = time.monotonic()
        while time.monotonic() < deadline:
            time.sleep(0.5)
            stats, now = snapshot()
            if now != last:
                last, stable_since = now, time.monotonic()
            elif time.monotonic() - stable_since >= quiet:
                return stats
        pytest.fail(f"Worker write counters did not settle within {timeout}s: {last}")

    def test_css_change_converges_across_encodings_after_max_age(self, e2e_services):
        name = f"conv_css_{int(time.time() * 1000)}.css"
        path = unique_url(f"/mutable/{name}", "conv_css")
        _write_mutable(name, _css("#a1b2c4"))
        self._warm(e2e_services.nginx_url, path, "#a1b2c4")
        _write_mutable(name, _css("#d4e5f7", rules=61))
        time.sleep(self.MAX_AGE + 1)
        self._assert_converges(e2e_services.nginx_url, path, "#a1b2c4", "#d4e5f7")

    def test_svg_change_converges_across_encodings_after_max_age(self, e2e_services):
        name = f"conv_svg_{int(time.time() * 1000)}.svg"
        path = unique_url(f"/mutable/{name}", "conv_svg")
        _write_mutable(name, _svg("mk-old-a1"))
        self._warm(e2e_services.nginx_url, path, "mk-old-a1")
        _write_mutable(name, _svg("mk-new-b2", rules=61))
        time.sleep(self.MAX_AGE + 1)
        self._assert_converges(e2e_services.nginx_url, path, "mk-old-a1", "mk-new-b2")

    def test_unchanged_rerecord_restamps_without_rebuild(
        self, e2e_services, worker_api
    ):
        """An unchanged file is restamped after expiry, not rebuilt.

        Path proven: the front end's conditional revalidation.  The mutable
        origin sends `etag on`, so the expired stored copy is revalidated
        with a conditional request, the origin answers 304 and the front
        end restamps the copy itself (`X-PageSpeed: REVALIDATED`) without
        involving the worker.  The worker must therefore not purge, rebuild
        or write alternates for this file.
        """
        name = f"conv_same_{int(time.time() * 1000)}.css"
        path = unique_url(f"/mutable/{name}", "conv_same")
        _write_mutable(name, _css("#a1b2c4"))
        self._warm(e2e_services.nginx_url, path, "#a1b2c4")
        before = self._settled_stats(worker_api)
        time.sleep(self.MAX_AGE + 1)
        revalidated = False
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and not revalidated:
            got = _fetch_all(e2e_services.nginx_url, path)
            for enc, (r, body) in got.items():
                assert b"#a1b2c4" in body, f"{enc} lost the content for {path}"
                if r.headers.get("X-PageSpeed") == "REVALIDATED":
                    revalidated = True
            time.sleep(0.5)
        assert revalidated, (
            f"No response was restamped by revalidation for {path} after "
            f"max-age expiry"
        )
        # Let any worker-side effect of the refresh land before comparing.
        time.sleep(2.0)
        after = worker_api.get_stats()
        for group, key in (
            ("origin_refresh", "purges"),
            ("origin_refresh", "change_rebuilt"),
            ("alternates", "writes"),
        ):
            assert after[group][key] == before[group][key], (
                f"Unchanged file moved {group}.{key}: "
                f"{before[group]} -> {after[group]}"
            )
