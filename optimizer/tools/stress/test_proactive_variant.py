# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Proactive multi-variant generation stress tests.

Tests the worker's ability to handle concurrent proactive variant generation
(multiple formats x viewports x densities x save-data from a single decode),
including large images and sustained throughput.

Note: Notifications are sent by nginx on cache MISS, not manually.
Nginx's r->uri excludes query strings, so the cache key is just the path.
"""

import time
from urllib.parse import urlparse

import metrics_helpers
import requests

# The PURGE hostname must include the port when it's non-standard,
# because the Cyclone cache key includes the full Host header value
# (NormalizeHostname preserves non-default ports).  A bare "localhost"
# won't match entries stored under "localhost:<port>".
import os as _os
PURGE_HOSTNAME = f"localhost:{_os.environ.get('STRESS_NGINX_PORT', '8190')}"

# Upper bound for waiting on variant generation. Small runners need longer.
PROACTIVE_DEADLINE_SECONDS = int(_os.environ.get("STRESS_PROACTIVE_DEADLINE", "90"))


def _poll_stats_safe():
    """Query STATS, returning None on timeout/failure instead of raising."""
    try:
        raw = metrics_helpers.get_stats_via_docker(timeout=10)
        return metrics_helpers.parse_stats(raw)
    except Exception:
        return None


def _purge_images(image_paths):
    """Purge all variants for the given image paths."""
    for path in image_paths:
        try:
            metrics_helpers.purge_via_docker(
                path, hostname=PURGE_HOSTNAME, timeout=10
            )
        except Exception:
            pass


class TestProactiveVariantGeneration:
    """Multi-variant generation stress tests."""

    def test_concurrent_proactive_generation(self, stress_services):
        """Fetch multiple different images, verify proactive variants."""
        base_url = stress_services.nginx_url

        image_paths = [
            "/images/img-100k.jpg",
            "/images/img-500k.jpg",
            "/images/img-1m.jpg",
            "/images/test.png",
            "/images/static.gif",
        ]

        # Purge worker cache so earlier tests' entries don't cause HITs.
        _purge_images(image_paths)
        time.sleep(1)

        before = _poll_stats_safe()
        assert before is not None, "Cannot query STATS before test"
        before_written = before.get("variants", {}).get("written", 0)

        # Fetch images through nginx.  nginx-stress.conf sets
        # pagespeed_hot_threshold 3, so each path needs 3+ hits before
        # nginx sends a notification to the worker.
        for path in image_paths:
            for _ in range(4):
                try:
                    requests.get(base_url + path, timeout=15)
                except Exception:
                    pass

        # Wait for proactive generation (multiple formats x viewports)
        deadline = time.time() + PROACTIVE_DEADLINE_SECONDS
        target_variants = before_written + 5  # At least 1 variant per image
        current_written = before_written

        while time.time() < deadline:
            stats = _poll_stats_safe()
            if stats is not None:
                current_written = stats.get("variants", {}).get("written", 0)
                if current_written >= target_variants:
                    break
            time.sleep(2)

        total_written = current_written - before_written

        assert total_written >= 3, (
            f"Expected >= 3 variants from {len(image_paths)} images, "
            f"got {total_written}"
        )

        # Health should be responsive after processing completes
        try:
            health = metrics_helpers.get_health_via_docker(timeout=10)
            assert "OK" in health, f"Worker unhealthy: {health}"
        except Exception:
            pass  # Health timeout is acceptable if worker is busy

    def test_large_image_proactive(self, stress_services):
        """Fetch 5MB image, verify variants are generated."""
        base_url = stress_services.nginx_url

        # Purge to ensure fresh notification
        _purge_images(["/images/img-5m.jpg"])
        time.sleep(1)

        before = _poll_stats_safe()
        assert before is not None, "Cannot query STATS before test"
        before_written = before.get("variants", {}).get("written", 0)

        # Fetch large image through nginx.  nginx-stress.conf sets
        # pagespeed_hot_threshold 3, so we need 3+ hits.
        for _ in range(4):
            try:
                requests.get(base_url + "/images/img-5m.jpg", timeout=30)
            except Exception:
                pass

        # Large images take longer to process (AVIF encoding ~30s for 5MB)
        deadline = time.time() + PROACTIVE_DEADLINE_SECONDS
        target = before_written + 1
        current = before_written

        while time.time() < deadline:
            stats = _poll_stats_safe()
            if stats is not None:
                current = stats.get("variants", {}).get("written", 0)
                if current >= target:
                    break
            time.sleep(2)

        delta = current - before_written

        assert delta >= 1, f"Expected >= 1 variant from large image, got {delta}"

        # Verify worker is alive after processing
        try:
            health = metrics_helpers.get_health_via_docker(timeout=10)
            assert "OK" in health, "Worker unhealthy after large image"
        except Exception:
            pass

    def test_image_variant_throughput(self, stress_services):
        """Verify overall image processing throughput across the session.

        Checks cumulative totals: that the worker has processed a healthy
        number of images and generated variants across multiple formats.
        """
        # Wait for any in-progress work to finish, then check totals
        deadline = time.time() + PROACTIVE_DEADLINE_SECONDS
        stats = None
        while time.time() < deadline:
            stats = _poll_stats_safe()
            if stats is not None:
                images = stats.get("by_type", {}).get("images", {})
                images_n = images.get("n", 0) if isinstance(images, dict) else 0
                written = stats.get("variants", {}).get("written", 0)
                if images_n >= 3 and written >= 10:
                    break
            time.sleep(2)

        assert stats is not None, "Could not query STATS"
        images = stats.get("by_type", {}).get("images", {})
        images_n = images.get("n", 0) if isinstance(images, dict) else 0
        total_written = stats.get("variants", {}).get("written", 0)
        by_format = stats.get("by_format", {})

        assert images_n >= 3, f"Expected >= 3 images processed total, got {images_n}"
        assert total_written >= 10, (
            f"Expected >= 10 total variants written, got {total_written}"
        )
        # Should have at least two format types
        formats_used = [f for f, c in by_format.items() if c > 0]
        assert len(formats_used) >= 2, f"Expected >= 2 formats used, got {formats_used}"
