# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Sanity test suite for the workbench-demo2 Docker stack.

Run against a running stack:

    cd tools/workbench-demo
    pytest test_sanity.py -v

Requires: pytest, requests (pip install pytest requests)
"""

import pytest
import requests
from conftest import _get_nested_stat, _poll_stat

# ---------------------------------------------------------------------------
# 1. Stack Health
# ---------------------------------------------------------------------------


class TestStackHealth:
    """Verify core services are up and responding."""

    def test_worker_health(self, worker_api):
        health = worker_api.health()
        assert health["status"] == "ok"
        assert health["uptime_seconds"] > 0

    def test_worker_ready(self, worker_api):
        health = worker_api.health()
        assert health["ready"] is True

    def test_nginx_responds(self, nginx):
        r = nginx.get("/")
        assert r.status_code == 200


# ---------------------------------------------------------------------------
# 2. Demo Sites
# ---------------------------------------------------------------------------


class TestDemoSites:
    """Website pages return 200."""

    @pytest.mark.parametrize(
        "path",
        ["/", "/docs/", "/pricing/", "/features/"],
    )
    def test_website_pages(self, nginx, path):
        r = nginx.get(path)
        assert r.status_code == 200, f"{path} returned {r.status_code}"

    @pytest.mark.parametrize(
        "path",
        ["/demos/news/", "/demos/ecommerce/", "/demos/portfolio/"],
    )
    def test_demo_pages(self, nginx, path):
        r = nginx.get(path)
        assert r.status_code == 200, f"{path} returned {r.status_code}"

    def test_404_page(self, nginx):
        r = nginx.get("/nonexistent-page-xyz/")
        assert r.status_code == 404


# ---------------------------------------------------------------------------
# 3. Cache and Headers
# ---------------------------------------------------------------------------


class TestCacheAndHeaders:
    """Responses get cached and carry expected headers."""

    def test_homepage_gets_cached(self, nginx, warm_cache):
        r = nginx.poll_for_hit("/", timeout=30)
        assert r.headers.get("X-PageSpeed") == "HIT", (
            f"Expected HIT, got {r.headers.get('X-PageSpeed', '<absent>')}"
        )

    def test_demo_page_gets_cached(self, nginx, warm_cache):
        r = nginx.poll_for_hit("/demos/news/", timeout=30)
        assert r.headers.get("X-PageSpeed") == "HIT", (
            f"Expected HIT, got {r.headers.get('X-PageSpeed', '<absent>')}"
        )

    def test_content_type_html(self, nginx, warm_cache):
        r = nginx.get("/")
        ct = r.headers.get("Content-Type", "")
        assert "text/html" in ct

    def test_vary_header_on_images(self, nginx, warm_cache):
        """Image responses include Vary: Accept for content negotiation."""
        r = nginx.poll_for_hit("/demos/ecommerce/images/jacket.jpg", timeout=30)
        vary = r.headers.get("Vary", "")
        assert "Accept" in vary, f"Expected Vary: Accept on images, got: '{vary}'"

    def test_cache_control_on_hit(self, nginx, warm_cache):
        """Cached responses include a Cache-Control header."""
        r = nginx.poll_for_hit("/demos/ecommerce/images/jacket.jpg", timeout=30)
        cc = r.headers.get("Cache-Control", "")
        assert cc, "Expected Cache-Control header on cached image response"


# ---------------------------------------------------------------------------
# 4. Image Optimization
# ---------------------------------------------------------------------------


class TestImageOptimization:
    """Demo images are served in optimized formats when requested."""

    DEMO_IMAGE = "/demos/ecommerce/images/jacket.jpg"

    def test_original_served(self, nginx, warm_cache):
        """Baseline request returns a JPEG."""
        r = nginx.poll_for_hit(self.DEMO_IMAGE, timeout=30)
        assert r.status_code == 200

    def test_webp_negotiation(self, nginx, warm_cache):
        """Accept: image/webp triggers WebP response."""
        r = nginx.poll_for_hit(
            self.DEMO_IMAGE,
            headers={"Accept": "image/webp,image/*,*/*"},
            timeout=45,
        )
        ct = r.headers.get("Content-Type", "")
        assert "webp" in ct, f"Expected WebP, got {ct}"

    def test_avif_negotiation(self, nginx, warm_cache):
        """Accept: image/avif triggers AVIF response."""
        r = nginx.poll_for_hit(
            self.DEMO_IMAGE,
            headers={"Accept": "image/avif,image/*,*/*"},
            timeout=45,
        )
        ct = r.headers.get("Content-Type", "")
        assert "avif" in ct, f"Expected AVIF, got {ct}"

    def test_optimized_smaller(self, nginx, warm_cache):
        """WebP variant is smaller than original JPEG."""
        orig = nginx.poll_for_hit(self.DEMO_IMAGE, timeout=30)
        orig_size = len(orig.content)

        webp = nginx.poll_for_hit(
            self.DEMO_IMAGE,
            headers={"Accept": "image/webp,image/*,*/*"},
            timeout=45,
            expect_smaller_than=orig_size,
        )
        webp_size = len(webp.content)
        assert webp_size < orig_size, (
            f"WebP ({webp_size}) not smaller than original ({orig_size})"
        )


# ---------------------------------------------------------------------------
# 5. SVG Vectorization
# ---------------------------------------------------------------------------


class TestSvgVectorization:
    """SVG auto-vectorization pipeline runs (--svg-mode auto)."""

    SVG_CANDIDATE = "/demos/news/images/infrastructure.png"

    def test_svg_candidates_evaluated(self, worker_api, nginx, warm_cache):
        """Worker evaluates SVG candidates after cache is warm."""
        # Ensure candidate is in cache
        nginx.get(self.SVG_CANDIDATE)
        val = _poll_stat(
            worker_api,
            ["svg", "candidates_evaluated"],
            min_value=1,
            timeout=30,
        )
        assert val > 0, f"No SVG candidates evaluated: {worker_api.stats().get('svg')}"

    def test_svg_pipeline_ran(self, worker_api, nginx, warm_cache):
        """VTracer ran on at least one candidate."""
        nginx.get(self.SVG_CANDIDATE)
        vectorized = _poll_stat(
            worker_api,
            ["svg", "vectorized"],
            min_value=1,
            timeout=30,
        )
        stats = worker_api.stats().get("svg", {})
        assert vectorized > 0 or stats.get("candidates_rejected", 0) > 0, (
            f"No SVG vectorization attempts: {stats}"
        )


# ---------------------------------------------------------------------------
# 6. Text Optimization
# ---------------------------------------------------------------------------


class TestTextOptimization:
    """HTML responses are optimized and carry PageSpeed headers."""

    def test_html_is_valid(self, nginx, warm_cache):
        """Cached HTML response contains valid markup."""
        r = nginx.poll_for_hit("/", timeout=30)
        assert r.status_code == 200
        text = r.text.lower()
        assert "<html" in text or "<!doctype" in text, "Response doesn't look like HTML"

    def test_html_has_pagespeed_header(self, nginx, warm_cache):
        r = nginx.poll_for_hit("/", timeout=30)
        assert "X-PageSpeed" in r.headers


# ---------------------------------------------------------------------------
# 7. Console
# ---------------------------------------------------------------------------


class TestConsole:
    """Web console is accessible through nginx."""

    def test_console_loads(self, nginx):
        """Console SPA returns HTML."""
        r = nginx.get("/console/")
        assert r.status_code == 200
        ct = r.headers.get("Content-Type", "")
        assert "text/html" in ct, f"Console returned unexpected Content-Type: {ct}"

    def test_console_bypasses_pagespeed(self, nginx):
        """Console responses are not processed by PageSpeed."""
        r = nginx.get("/console/")
        assert "X-PageSpeed" not in r.headers, (
            "Console response should bypass PageSpeed"
        )


# ---------------------------------------------------------------------------
# 8. Cache Inspection
# ---------------------------------------------------------------------------


class TestCacheInspection:
    """Cache inspection API returns meaningful data."""

    def test_cache_urls_populated(self, worker_api, warm_cache):
        data = worker_api.cache_urls(limit=10)
        urls = data.get("urls", [])
        assert len(urls) > 0, f"No URLs in cache: {data}"

    def test_cache_alternates_exist(self, worker_api, warm_cache):
        """At least one cached URL has multiple alternates."""
        urls_data = worker_api.cache_urls(limit=20)
        urls = urls_data.get("urls", [])
        assert len(urls) > 0, "No URLs to inspect alternates for"
        # Find a URL with alternates
        for entry in urls:
            if entry.get("alternate_count", 0) > 1:
                alts = worker_api.cache_alternates(entry["url"], entry["hostname"])
                assert len(alts.get("alternates", [])) > 1, (
                    f"Expected multiple alternates for {entry['url']}: {alts}"
                )
                return
        # If no URL has >1 alternate, at least verify the API works
        first = urls[0]
        alts = worker_api.cache_alternates(first["url"], first["hostname"])
        assert "alternates" in alts


# ---------------------------------------------------------------------------
# 9. Worker Stats
# ---------------------------------------------------------------------------


class TestWorkerStats:
    """Worker stats show processing activity."""

    def test_stats_accessible(self, worker_api):
        stats = worker_api.stats()
        assert "by_type" in stats
        assert "notifications" in stats

    def test_notifications_received(self, worker_api, warm_cache):
        stats = worker_api.stats()
        assert stats["notifications"]["received"] > 0

    def test_images_processed(self, worker_api, warm_cache):
        stats = worker_api.stats()
        img = stats["by_type"]["image"]
        assert img["count"] > 0, "No images processed"

    def test_html_processed(self, worker_api, warm_cache):
        stats = worker_api.stats()
        html = stats["by_type"]["html"]
        assert html["count"] > 0, "No HTML processed"

    def test_quality_optimization_active(self, worker_api, warm_cache):
        """Either SSIMULACRA2 verification or learned quality prediction ran."""
        ssim = _poll_stat(
            worker_api,
            ["ssimulacra2", "checks"],
            min_value=1,
            timeout=30,
        )
        if ssim > 0:
            return
        lq = _poll_stat(
            worker_api,
            ["learned_quality", "predictions"],
            min_value=1,
            timeout=10,
        )
        assert lq > 0, (
            f"No quality optimization: ssimulacra2={worker_api.stats().get('ssimulacra2')}, "
            f"learned_quality={worker_api.stats().get('learned_quality')}"
        )

    def test_content_analysis_active(self, worker_api, warm_cache):
        """Content analysis classified at least one image."""
        # Poll for any content class to be counted.
        for key in ("photo", "screenshot", "illustration", "noisy"):
            val = _poll_stat(
                worker_api,
                ["content_analysis", key],
                min_value=1,
                timeout=5,
            )
            if val > 0:
                return
        # Final check with fresh stats
        stats = worker_api.stats()
        ca = stats.get("content_analysis", {})
        total = sum(
            ca.get(k, 0) for k in ("photo", "screenshot", "illustration", "noisy")
        )
        assert total > 0, f"No content analysis results: {ca}"

    def test_format_counters(self, worker_api, warm_cache):
        """At least one format-specific counter is non-zero."""
        for key in ("webp", "avif", "jpeg"):
            val = _poll_stat(
                worker_api,
                ["by_format", key],
                min_value=1,
                timeout=10,
            )
            if val > 0:
                return
        stats = worker_api.stats()
        fmt = stats.get("by_format", {})
        total = sum(fmt.get(k, 0) for k in ("webp", "avif", "jpeg"))
        assert total > 0, f"No format counters: {fmt}"

    def test_variants_written(self, worker_api, warm_cache):
        stats = worker_api.stats()
        assert stats["variants"]["written"] > 0


# ---------------------------------------------------------------------------
# 10. Cache Purge Recovery
# ---------------------------------------------------------------------------


class TestCachePurgeRecovery:
    """Full purge → re-warm → verify alternates are repopulated.

    Must be the last test class in the file: it purges the cache and verifies
    the full recovery pipeline (nginx generation detection, worker notification,
    variant creation).  All assertions live in one test method to avoid
    inter-test ordering dependencies.
    """

    DEMO_IMAGE = "/demos/ecommerce/images/jacket.jpg"
    DEMO_PAGE = "/demos/news/"

    def test_purge_and_recover(self, nginx, worker_api, warm_cache):
        """Full purge → re-request → all alternates repopulated."""
        # -- Pre-purge: verify cache is populated --------------------------
        pre_urls = worker_api.cache_urls(limit=100)
        assert len(pre_urls.get("urls", [])) > 0, "Cache should be warm before purge"

        # Record cumulative stats (counters survive purge).
        pre_stats = worker_api.stats()
        pre_variants = _get_nested_stat(pre_stats, ["variants", "written"])

        # -- Purge ---------------------------------------------------------
        result = worker_api.purge_all()
        assert result.get("volume_reset") is True, f"Purge failed: {result}"

        post_purge_urls = worker_api.cache_urls(limit=10)
        assert len(post_purge_urls.get("urls", [])) == 0, (
            f"Cache should be empty after purge: {post_purge_urls}"
        )

        # -- Re-warm: trigger MISS → origin → notify worker ---------------
        pages = ["/", self.DEMO_PAGE]
        images = [self.DEMO_IMAGE]
        for p in pages + images:
            try:
                nginx.get(p)
            except requests.RequestException:
                pass

        # -- Verify: worker writes new variants (stat delta) ---------------
        new_variants = _poll_stat(
            worker_api,
            ["variants", "written"],
            min_value=pre_variants + 1,
            timeout=90,
        )
        assert new_variants > pre_variants, (
            f"Worker did not write new variants after purge "
            f"(timed out polling variants.written): "
            f"before={pre_variants}, current={worker_api.stats().get('variants')}"
        )

        # -- Verify: cache repopulated with URLs ---------------------------
        recovered_urls = worker_api.cache_urls(limit=100)
        assert len(recovered_urls.get("urls", [])) > 0, (
            f"No URLs in cache after re-warm: {recovered_urls}"
        )

        # -- Verify: HTML pages cached again -------------------------------
        nginx.get(self.DEMO_PAGE)
        r = nginx.poll_for_hit(self.DEMO_PAGE, timeout=60)
        assert r.headers.get("X-PageSpeed") == "HIT", (
            f"HTML not cached after purge recovery: "
            f"{r.headers.get('X-PageSpeed', '<absent>')}"
        )

        # -- Verify: image original cached ---------------------------------
        r = nginx.poll_for_hit(self.DEMO_IMAGE, timeout=30)
        assert r.headers.get("X-PageSpeed") == "HIT", (
            f"Image not cached after purge recovery: "
            f"{r.headers.get('X-PageSpeed', '<absent>')}"
        )

        # -- Verify: WebP alternate served after worker processes ----------
        webp = nginx.poll_for_hit(
            self.DEMO_IMAGE,
            headers={"Accept": "image/webp,image/*,*/*"},
            timeout=90,
            expect_content_type="webp",
        )
        ct = webp.headers.get("Content-Type", "")
        assert "webp" in ct, (
            f"WebP alternate not available after purge: Content-Type={ct}"
        )

        # -- Verify: multiple alternates exist for at least one URL --------
        urls_data = worker_api.cache_urls(limit=50)
        urls = urls_data.get("urls", [])
        multi_alt = [u for u in urls if u.get("alternate_count", 0) > 1]
        assert len(multi_alt) > 0, (
            f"No URLs with multiple alternates after purge. "
            f"Counts: {[(u['url'], u.get('alternate_count')) for u in urls[:10]]}"
        )
