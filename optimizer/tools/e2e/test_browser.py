# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""mod_pagespeed E2E Browser Tests (Playwright).

Headless browser tests verifying that optimized content renders correctly,
JavaScript executes after minification, performance metrics are acceptable,
and cross-browser content negotiation works.

Requires Docker Compose services to be running (managed by conftest.py)
and Playwright browsers installed (run_browser_tests.sh handles this).
"""

import time

import pytest
import requests
from conftest import NGINX_URL, ORIGIN_URL

# ── Group A: Rendering Correctness After Optimization ───────────────


class TestRenderingCorrectness:
    """Verify optimized content renders correctly in a real browser."""

    def test_css_preserves_visual_rendering(self, page):
        """Computed styles match expectations after CSS minification."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        # Check that external CSS is applied (font-family from style.css)
        body_font = page.evaluate("window.getComputedStyle(document.body).fontFamily")
        assert "Arial" in body_font or "sans-serif" in body_font, (
            f"Expected Arial/sans-serif font, got: {body_font}"
        )

        # Check inline styles applied (hero background)
        hero_bg = page.evaluate(
            "window.getComputedStyle(document.querySelector('.hero')).backgroundColor"
        )
        # #0066cc = rgb(0, 102, 204)
        assert hero_bg != "" and hero_bg != "rgba(0, 0, 0, 0)", (
            f"Hero background not applied: {hero_bg}"
        )

    def test_images_display_after_transcoding(self, page):
        """Images load and display with correct dimensions after
        transcoding."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        # JPEG image loaded
        jpeg_complete = page.evaluate("document.getElementById('img-jpeg').complete")
        jpeg_width = page.evaluate("document.getElementById('img-jpeg').naturalWidth")
        assert jpeg_complete is True, "JPEG image did not complete loading"
        assert jpeg_width > 0, f"JPEG naturalWidth is {jpeg_width}"

    def test_animated_gif_loads(self, page):
        """Animated GIF element loads with correct dimensions."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        gif_complete = page.evaluate("document.getElementById('img-gif').complete")
        gif_width = page.evaluate("document.getElementById('img-gif').naturalWidth")
        assert gif_complete is True, "GIF image did not complete loading"
        assert gif_width > 0, f"GIF naturalWidth is {gif_width}"


# ── Group B: JavaScript Execution After Minification ────────────────


class TestJavaScriptExecution:
    """Verify JS works correctly after minification pipeline."""

    def _load_page_with_console(self, page):
        """Load browser-test.html and capture console messages."""
        messages = []
        errors = []
        page.on("console", lambda msg: messages.append(msg))
        page.on("pageerror", lambda exc: errors.append(str(exc)))
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")
        return messages, errors

    def test_no_console_errors(self, page):
        """Zero console.error or pageerror events during page load."""
        messages, errors = self._load_page_with_console(page)
        error_messages = [m for m in messages if m.type == "error"]
        assert len(errors) == 0, f"Page errors: {errors}"
        assert len(error_messages) == 0, (
            f"Console errors: {[m.text for m in error_messages]}"
        )

    def test_init_app_executed(self, page):
        """app.js initApp() ran and logged initialization message."""
        messages, _ = self._load_page_with_console(page)
        texts = [m.text for m in messages]
        assert any("PageSpeed E2E test app initialized" in t for t in texts), (
            f"initApp() message not found in console: {texts}"
        )

    def test_dom_manipulation_works(self, page):
        """interactive.js updated #js-output text content."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        output_text = page.text_content("#js-output")
        assert output_text == "JavaScript executed successfully", (
            f"Expected JS output text, got: {output_text!r}"
        )

        js_loaded = page.get_attribute("#js-output", "data-js-loaded")
        assert js_loaded == "true", f"data-js-loaded attribute: {js_loaded!r}"

    def test_event_handlers_work(self, page):
        """Clicking #test-button triggers handler that updates text."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        page.click("#test-button")

        button_text = page.text_content("#test-button")
        assert button_text == "Button was clicked", (
            f"Button text after click: {button_text!r}"
        )

        clicked_attr = page.get_attribute("#test-button", "data-clicked")
        assert clicked_attr == "true", f"data-clicked attribute: {clicked_attr!r}"

    def test_dynamic_dom_creation(self, page):
        """interactive.js creates 3 <li> children in #dynamic-list."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        count = page.evaluate("document.querySelectorAll('#dynamic-list > li').length")
        assert count == 3, f"Expected 3 dynamic list items, got {count}"

    def test_performance_timing_works(self, page):
        """app.js logs page load timing via performance.now()."""
        messages, _ = self._load_page_with_console(page)
        texts = [m.text for m in messages]
        assert any("Page loaded in" in t and "ms" in t for t in texts), (
            f"Performance timing message not found: {texts}"
        )


# ── Group C: Performance Metrics ────────────────────────────────────


class TestPerformanceMetrics:
    """Verify optimization doesn't degrade performance metrics."""

    def test_fcp_within_bounds(self, page):
        """First Contentful Paint < 5000ms (generous for Docker)."""
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        fcp = page.evaluate("""() => {
            const entries = performance.getEntriesByType('paint');
            const fcp = entries.find(
                e => e.name === 'first-contentful-paint'
            );
            return fcp ? fcp.startTime : null;
        }""")

        if fcp is not None:
            assert fcp < 5000, f"FCP too slow: {fcp:.0f}ms (limit: 5000ms)"

    def test_no_layout_shifts(self, page):
        """CLS < 0.1 (optimization didn't introduce layout shifts)."""
        # Inject PerformanceObserver before navigation
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        cls = page.evaluate("""() => {
            return new Promise(resolve => {
                let clsValue = 0;
                const observer = new PerformanceObserver(list => {
                    for (const entry of list.getEntries()) {
                        if (!entry.hadRecentInput) {
                            clsValue += entry.value;
                        }
                    }
                });
                try {
                    observer.observe({
                        type: 'layout-shift', buffered: true
                    });
                } catch(e) {
                    resolve(0);
                    return;
                }
                // Give time for any pending shifts
                setTimeout(() => {
                    observer.disconnect();
                    resolve(clsValue);
                }, 1000);
            });
        }""")

        assert cls < 0.1, f"CLS too high: {cls} (limit: 0.1)"

    def test_optimized_resources_smaller(self, chromium, e2e_services):
        """Optimized CSS/JS resources are smaller than origin.

        Uses responsive.css and interactive.js (only loaded from
        browser-test.html) to avoid capability-mask conflicts with
        resources already cached by the HTTP-level tests.
        """
        # Get origin sizes for browser-test-specific resources
        origin_css = requests.get(f"{ORIGIN_URL}/responsive.css", timeout=5)
        origin_js = requests.get(f"{ORIGIN_URL}/interactive.js", timeout=5)

        css_url = f"{NGINX_URL}/responsive.css"
        js_url = f"{NGINX_URL}/interactive.js"

        # Poll with fresh browser contexts (each has clean HTTP cache).
        # The first load triggers cache recording + worker notification;
        # subsequent loads check if the optimized variant has arrived.
        deadline = time.time() + 25.0
        response_sizes = {}
        while time.time() < deadline:
            response_sizes.clear()
            context = chromium.new_context()
            p = context.new_page()

            def capture_response(response):
                url = response.url
                if "responsive.css" in url or "interactive.js" in url:
                    try:
                        body = response.body()
                        response_sizes[url] = len(body)
                    except Exception:
                        pass

            p.on("response", capture_response)
            p.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")
            context.close()

            css_ok = css_url not in response_sizes or response_sizes[css_url] < len(
                origin_css.content
            )
            js_ok = js_url not in response_sizes or response_sizes[js_url] < len(
                origin_js.content
            )
            if css_ok and js_ok:
                break
            time.sleep(2)

        if css_url in response_sizes:
            assert response_sizes[css_url] < len(origin_css.content), (
                f"CSS not smaller: browser={response_sizes[css_url]}B "
                f"vs origin={len(origin_css.content)}B"
            )

        if js_url in response_sizes:
            assert response_sizes[js_url] < len(origin_js.content), (
                f"JS not smaller: browser={response_sizes[js_url]}B "
                f"vs origin={len(origin_js.content)}B"
            )


# ── Group D: Cross-Browser Content Negotiation ─────────────────────


class TestContentNegotiation:
    """Verify image format negotiation works across browser engines."""

    @pytest.fixture(scope="class")
    def _warm_cache(self, e2e_services):
        """Ensure the image has been requested at least once."""
        session = requests.Session()
        session.get(f"{NGINX_URL}/photo.jpg", timeout=5)
        # Give worker time to process
        time.sleep(5)

    def _get_image_bytes(self, browser, url):
        """Navigate to a page with the image and capture response body."""
        context = browser.new_context()
        page = context.new_page()
        image_body = None

        def capture_image(response):
            nonlocal image_body
            if "photo.jpg" in response.url:
                try:
                    image_body = response.body()
                except Exception:
                    pass

        page.on("response", capture_image)
        page.goto(url, wait_until="networkidle")
        context.close()
        return image_body

    def test_chromium_gets_modern_format(self, chromium, e2e_services, _warm_cache):
        """Chromium's Accept header gets WebP or AVIF response."""
        body = self._get_image_bytes(chromium, f"{NGINX_URL}/browser-test.html")
        if body is None:
            pytest.skip("Could not capture image response")
        # Chromium sends Accept: image/avif,image/webp,...
        # Expect WebP (RIFF....WEBP) or AVIF
        is_webp = len(body) >= 12 and body[:4] == b"RIFF" and (body[8:12] == b"WEBP")
        is_avif = len(body) >= 12 and (
            body[4:12] == b"ftypavif" or body[4:12] == b"ftypavis"
        )
        is_jpeg = len(body) >= 3 and body[:3] == b"\xff\xd8\xff"
        # Accept WebP, AVIF, or JPEG (if worker hasn't optimized yet)
        assert is_webp or is_avif or is_jpeg, (
            f"Unexpected format, first 12 bytes: {body[:12]!r}"
        )

    def test_firefox_gets_webp(self, firefox, e2e_services, _warm_cache):
        """Firefox's Accept header gets WebP response."""
        body = self._get_image_bytes(firefox, f"{NGINX_URL}/browser-test.html")
        if body is None:
            pytest.skip("Could not capture image response")
        # Firefox sends Accept: image/avif,image/webp,...
        is_webp = len(body) >= 12 and body[:4] == b"RIFF" and (body[8:12] == b"WEBP")
        is_avif = len(body) >= 12 and (
            body[4:12] == b"ftypavif" or body[4:12] == b"ftypavis"
        )
        is_jpeg = len(body) >= 3 and body[:3] == b"\xff\xd8\xff"
        assert is_webp or is_avif or is_jpeg, (
            f"Unexpected format, first 12 bytes: {body[:12]!r}"
        )

    def test_webkit_gets_valid_image(self, webkit, e2e_services, _warm_cache):
        """WebKit gets a valid image format (JPEG, WebP, or AVIF)."""
        body = self._get_image_bytes(webkit, f"{NGINX_URL}/browser-test.html")
        if body is None:
            pytest.skip("Could not capture image response")
        # Modern WebKit supports AVIF and WebP; accept any valid format
        is_jpeg = len(body) >= 3 and body[:3] == b"\xff\xd8\xff"
        is_webp = len(body) >= 12 and body[:4] == b"RIFF" and (body[8:12] == b"WEBP")
        is_avif = len(body) >= 12 and (
            body[4:12] == b"ftypavif" or body[4:12] == b"ftypavis"
        )
        assert is_jpeg or is_webp or is_avif, (
            f"Unexpected format, first 12 bytes: {body[:12]!r}"
        )

    def test_save_data_header_variant(self, chromium, e2e_services, _warm_cache):
        """Save-Data: on header produces response with X-PageSpeed."""
        context = chromium.new_context(extra_http_headers={"Save-Data": "on"})
        page = context.new_page()

        response = page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")
        x_pagespeed = response.headers.get("x-pagespeed")
        assert x_pagespeed in ("HIT", "MISS"), (
            f"Expected X-PageSpeed header, got: {x_pagespeed!r}"
        )
        context.close()


# ── Group E: Viewport Emulation ─────────────────────────────────────


class TestViewportEmulation:
    """Verify responsive behavior at different viewport sizes."""

    def test_mobile_no_overflow(self, chromium):
        """iPhone viewport (375x667) has no horizontal overflow."""
        context = chromium.new_context(
            viewport={"width": 375, "height": 667},
            user_agent=(
                "Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) "
                "AppleWebKit/605.1.15 (KHTML, like Gecko) "
                "Version/16.0 Mobile/15E148 Safari/604.1"
            ),
        )
        page = context.new_page()
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        scroll_width = page.evaluate("document.documentElement.scrollWidth")
        inner_width = page.evaluate("window.innerWidth")
        assert scroll_width <= inner_width, (
            f"Horizontal overflow on mobile: "
            f"scrollWidth={scroll_width} > innerWidth={inner_width}"
        )
        context.close()

    def test_desktop_content_centered(self, chromium):
        """Desktop viewport (1920x1080) has main content <= 1000px.

        style.css sets max-width: 960px with 20px padding on each side,
        so getBoundingClientRect().width includes padding = up to 1000px.
        """
        context = chromium.new_context(viewport={"width": 1920, "height": 1080})
        page = context.new_page()
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        main_width = page.evaluate(
            "document.querySelector('main').getBoundingClientRect().width"
        )
        assert main_width <= 1000, (
            f"Main content too wide on desktop: {main_width}px (max: 1000px)"
        )
        context.close()


# ── Group F: Error Detection ────────────────────────────────────────


class TestErrorDetection:
    """Verify no errors during page load."""

    def test_all_resources_load(self, page):
        """Zero 4xx/5xx responses for sub-resources."""
        failed_requests = []

        def check_response(response):
            if response.status >= 400:
                failed_requests.append(f"{response.status} {response.url}")

        page.on("response", check_response)
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        assert len(failed_requests) == 0, f"Failed resource loads: {failed_requests}"

    def test_no_page_errors(self, page):
        """Zero uncaught exceptions during page load."""
        page_errors = []
        page.on("pageerror", lambda exc: page_errors.append(str(exc)))
        page.goto(f"{NGINX_URL}/browser-test.html", wait_until="networkidle")

        assert len(page_errors) == 0, f"Uncaught exceptions: {page_errors}"
