# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""mod_pagespeed E2E Tests: Story 4 — Configuration Changes.

Tests the worker's /v1/config API for runtime config reload, validation,
persistence, and edge cases.  Runs against the E2E Docker Compose stack
(nginx :8083, worker API :9881, origin :8081).

All tests that modify config use the ``restore_config`` fixture so the
worker is returned to default settings after each test.
"""

import json
import time

import pytest
from conftest import (
    DEFAULT_CONFIG,
    docker_exec,
    read_container_file,
    restart_worker,
    stat_container_file,
)

# ---------------------------------------------------------------------------
# 4a. Quality Settings
# ---------------------------------------------------------------------------


class TestQualitySettings:
    """PATCH / GET round-trips for image quality fields."""

    def test_jpeg_quality_change_applied(self, restore_config):
        api = restore_config
        resp = api.patch_config(jpeg_quality=50)
        assert "jpeg_quality" in resp["applied"]
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 50

    def test_webp_quality_change_applied(self, restore_config):
        api = restore_config
        resp = api.patch_config(webp_quality=40)
        assert "webp_quality" in resp["applied"]
        cfg = api.get_config()
        assert cfg["webp_quality"] == 40

    def test_avif_quality_change_applied(self, restore_config):
        api = restore_config
        resp = api.patch_config(avif_quality=30)
        assert "avif_quality" in resp["applied"]
        cfg = api.get_config()
        assert cfg["avif_quality"] == 30

    def test_savedata_quality_change_applied(self, restore_config):
        api = restore_config
        resp = api.patch_config(
            savedata_jpeg_quality=40,
            savedata_webp_quality=30,
            savedata_avif_quality=25,
        )
        assert "savedata_jpeg_quality" in resp["applied"]
        assert "savedata_webp_quality" in resp["applied"]
        assert "savedata_avif_quality" in resp["applied"]
        cfg = api.get_config()
        assert cfg["savedata_jpeg_quality"] == 40
        assert cfg["savedata_webp_quality"] == 30
        assert cfg["savedata_avif_quality"] == 25

    def test_quality_clamped_to_range(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=200)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 100

    def test_quality_min_clamped(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=0)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 1

    def test_negative_quality_clamped(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=-10)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 1


# ---------------------------------------------------------------------------
# 4b. Content Analysis & Quality Verification
# ---------------------------------------------------------------------------


class TestContentAnalysis:
    """Content analysis, quality verification, and SSIMULACRA2 settings."""

    def test_content_analysis_toggle(self, restore_config):
        api = restore_config
        api.patch_config(content_analysis=False)
        cfg = api.get_config()
        assert cfg["content_analysis"] is False

    def test_quality_verify_toggle(self, restore_config):
        api = restore_config
        api.patch_config(quality_verify=False)
        cfg = api.get_config()
        assert cfg["quality_verify"] is False

    def test_target_ssimulacra2_change(self, restore_config):
        api = restore_config
        api.patch_config(target_ssimulacra2=85.0)
        cfg = api.get_config()
        assert cfg["target_ssimulacra2"] == pytest.approx(85.0)

    def test_ssimulacra2_high_warns(self, restore_config):
        api = restore_config
        resp = api.patch_config(target_ssimulacra2=95.0)
        assert len(resp.get("warnings", [])) > 0


# ---------------------------------------------------------------------------
# 4c. Learned Quality Prediction
# ---------------------------------------------------------------------------


class TestLearnedQuality:
    """Learned quality prediction toggles."""

    def test_learned_quality_global_toggle(self, restore_config):
        api = restore_config
        api.patch_config(learned_quality=False)
        cfg = api.get_config()
        assert cfg["learned_quality"] is False

    def test_learned_quality_per_format_toggle(self, restore_config):
        api = restore_config
        api.patch_config(learned_quality_jpeg=False)
        cfg = api.get_config()
        assert cfg["learned_quality_jpeg"] is False
        # Other per-format toggles remain unchanged.
        assert cfg["learned_quality_webp"] is True
        assert cfg["learned_quality_avif"] is True

    def test_savedata_score_reduction_applied(self, restore_config):
        api = restore_config
        api.patch_config(savedata_score_reduction=20.0)
        cfg = api.get_config()
        assert cfg["savedata_score_reduction"] == pytest.approx(20.0)


# ---------------------------------------------------------------------------
# 4d. Viewport Configuration
# ---------------------------------------------------------------------------


class TestViewportConfig:
    """Viewport breakpoint settings."""

    def test_mobile_width_change_applied(self, restore_config):
        api = restore_config
        api.patch_config(mobile_width=320)
        cfg = api.get_config()
        assert cfg["mobile_width"] == 320

    def test_tablet_width_change_applied(self, restore_config):
        api = restore_config
        api.patch_config(tablet_width=600)
        cfg = api.get_config()
        assert cfg["tablet_width"] == 600

    def test_viewport_width_min_rejected(self, restore_config):
        api = restore_config
        resp = api.patch_config(mobile_width=50)
        assert "mobile_width" in resp.get("rejected", {})

    def test_viewport_width_zero_accepted(self, restore_config):
        api = restore_config
        resp = api.patch_config(mobile_width=0)
        assert "mobile_width" in resp["applied"]
        cfg = api.get_config()
        assert cfg["mobile_width"] == 0


# ---------------------------------------------------------------------------
# 4e. Proactive Variant Generation
# ---------------------------------------------------------------------------


class TestProactiveVariants:
    """Proactive variant generation toggles."""

    def test_proactive_image_variants_toggle(self, restore_config):
        api = restore_config
        api.patch_config(proactive_image_variants=False)
        cfg = api.get_config()
        assert cfg["proactive_image_variants"] is False

    def test_proactive_viewport_variants_toggle(self, restore_config):
        api = restore_config
        api.patch_config(proactive_viewport_variants=False)
        cfg = api.get_config()
        assert cfg["proactive_viewport_variants"] is False

    def test_proactive_savedata_variants_toggle(self, restore_config):
        api = restore_config
        api.patch_config(proactive_savedata_variants=False)
        cfg = api.get_config()
        assert cfg["proactive_savedata_variants"] is False

    def test_proactive_density_variants_toggle(self, restore_config):
        api = restore_config
        api.patch_config(proactive_density_variants=False)
        cfg = api.get_config()
        assert cfg["proactive_density_variants"] is False


# ---------------------------------------------------------------------------
# 4f. HTML Transformations
# ---------------------------------------------------------------------------


class TestHtmlTransformations:
    """HTML feature toggles."""

    def test_disable_html_applied(self, restore_config):
        # The legacy key still works: true is html_mode off.
        api = restore_config
        api.patch_config(disable_html=True)
        cfg = api.get_config()
        assert cfg["disable_html"] is True
        assert cfg["html_mode"] == "off"
        api.patch_config(disable_html=False)
        cfg = api.get_config()
        assert cfg["disable_html"] is False
        assert cfg["html_mode"] == "full"

    def test_html_mode_default_is_full(self, worker_api):
        cfg = worker_api.get_config()
        assert cfg["html_mode"] == "full"
        assert cfg["disable_html"] is False

    @pytest.mark.parametrize(
        "mode,disable_html",
        [
            ("off", True),
            ("markdown-static", False),
            ("markdown-rendered", False),
            ("full", False),
        ],
    )
    def test_html_mode_applied(self, restore_config, mode, disable_html):
        api = restore_config
        result = api.patch_config(html_mode=mode)
        assert result["applied"]["html_mode"] == mode
        cfg = api.get_config()
        assert cfg["html_mode"] == mode
        assert cfg["disable_html"] is disable_html

    def test_html_mode_unknown_value_rejected(self, restore_config):
        api = restore_config
        result = api.patch_config(html_mode="markdown")
        assert "html_mode" in result["rejected"]
        assert api.get_config()["html_mode"] == "full"

    def test_html_mode_wins_over_disable_html(self, restore_config):
        api = restore_config
        result = api.patch_config(html_mode="markdown-static", disable_html=True)
        assert result["applied"]["html_mode"] == "markdown-static"
        assert "disable_html" not in result["applied"]
        assert result["warnings"]
        assert api.get_config()["html_mode"] == "markdown-static"

    def test_disable_lazy_load_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_lazy_load=True)
        cfg = api.get_config()
        assert cfg["disable_lazy_load"] is True

    def test_disable_image_dimensions_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_image_dimensions=True)
        cfg = api.get_config()
        assert cfg["disable_image_dimensions"] is True

    def test_disable_lcp_preload_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_lcp_preload=True)
        cfg = api.get_config()
        assert cfg["disable_lcp_preload"] is True

    def test_disable_preconnect_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_preconnect_injection=True)
        cfg = api.get_config()
        assert cfg["disable_preconnect_injection"] is True

    def test_enable_speculation_rules_applied(self, restore_config):
        api = restore_config
        api.patch_config(enable_speculation_rules=True)
        cfg = api.get_config()
        assert cfg["enable_speculation_rules"] is True

    def test_disable_css_import_flattening_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_css_import_flattening=True)
        cfg = api.get_config()
        assert cfg["disable_css_import_flattening"] is True


# ---------------------------------------------------------------------------
# 4g. Content Type Toggles
# ---------------------------------------------------------------------------


class TestContentTypeToggles:
    """Per-content-type disable/enable toggles."""

    def test_disable_css_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_css=True)
        cfg = api.get_config()
        assert cfg["disable_css"] is True

    def test_disable_js_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_js=True)
        cfg = api.get_config()
        assert cfg["disable_js"] is True

    def test_disable_image_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_image=True)
        cfg = api.get_config()
        assert cfg["disable_image"] is True

    def test_reenable_css_applied(self, restore_config):
        api = restore_config
        api.patch_config(disable_css=True)
        cfg = api.get_config()
        assert cfg["disable_css"] is True
        api.patch_config(disable_css=False)
        cfg = api.get_config()
        assert cfg["disable_css"] is False


# ---------------------------------------------------------------------------
# 4h. Compression Levels
# ---------------------------------------------------------------------------


class TestCompressionLevels:
    """Gzip and Brotli compression level settings."""

    def test_gzip_level_change_applied(self, restore_config):
        api = restore_config
        api.patch_config(gzip_level=9)
        cfg = api.get_config()
        assert cfg["gzip_level"] == 9

    def test_brotli_level_change_applied(self, restore_config):
        api = restore_config
        api.patch_config(brotli_level=11)
        cfg = api.get_config()
        assert cfg["brotli_level"] == 11

    def test_gzip_level_zero_disables(self, restore_config):
        api = restore_config
        api.patch_config(gzip_level=0)
        cfg = api.get_config()
        assert cfg["gzip_level"] == 0

    def test_brotli_level_zero_disables(self, restore_config):
        api = restore_config
        api.patch_config(brotli_level=0)
        cfg = api.get_config()
        assert cfg["brotli_level"] == 0


# ---------------------------------------------------------------------------
# 4i. SVG Auto-Vectorization
# ---------------------------------------------------------------------------


class TestSvgConfig:
    """SVG auto-vectorization configuration."""

    def test_svg_mode_change_applied(self, restore_config):
        api = restore_config
        api.patch_config(svg_mode="auto")
        cfg = api.get_config()
        assert cfg["svg_mode"] == "auto"

    def test_svg_mode_invalid_rejected(self, restore_config):
        api = restore_config
        resp = api.patch_config(svg_mode="invalid")
        assert "svg_mode" in resp.get("rejected", {})

    def test_svg_candidacy_threshold_applied(self, restore_config):
        api = restore_config
        api.patch_config(svg_candidacy_threshold=0)
        cfg = api.get_config()
        assert cfg["svg_candidacy_threshold"] == 0

    def test_svg_max_pixels_applied(self, restore_config):
        api = restore_config
        api.patch_config(svg_max_pixels=1024)
        cfg = api.get_config()
        assert cfg["svg_max_pixels"] == 1024

    def test_svg_exclude_lcp_toggle(self, restore_config):
        api = restore_config
        api.patch_config(svg_exclude_lcp=False)
        cfg = api.get_config()
        assert cfg["svg_exclude_lcp"] is False


# ---------------------------------------------------------------------------
# 4j. Security Limits
# ---------------------------------------------------------------------------


class TestSecurityLimits:
    """Max size / length security limits."""

    def test_max_html_size_applied(self, restore_config):
        api = restore_config
        api.patch_config(max_html_size=1024)
        cfg = api.get_config()
        assert cfg["max_html_size"] == 1024

    def test_max_css_size_applied(self, restore_config):
        api = restore_config
        # min is 1024 (1 KB), so use 2048
        api.patch_config(max_css_size=2048)
        cfg = api.get_config()
        assert cfg["max_css_size"] == 2048

    def test_max_image_size_applied(self, restore_config):
        api = restore_config
        api.patch_config(max_image_size=1048576)
        cfg = api.get_config()
        assert cfg["max_image_size"] == 1048576

    def test_max_url_length_applied(self, restore_config):
        api = restore_config
        # min is 64, so 256 is fine
        api.patch_config(max_url_length=256)
        cfg = api.get_config()
        assert cfg["max_url_length"] == 256


# ---------------------------------------------------------------------------
# 4k. Non-Reloadable Fields
# ---------------------------------------------------------------------------


class TestNonReloadableFields:
    """Fields that cannot be changed at runtime (threads, paths)."""

    def test_num_threads_rejected(self, restore_config):
        api = restore_config
        resp = api.patch_config(num_threads=4)
        assert "num_threads" in resp.get("rejected", {})
        assert "runtime" in resp["rejected"]["num_threads"].lower()

    def test_cache_path_rejected(self, restore_config):
        api = restore_config
        resp = api.patch_config(cache_path="/tmp")
        assert "cache_path" in resp.get("rejected", {})

    def test_socket_path_rejected(self, restore_config):
        api = restore_config
        resp = api.patch_config(socket_path="/tmp/x")
        assert "socket_path" in resp.get("rejected", {})

    def test_mixed_reloadable_and_non(self, restore_config):
        api = restore_config
        resp = api.patch_config(jpeg_quality=50, num_threads=4)
        assert "jpeg_quality" in resp["applied"]
        assert "num_threads" in resp.get("rejected", {})

    def test_mixed_persists_correctly(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=50, num_threads=4)
        restart_worker(timeout_sec=30)
        time.sleep(2)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 50
        # num_threads is a non-reloadable field; it should not appear in
        # the persisted config (rejected fields are not persisted).
        assert "num_threads" not in cfg or cfg["num_threads"] != 4


# ---------------------------------------------------------------------------
# 4l. Persistence Across Restarts
# ---------------------------------------------------------------------------


class TestConfigPersistence:
    """Config file on disk and restart survival."""

    def test_config_persists_to_file(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=42)
        raw = read_container_file("worker", "/shared/pagespeed.json")
        assert raw is not None, "Config file not found in container"
        data = json.loads(raw)
        assert data["jpeg_quality"] == 42

    def test_config_file_permissions(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=42)
        perms = stat_container_file("worker", "/shared/pagespeed.json")
        assert perms is not None
        assert perms == "644"

    def test_config_survives_restart(self, restore_config):
        api = restore_config
        api.patch_config(jpeg_quality=42, webp_quality=33)
        restart_worker(timeout_sec=30)
        time.sleep(2)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 42
        assert cfg["webp_quality"] == 33

    def test_multiple_fields_persist(self, restore_config):
        api = restore_config
        api.patch_config(
            jpeg_quality=55,
            webp_quality=44,
            avif_quality=33,
            gzip_level=9,
        )
        restart_worker(timeout_sec=30)
        time.sleep(2)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == 55
        assert cfg["webp_quality"] == 44
        assert cfg["avif_quality"] == 33
        assert cfg["gzip_level"] == 9

    def test_corrupted_config_falls_back_to_defaults(self, restore_config):
        api = restore_config
        # Write garbage into the config file.
        docker_exec(
            "worker",
            ["sh", "-c", "echo 'GARBAGE' > /shared/pagespeed.json"],
        )
        restart_worker(timeout_sec=30)
        time.sleep(2)
        cfg = api.get_config()
        assert cfg["jpeg_quality"] == DEFAULT_CONFIG["jpeg_quality"]


# ---------------------------------------------------------------------------
# 4m. Validation & Edge Cases
# ---------------------------------------------------------------------------


class TestConfigValidation:
    """Malformed input, type mismatches, and boundary values."""

    def test_invalid_json_returns_400(self, restore_config):
        api = restore_config
        resp = api.patch_config_raw("{invalid json")
        assert resp.status_code == 400

    def test_unknown_field_ignored(self, restore_config):
        api = restore_config
        resp = api.patch_config(nonexistent_field=1)
        # Unknown fields should not appear in applied.
        assert "nonexistent_field" not in resp.get("applied", {})

    def test_nan_rejected(self, restore_config):
        """Infinity (1e999 overflows to inf) should be rejected as non-finite."""
        api = restore_config
        resp = api.patch_config_raw('{"target_ssimulacra2": 1e999}')
        # Either rejected with a message, or 400 from the JSON parser.
        if resp.status_code == 200:
            body = resp.json()
            assert "target_ssimulacra2" in body.get("rejected", {})
        else:
            assert resp.status_code == 400

    def test_wrong_type_rejected(self, restore_config):
        api = restore_config
        resp = api.patch_config_raw('{"jpeg_quality": "hello"}')
        if resp.status_code == 200:
            body = resp.json()
            assert "jpeg_quality" in body.get("rejected", {})
        else:
            assert resp.status_code == 400

    def test_empty_patch_succeeds(self, restore_config):
        api = restore_config
        resp = api.patch_config()
        assert "applied" in resp
        assert len(resp["applied"]) == 0

    def test_integer_overflow_handled(self, restore_config):
        api = restore_config
        resp = api.patch_config_raw('{"jpeg_quality": 999999999999}')
        # The worker may either clamp the value or reject the request.
        # Either is acceptable; a crash (5xx) is not.
        assert resp.status_code in (200, 400), (
            f"Expected 200 or 400 for overflow, got: {resp.status_code}"
        )
        if resp.status_code == 200:
            cfg = api.get_config()
            # If accepted, it should be clamped to a valid range [1, 100].
            assert 1 <= cfg["jpeg_quality"] <= 100

    def test_deeply_nested_json_rejected(self, restore_config):
        api = restore_config
        # Build 100 levels of nesting.
        body = '{"a":' * 100 + "1" + "}" * 100
        resp = api.patch_config_raw(body)
        assert resp.status_code == 400


# ---------------------------------------------------------------------------
# 4n. API Authentication (default open)
# ---------------------------------------------------------------------------


class TestApiAuthentication:
    """Verify that the E2E worker (started without --api-token) is open."""

    def test_health_always_accessible(self, worker_api):
        health = worker_api.get_health()
        assert health is not None

    def test_config_get_accessible_without_token(self, worker_api):
        cfg = worker_api.get_config()
        assert "jpeg_quality" in cfg

    def test_config_patch_accessible_without_token(self, restore_config):
        api = restore_config
        resp = api.patch_config()
        assert "applied" in resp
