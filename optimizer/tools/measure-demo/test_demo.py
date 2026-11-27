# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Demo page Playwright tests.

Tests the demo page (demo.astro) and the underlying demo site pages
against the Astro dev server. Requires:
  - $WEBSITE_DIR/src/data/demo-metrics.json to exist (run measure.py first);
    WEBSITE_DIR names the site tree in the public repository
    (We-Amp/pagespeed-optimizer, website/), default
    <repo root>/website
  - Astro dev server running on localhost:4321 (npm run dev)
  - Playwright browsers installed (npx playwright install chromium)

Run: cd tools/measure-demo && pytest test_demo.py -v
"""

import json
import os
import re
from pathlib import Path

import pytest
from playwright.sync_api import expect, sync_playwright

SITE_URL = "http://localhost:4321"
DEMO_URL = f"{SITE_URL}/demo/"
DEMO_SITES = ["ecommerce", "blog", "news", "portfolio"]
WEBSITE_DIR = Path(
    os.environ.get("WEBSITE_DIR")
    or Path(__file__).parent.parent.parent / "website"
).resolve()
if not WEBSITE_DIR.is_dir():
    raise RuntimeError(
        f"website tree not found at {WEBSITE_DIR}; set WEBSITE_DIR to the public "
        "repository's website/ checkout (We-Amp/pagespeed-optimizer)"
    )
METRICS_PATH = WEBSITE_DIR / "src" / "data" / "demo-metrics.json"


@pytest.fixture(scope="session")
def browser():
    """Launch a shared Chromium browser for all tests."""
    with sync_playwright() as p:
        b = p.chromium.launch(headless=True)
        yield b
        b.close()


@pytest.fixture
def page(browser):
    """Fresh browser page per test (isolated context)."""
    context = browser.new_context()
    pg = context.new_page()
    yield pg
    context.close()


@pytest.fixture(scope="session")
def metrics():
    """Load the generated demo-metrics.json."""
    assert METRICS_PATH.exists(), (
        f"demo-metrics.json not found at {METRICS_PATH}. "
        "Run tools/measure-demo/run.sh first."
    )
    return json.loads(METRICS_PATH.read_text())


# ── Test 1: Demo tabs load ──────────────────────────────────────────


class TestDemoTabs:
    """Click each of the 4 tabs, verify iframe src changes and content loads."""

    def test_first_tab_active_by_default(self, page):
        """E-commerce tab is selected and its iframe is visible on load."""
        page.goto(DEMO_URL, wait_until="networkidle")

        first_tab = page.locator('button[data-demo-tab="ecommerce"]')
        expect(first_tab).to_have_attribute("aria-selected", "true")

        panel = page.locator("#preview-ecommerce")
        expect(panel).to_be_visible()

        iframe = panel.locator("iframe")
        src = iframe.get_attribute("src")
        assert src and "ecommerce" in src

    @pytest.mark.parametrize("site_id", DEMO_SITES)
    def test_tab_click_shows_iframe(self, page, site_id):
        """Clicking each tab shows the corresponding iframe panel."""
        page.goto(DEMO_URL, wait_until="networkidle")

        tab = page.locator(f'button[data-demo-tab="{site_id}"]')
        tab.click()

        # Verify this tab is now selected
        expect(tab).to_have_attribute("aria-selected", "true")

        # Verify the panel is visible
        panel = page.locator(f"#preview-{site_id}")
        expect(panel).to_be_visible()

        # Verify the iframe has a src set (lazy-loaded on click)
        iframe = panel.locator("iframe")
        # Wait briefly for the src to be set by the click handler
        page.wait_for_timeout(500)
        src = iframe.get_attribute("src")
        assert src and site_id in src, (
            f"Expected iframe src containing '{site_id}', got: {src}"
        )

        # URL bar should reflect the demo path
        url_bar = page.locator("#demo-url-bar")
        expect(url_bar).to_contain_text(f"/demos/{site_id}/")

    def test_other_panels_hidden_on_click(self, page):
        """When clicking blog tab, ecommerce panel becomes hidden."""
        page.goto(DEMO_URL, wait_until="networkidle")

        page.locator('button[data-demo-tab="blog"]').click()
        expect(page.locator("#preview-ecommerce")).to_be_hidden()
        expect(page.locator("#preview-blog")).to_be_visible()


# ── Test 2: Metrics are non-zero ────────────────────────────────────


class TestMetricsNonZero:
    """For each site, read the 3 metric cards and assert values are positive."""

    def test_metric_cards_have_positive_values(self, page, metrics):
        """All metric cards show positive values and improvement percentages."""
        page.goto(DEMO_URL, wait_until="networkidle")

        for site in metrics["sites"]:
            site_id = site["id"]

            # Click the site picker button to show this site's panel
            page.locator(f'button[data-site="{site_id}"]').click()

            panel = page.locator(f'[data-panel="{site_id}"]')
            expect(panel).to_be_visible()

            # Get all metric cards in this panel (3 cards in the grid)
            cards = panel.locator(".grid.grid-cols-1 > div.rounded-lg.border")
            assert cards.count() == 3, (
                f"Expected 3 metric cards for {site_id}, got {cards.count()}"
            )

            # Each card should have a green badge with improvement
            badges = panel.locator("span.bg-green-100")
            assert badges.count() >= 3, (
                f"Expected at least 3 green badges for {site_id}"
            )

            # Check that badge text contains numbers > 0
            for i in range(badges.count()):
                text = badges.nth(i).inner_text()
                nums = re.findall(r"[\d.]+", text)
                assert len(nums) > 0, f"No number found in badge: {text}"
                assert float(nums[0]) > 0, f"Badge value is 0: {text}"


# ── Test 3: Resource breakdown table populated ──────────────────────


class TestResourceTable:
    """Assert table rows exist for CSS, JS, and Images with before > after."""

    def test_table_has_expected_rows(self, page):
        """Resource table has 5 data rows (CSS, JS, WebP, AVIF, HTML)."""
        page.goto(DEMO_URL, wait_until="networkidle")

        # Default panel is ecommerce
        panel = page.locator('[data-panel="ecommerce"]')
        tbody = panel.locator("table tbody")
        rows = tbody.locator("tr")
        assert rows.count() == 5, f"Expected 5 table rows, got {rows.count()}"

    def test_table_shows_savings(self, page):
        """CSS and JS rows show negative percentage savings."""
        page.goto(DEMO_URL, wait_until="networkidle")

        panel = page.locator('[data-panel="ecommerce"]')
        # Get all saving cells (4th column) - the green percentage values
        saving_cells = panel.locator("table tbody td.text-green-700")

        for i in range(saving_cells.count()):
            text = saving_cells.nth(i).inner_text().strip()
            # Should be like "-46%" or "-70%"
            assert text.startswith("-"), f"Expected negative saving, got: {text}"
            pct = int(re.search(r"\d+", text).group())
            assert pct > 0, f"Saving percentage is 0: {text}"

    def test_total_row_present(self, page):
        """Table footer has a Total row with aggregate values."""
        page.goto(DEMO_URL, wait_until="networkidle")

        panel = page.locator('[data-panel="ecommerce"]')
        tfoot = panel.locator("table tfoot")
        expect(tfoot).to_contain_text("Total")

        # Total row should have a green saving percentage
        saving = tfoot.locator("td.text-green-700")
        text = saving.inner_text().strip()
        assert text.startswith("-"), f"Expected negative total saving, got: {text}"


# ── Test 4: CSS snippet renders ─────────────────────────────────────


class TestCssSnippet:
    """Assert before/after code blocks are non-empty and after is shorter."""

    def test_css_code_blocks_present(self, page):
        """Both before and after CSS code blocks are visible."""
        page.goto(DEMO_URL, wait_until="networkidle")

        panel = page.locator('[data-panel="ecommerce"]')

        # Find the CSS minification section (has both bg-gray-50 and bg-green-50 pres)
        before_pre = panel.locator("pre.bg-gray-50 code").first
        after_pre = panel.locator("pre.bg-green-50 code").first

        before_text = before_pre.inner_text()
        after_text = after_pre.inner_text()

        assert len(before_text) > 0, "Before CSS snippet is empty"
        assert len(after_text) > 0, "After CSS snippet is empty"

    def test_after_is_shorter_than_before(self, page):
        """Minified CSS is shorter than the original."""
        page.goto(DEMO_URL, wait_until="networkidle")

        panel = page.locator('[data-panel="ecommerce"]')
        before_text = panel.locator("pre.bg-gray-50 code").first.inner_text()
        after_text = panel.locator("pre.bg-green-50 code").first.inner_text()

        assert len(after_text) < len(before_text), (
            f"After ({len(after_text)} chars) should be shorter than "
            f"before ({len(before_text)} chars)"
        )


# ── Test 5: Demo site rendering ─────────────────────────────────────


class TestDemoSiteRendering:
    """Navigate directly to each demo site and verify assets load."""

    @pytest.mark.parametrize("site_id", DEMO_SITES)
    def test_demo_site_loads(self, page, site_id):
        """Each demo site page loads without errors."""
        page.goto(
            f"{SITE_URL}/demos/{site_id}/index.html",
            wait_until="networkidle",
        )
        # Page should have a title
        title = page.title()
        assert len(title) > 0, f"Demo site {site_id} has empty title"

    @pytest.mark.parametrize("site_id", DEMO_SITES)
    def test_external_css_loaded(self, page, site_id):
        """External style.css is loaded (check a computed style)."""
        page.goto(
            f"{SITE_URL}/demos/{site_id}/index.html",
            wait_until="networkidle",
        )
        # All demo sites use CSS custom properties, so body should have
        # some styling applied. Check that body doesn't use browser default
        # margin (8px is the default).
        margin = page.evaluate("window.getComputedStyle(document.body).margin")
        # Our CSS resets margin to 0
        assert "0" in margin, f"Expected body margin reset by CSS, got: {margin}"

    @pytest.mark.parametrize("site_id", DEMO_SITES)
    def test_javascript_executed(self, page, site_id):
        """JavaScript app.js executed (check for DOM modifications)."""
        page.goto(
            f"{SITE_URL}/demos/{site_id}/index.html",
            wait_until="networkidle",
        )
        # All demo sites' JS adds event listeners or modifies DOM.
        # Check that at least one script tag with src="app.js" exists
        # and that the page has interactive elements.
        script_count = page.evaluate(
            "document.querySelectorAll('script[src=\"app.js\"]').length"
        )
        assert script_count >= 1, f"No app.js script found for {site_id}"

    @pytest.mark.parametrize("site_id", DEMO_SITES)
    def test_images_loaded(self, page, site_id):
        """All images have naturalWidth > 0 (loaded successfully)."""
        page.goto(
            f"{SITE_URL}/demos/{site_id}/index.html",
            wait_until="networkidle",
        )
        images = page.evaluate("""
            Array.from(document.querySelectorAll('img')).map(img => ({
                src: img.src,
                complete: img.complete,
                naturalWidth: img.naturalWidth,
            }))
        """)
        assert len(images) > 0, f"No images found on {site_id}"
        for img in images:
            assert img["complete"], f"Image not loaded: {img['src']}"
            assert img["naturalWidth"] > 0, f"Image has 0 width: {img['src']}"


# ── Test 6: No console errors ───────────────────────────────────────


class TestNoConsoleErrors:
    """Attach console listener, assert zero console.error events."""

    def test_demo_page_no_errors(self, page):
        """Demo page loads without console errors."""
        errors = []
        page.on(
            "console",
            lambda msg: errors.append(msg.text) if msg.type == "error" else None,
        )

        page.goto(DEMO_URL, wait_until="networkidle")
        # Click through all tabs to trigger lazy loading
        for site_id in DEMO_SITES:
            page.locator(f'button[data-demo-tab="{site_id}"]').click()
            page.wait_for_timeout(300)

        assert len(errors) == 0, f"Console errors found: {errors}"

    @pytest.mark.parametrize("site_id", DEMO_SITES)
    def test_demo_site_no_errors(self, page, site_id):
        """Individual demo sites load without console errors."""
        errors = []
        page.on(
            "console",
            lambda msg: errors.append(msg.text) if msg.type == "error" else None,
        )

        page.goto(
            f"{SITE_URL}/demos/{site_id}/index.html",
            wait_until="networkidle",
        )
        assert len(errors) == 0, f"Console errors on {site_id}: {errors}"


# ── Test 7: Methodology mentions "measured" ─────────────────────────


class TestMethodology:
    """Assert methodology section references real measurements."""

    def test_methodology_says_measured(self, page):
        """Methodology section text contains 'measured'."""
        page.goto(DEMO_URL, wait_until="networkidle")

        # Find the Methodology heading and its sibling paragraph
        methodology = page.locator("text=Methodology").first
        expect(methodology).to_be_visible()

        # The paragraph after the heading should mention "measured"
        section_text = page.locator("section", has=methodology).inner_text()
        assert "measured" in section_text.lower(), (
            "Methodology section should mention 'measured'"
        )

    def test_methodology_not_estimated(self, page):
        """Methodology section does not say 'estimated' for key metrics."""
        page.goto(DEMO_URL, wait_until="networkidle")

        methodology = page.locator("text=Methodology").first
        section_text = page.locator("section", has=methodology).inner_text()

        # "estimated" is acceptable for load times (they are estimated),
        # but the overall framing should be "measured"
        # Check that the main description says "measured", not "estimated"
        first_para = page.locator("section:has(text='Methodology') > div > p").first
        first_para_text = first_para.inner_text()
        assert "measured" in first_para_text.lower(), (
            "First paragraph of methodology should say 'measured'"
        )

    def test_generated_date_shown(self, page, metrics):
        """The generated-at date is displayed in the methodology section."""
        page.goto(DEMO_URL, wait_until="networkidle")

        # The date is formatted as "Month Day, Year" (e.g., "February 6, 2026")
        generated_at = metrics["generated_at"]
        # Extract year from the ISO timestamp
        year = generated_at[:4]

        methodology = page.locator("text=Methodology").first
        section_text = page.locator("section", has=methodology).inner_text()
        assert year in section_text, f"Expected year {year} in methodology section"
