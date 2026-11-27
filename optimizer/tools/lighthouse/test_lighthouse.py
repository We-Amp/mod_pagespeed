# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Lighthouse A/B validation tests for mod_pagespeed.

Compares Lighthouse audits between origin (unoptimized) and nginx (optimized)
to validate that optimizations improve performance scores.

Requires: npm install -g @lhci/cli lighthouse
Stack: docker compose (origin:8081, nginx:8080) from tools/e2e/
"""

import json
import os
import shutil

import pytest
from conftest import get_audit

pytestmark = pytest.mark.lighthouse


def _skip_if_no_lighthouse():
    """Raise ``pytest.skip`` when the Lighthouse CLI is not installed."""
    if not shutil.which("lighthouse"):
        pytest.skip("lighthouse CLI not found on PATH")


# ---------------------------------------------------------------------------
# TestLighthouseScoring
# ---------------------------------------------------------------------------


class TestLighthouseScoring:
    """Performance score for the optimized page must be >= the origin."""

    def test_performance_score_not_regressed(
        self,
        lighthouse_origin_results: dict,
        lighthouse_nginx_results: dict,
    ):
        """Optimized performance score >= origin performance score."""
        _skip_if_no_lighthouse()

        origin_score = (
            lighthouse_origin_results.get("categories", {})
            .get("performance", {})
            .get("score", 0.0)
        )
        nginx_score = (
            lighthouse_nginx_results.get("categories", {})
            .get("performance", {})
            .get("score", 0.0)
        )
        assert nginx_score >= origin_score, (
            f"Performance regression: "
            f"origin={origin_score:.2f}, nginx={nginx_score:.2f}"
        )

    def test_performance_score_minimum(
        self,
        lighthouse_nginx_results: dict,
    ):
        """Optimized page must achieve at least 0.7 performance score."""
        _skip_if_no_lighthouse()

        score = (
            lighthouse_nginx_results.get("categories", {})
            .get("performance", {})
            .get("score", 0.0)
        )
        assert score >= 0.7, f"Performance score below minimum: {score:.2f} < 0.70"


# ---------------------------------------------------------------------------
# TestRenderBlocking
# ---------------------------------------------------------------------------


class TestRenderBlocking:
    """Render-blocking resource count must decrease after optimization."""

    @staticmethod
    def _blocking_count(report: dict) -> int:
        audit = get_audit(report, "render-blocking-resources")
        if audit is None:
            return 0
        items = audit.get("details", {}).get("items", [])
        return len(items)

    def test_render_blocking_reduced(
        self,
        lighthouse_origin_results: dict,
        lighthouse_nginx_results: dict,
    ):
        """Optimized page has fewer render-blocking resources."""
        _skip_if_no_lighthouse()

        origin_count = self._blocking_count(lighthouse_origin_results)
        nginx_count = self._blocking_count(lighthouse_nginx_results)
        assert nginx_count <= origin_count, (
            f"Render-blocking resources increased: "
            f"origin={origin_count}, nginx={nginx_count}"
        )

    def test_render_blocking_max(
        self,
        lighthouse_nginx_results: dict,
    ):
        """Optimized page has at most 1 render-blocking resource."""
        _skip_if_no_lighthouse()

        count = self._blocking_count(lighthouse_nginx_results)
        assert count <= 1, f"Too many render-blocking resources: {count} > 1"


# ---------------------------------------------------------------------------
# TestMinification
# ---------------------------------------------------------------------------


class TestMinification:
    """Minification audits must pass cleanly for the optimized page."""

    def test_unminified_css_clean(
        self,
        lighthouse_nginx_results: dict,
    ):
        """No unminified CSS on the optimized page."""
        _skip_if_no_lighthouse()

        audit = get_audit(lighthouse_nginx_results, "unminified-css")
        items = audit.get("details", {}).get("items", []) if audit else []
        assert len(items) == 0, (
            f"Unminified CSS found ({len(items)} item(s)): "
            f"{[it.get('url', '?') for it in items]}"
        )

    def test_unminified_js_clean(
        self,
        lighthouse_nginx_results: dict,
    ):
        """No unminified JavaScript on the optimized page."""
        _skip_if_no_lighthouse()

        audit = get_audit(lighthouse_nginx_results, "unminified-javascript")
        items = audit.get("details", {}).get("items", []) if audit else []
        assert len(items) == 0, (
            f"Unminified JS found ({len(items)} item(s)): "
            f"{[it.get('url', '?') for it in items]}"
        )


# ---------------------------------------------------------------------------
# TestImageFormats
# ---------------------------------------------------------------------------


class TestImageFormats:
    """Next-gen image format audit must pass for the optimized page."""

    def test_uses_webp_images(
        self,
        lighthouse_nginx_results: dict,
    ):
        """Optimized page should serve images in next-gen formats (WebP/AVIF)."""
        _skip_if_no_lighthouse()

        audit = get_audit(lighthouse_nginx_results, "uses-webp-images")
        # Newer Lighthouse versions merged this into "modern-image-formats".
        if audit is None:
            audit = get_audit(lighthouse_nginx_results, "modern-image-formats")
        items = audit.get("details", {}).get("items", []) if audit else []
        assert len(items) == 0, (
            f"Images not using next-gen formats ({len(items)} item(s)): "
            f"{[it.get('url', '?') for it in items[:5]]}"
        )


# ---------------------------------------------------------------------------
# TestCoreWebVitals
# ---------------------------------------------------------------------------


class TestCoreWebVitals:
    """Core Web Vitals thresholds for the optimized page."""

    def test_lcp_threshold(
        self,
        lighthouse_nginx_results: dict,
    ):
        """Largest Contentful Paint must be under 4000 ms."""
        _skip_if_no_lighthouse()

        audit = get_audit(lighthouse_nginx_results, "largest-contentful-paint")
        assert audit is not None, "LCP audit not found in report"
        lcp_ms = audit.get("numericValue", float("inf"))
        assert lcp_ms <= 4000, f"LCP too high: {lcp_ms:.0f} ms > 4000 ms"

    def test_cls_threshold(
        self,
        lighthouse_nginx_results: dict,
    ):
        """Cumulative Layout Shift must be under 0.25."""
        _skip_if_no_lighthouse()

        audit = get_audit(lighthouse_nginx_results, "cumulative-layout-shift")
        assert audit is not None, "CLS audit not found in report"
        cls_val = audit.get("numericValue", float("inf"))
        assert cls_val <= 0.25, f"CLS too high: {cls_val:.3f} > 0.25"

    def test_lcp_improved(
        self,
        lighthouse_origin_results: dict,
        lighthouse_nginx_results: dict,
    ):
        """LCP should not regress compared to origin."""
        _skip_if_no_lighthouse()

        origin_audit = get_audit(lighthouse_origin_results, "largest-contentful-paint")
        nginx_audit = get_audit(lighthouse_nginx_results, "largest-contentful-paint")
        if origin_audit is None or nginx_audit is None:
            pytest.skip("LCP audit missing from one or both reports")

        origin_lcp = origin_audit.get("numericValue", 0)
        nginx_lcp = nginx_audit.get("numericValue", 0)
        # Allow 10% tolerance for Lighthouse variability.
        assert nginx_lcp <= origin_lcp * 1.10, (
            f"LCP regressed: origin={origin_lcp:.0f} ms, "
            f"nginx={nginx_lcp:.0f} ms "
            f"(>{origin_lcp * 1.10:.0f} ms with 10% tolerance)"
        )


# ---------------------------------------------------------------------------
# TestServerTiming
# ---------------------------------------------------------------------------


class TestServerTiming:
    """Server response time for cached content should be fast."""

    def test_server_response_time(
        self,
        lighthouse_nginx_results: dict,
    ):
        """Server response time audit score should be passing (>= 0.5)."""
        _skip_if_no_lighthouse()

        audit = get_audit(lighthouse_nginx_results, "server-response-time")
        if audit is None:
            pytest.skip("server-response-time audit not found")

        score = audit.get("score", 0.0)
        numeric = audit.get("numericValue", -1)
        assert score >= 0.5, (
            f"Server response time too slow: score={score:.2f}, value={numeric:.0f} ms"
        )

    def test_server_response_improved(
        self,
        lighthouse_origin_results: dict,
        lighthouse_nginx_results: dict,
    ):
        """Cached nginx responses should not be slower than origin."""
        _skip_if_no_lighthouse()

        origin_audit = get_audit(lighthouse_origin_results, "server-response-time")
        nginx_audit = get_audit(lighthouse_nginx_results, "server-response-time")
        if origin_audit is None or nginx_audit is None:
            pytest.skip("server-response-time audit missing from one or both")

        origin_ms = origin_audit.get("numericValue", 0)
        nginx_ms = nginx_audit.get("numericValue", 0)
        # Allow 20% tolerance for network jitter in local Docker.
        assert nginx_ms <= origin_ms * 1.20, (
            f"Server response time regressed: "
            f"origin={origin_ms:.0f} ms, nginx={nginx_ms:.0f} ms"
        )


# ---------------------------------------------------------------------------
# TestBudgetCompliance
# ---------------------------------------------------------------------------


class TestBudgetCompliance:
    """Resource budget checks against budget.json."""

    BUDGET_FILE = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "budget.json"
    )

    @staticmethod
    def _load_budget() -> list[dict]:
        budget_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "budget.json"
        )
        with open(budget_path) as fh:
            return json.load(fh)

    @staticmethod
    def _resource_summary(report: dict) -> dict[str, int]:
        """Extract resource sizes in KB from the Lighthouse report.

        Uses the ``resource-summary`` audit when available, falling
        back to ``network-requests`` for a rough breakdown.
        """
        audit = get_audit(report, "resource-summary")
        if audit is None:
            return {}
        sizes: dict[str, int] = {}
        for item in audit.get("details", {}).get("items", []):
            rtype = item.get("resourceType", "")
            transfer_size = item.get("transferSize", 0)
            sizes[rtype] = transfer_size // 1024  # bytes -> KB
        return sizes

    def test_total_size_budget(
        self,
        lighthouse_nginx_results: dict,
        budget: list[dict],
    ):
        """Total resource size under budget."""
        _skip_if_no_lighthouse()

        sizes = self._resource_summary(lighthouse_nginx_results)
        if not sizes:
            pytest.skip("resource-summary audit not available")

        total_kb = sum(sizes.values())
        budget_kb = 500
        for entry in budget:
            for rs in entry.get("resourceSizes", []):
                if rs["resourceType"] == "total":
                    budget_kb = rs["budget"]
        assert total_kb <= budget_kb, (
            f"Total resource size {total_kb} KB exceeds budget of {budget_kb} KB"
        )

    def test_script_size_budget(
        self,
        lighthouse_nginx_results: dict,
        budget: list[dict],
    ):
        """Script resource size under budget."""
        _skip_if_no_lighthouse()

        sizes = self._resource_summary(lighthouse_nginx_results)
        if not sizes:
            pytest.skip("resource-summary audit not available")

        script_kb = sizes.get("script", 0)
        budget_kb = 100
        for entry in budget:
            for rs in entry.get("resourceSizes", []):
                if rs["resourceType"] == "script":
                    budget_kb = rs["budget"]
        assert script_kb <= budget_kb, (
            f"Script size {script_kb} KB exceeds budget of {budget_kb} KB"
        )

    def test_stylesheet_size_budget(
        self,
        lighthouse_nginx_results: dict,
        budget: list[dict],
    ):
        """Stylesheet resource size under budget."""
        _skip_if_no_lighthouse()

        sizes = self._resource_summary(lighthouse_nginx_results)
        if not sizes:
            pytest.skip("resource-summary audit not available")

        css_kb = sizes.get("stylesheet", 0)
        budget_kb = 50
        for entry in budget:
            for rs in entry.get("resourceSizes", []):
                if rs["resourceType"] == "stylesheet":
                    budget_kb = rs["budget"]
        assert css_kb <= budget_kb, (
            f"Stylesheet size {css_kb} KB exceeds budget of {budget_kb} KB"
        )

    def test_image_size_budget(
        self,
        lighthouse_nginx_results: dict,
        budget: list[dict],
    ):
        """Image resource size under budget."""
        _skip_if_no_lighthouse()

        sizes = self._resource_summary(lighthouse_nginx_results)
        if not sizes:
            pytest.skip("resource-summary audit not available")

        img_kb = sizes.get("image", 0)
        budget_kb = 300
        for entry in budget:
            for rs in entry.get("resourceSizes", []):
                if rs["resourceType"] == "image":
                    budget_kb = rs["budget"]
        assert img_kb <= budget_kb, (
            f"Image size {img_kb} KB exceeds budget of {budget_kb} KB"
        )

    def test_font_size_budget(
        self,
        lighthouse_nginx_results: dict,
        budget: list[dict],
    ):
        """Font resource size under budget."""
        _skip_if_no_lighthouse()

        sizes = self._resource_summary(lighthouse_nginx_results)
        if not sizes:
            pytest.skip("resource-summary audit not available")

        font_kb = sizes.get("font", 0)
        budget_kb = 100
        for entry in budget:
            for rs in entry.get("resourceSizes", []):
                if rs["resourceType"] == "font":
                    budget_kb = rs["budget"]
        assert font_kb <= budget_kb, (
            f"Font size {font_kb} KB exceeds budget of {budget_kb} KB"
        )
