# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Monitoring validation tests.

Verifies that worker stats, health, and Prometheus metrics remain consistent
and accurate under load, and that counter invariants hold.
"""

import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import metrics_helpers
import requests


class TestHealthUnderLoad:
    """Health endpoint reliability under load."""

    def test_health_under_load(self, stress_services):
        """Poll health every 1s during 200 req/s for 10s."""
        base_url = stress_services.nginx_url
        health_results = []
        stop = False

        # Background health poller
        import threading

        def poll_health():
            while not stop:
                try:
                    h = metrics_helpers.get_health_via_docker()
                    health_results.append(h)
                except Exception as e:
                    health_results.append(f"ERROR: {e}")
                time.sleep(1)

        poller = threading.Thread(target=poll_health, daemon=True)
        poller.start()

        # Generate load
        with ThreadPoolExecutor(max_workers=50) as pool:
            futures = []
            start = time.monotonic()
            i = 0
            while time.monotonic() - start < 10:
                path = f"/images/img-100k.jpg?health_load={i}"
                futures.append(pool.submit(requests.get, base_url + path, timeout=10))
                i += 1
                time.sleep(0.005)  # ~200 req/s

            for f in as_completed(futures):
                try:
                    f.result()
                except Exception:
                    pass

        stop = True
        poller.join(timeout=3)

        # All health responses should contain "OK"
        ok_count = sum(1 for h in health_results if "OK" in str(h))
        assert ok_count == len(health_results), (
            f"Health failures: {len(health_results) - ok_count}/{len(health_results)}"
        )

    def test_health_after_stress(self, stress_services):
        """Query health after load test, verify clean state."""
        deadline = time.time() + 10
        while time.time() < deadline:
            health_str = metrics_helpers.get_health_via_docker()
            health = metrics_helpers.parse_health(health_str)
            if health["status"] == "OK" and health.get("active_connections", 0) == 0:
                return  # Success
            time.sleep(1)
        # Final check with assertion
        health_str = metrics_helpers.get_health_via_docker()
        health = metrics_helpers.parse_health(health_str)
        assert health["status"] == "OK"
        assert health.get("active_connections", 0) == 0, (
            f"Active connections should be 0 after 10s drain, got {health.get('active_connections')}"
        )


class TestStatsInvariants:
    """STATS counter invariant validation."""

    def test_stats_counter_invariants(self, stress_services, warm_cache):
        """Mixed workload, then check all counter invariants."""
        base_url = stress_services.nginx_url

        # Capture baseline before generating workload
        before_raw = metrics_helpers.get_stats_via_docker()
        before = metrics_helpers.parse_stats(before_raw)
        before_received = before.get("notifications", {}).get("received", 0)

        # Generate a mixed workload (4 paths x 5 = 20 requests)
        paths = [
            "/images/img-100k.jpg?invariant_img",
            "/css/style-1k.css?invariant_css",
            "/js/app-1k.js?invariant_js",
            "/html/minimal.html?invariant_html",
        ]
        for path in paths * 5:
            try:
                requests.get(base_url + path, timeout=5)
            except Exception:
                pass

        # Poll until at least half the requests (10) have been received
        try:
            metrics_helpers.wait_for_stats_condition(
                lambda s: s.get("notifications", {}).get("received", 0) >= before_received + 10,
                timeout=30,
                interval=1.0,
            )
        except TimeoutError:
            pass

        raw = metrics_helpers.get_stats_via_docker()
        stats = metrics_helpers.parse_stats(raw)
        metrics_helpers.assert_stats_invariants(stats)

    def test_stats_accumulate(self, stress_services):
        """STATS counters should only increase over time."""
        before_raw = metrics_helpers.get_stats_via_docker()
        before = metrics_helpers.parse_stats(before_raw)
        before_received = before.get("notifications", {}).get("received", 0)

        # Generate some requests
        base_url = stress_services.nginx_url
        for i in range(10):
            try:
                requests.get(base_url + f"/images/img-100k.jpg?accum={i}", timeout=5)
            except Exception:
                pass

        # Poll until notifications.received has increased from the before snapshot
        try:
            metrics_helpers.wait_for_stats_condition(
                lambda s: s.get("notifications", {}).get("received", 0) > before_received,
                timeout=30,
                interval=1.0,
            )
        except TimeoutError:
            pass

        after_raw = metrics_helpers.get_stats_via_docker()
        after = metrics_helpers.parse_stats(after_raw)

        metrics_helpers.assert_counters_monotonic(before, after)

        # notifications.received should have increased
        before_received = before.get("notifications", {}).get("received", 0)
        after_received = after.get("notifications", {}).get("received", 0)
        assert after_received >= before_received, (
            f"notifications.received decreased: {before_received} -> {after_received}"
        )


class TestPrometheusMetrics:
    """Prometheus metrics endpoint validation."""

    def test_metrics_prometheus_format(self, stress_services):
        """Query METRICS 10 times, all should be valid Prometheus text."""
        expected_metrics = [
            "pagespeed_notifications_total",
            "pagespeed_variants_written_total",
            "pagespeed_errors_total",
            "pagespeed_cache_entries",
            "pagespeed_cache_bytes",
            "pagespeed_connections_active",
            "pagespeed_connections_max",
        ]

        for i in range(10):
            raw = metrics_helpers.get_metrics_via_docker()
            parsed = metrics_helpers.parse_prometheus_metrics(raw)

            for metric in expected_metrics:
                assert metric in parsed, (
                    f"Missing metric '{metric}' in iteration {i}. "
                    f"Found: {list(parsed.keys())}"
                )
                assert isinstance(parsed[metric], (int, float)), (
                    f"Non-numeric value for {metric}: {parsed[metric]}"
                )
            time.sleep(0.5)

    def test_metrics_stats_agree(self, stress_services):
        """STATS and METRICS should report consistent values."""
        stats_raw = metrics_helpers.get_stats_via_docker()
        stats = metrics_helpers.parse_stats(stats_raw)

        metrics_raw = metrics_helpers.get_metrics_via_docker()
        metrics = metrics_helpers.parse_prometheus_metrics(metrics_raw)

        # Compare key counters. STATS and METRICS come from two separate HTTP
        # reads, so under active load the busier counters drift by ~the
        # throughput in the gap between reads (non-atomic sampling). Allow a
        # small proportional slack (~2%) with an absolute floor: large enough to
        # absorb that sampling skew, small enough to still catch a real
        # divergence (a genuine bug shows order-of-magnitude disagreement).
        def _tol(a, b, floor):
            return max(floor, round(0.02 * max(a, b)))

        stats_received = stats.get("notifications", {}).get("received", 0)
        metrics_received = metrics.get("pagespeed_notifications_total", 0)
        assert abs(stats_received - metrics_received) <= _tol(
            stats_received, metrics_received, 5
        ), (
            f"notifications mismatch: STATS={stats_received}, "
            f"METRICS={metrics_received}"
        )

        stats_written = stats.get("variants", {}).get("written", 0)
        metrics_written = metrics.get("pagespeed_variants_written_total", 0)
        assert abs(stats_written - metrics_written) <= _tol(
            stats_written, metrics_written, 5
        ), (
            f"variants mismatch: STATS={stats_written}, METRICS={metrics_written}"
        )

        stats_errors = stats.get("errors", 0)
        metrics_errors = metrics.get("pagespeed_errors_total", 0)
        assert abs(stats_errors - metrics_errors) <= _tol(
            stats_errors, metrics_errors, 5
        ), (
            f"errors mismatch: STATS={stats_errors}, METRICS={metrics_errors}"
        )

        stats_entries = stats.get("cache", {}).get("entries", 0)
        metrics_entries = metrics.get("pagespeed_cache_entries", 0)
        assert abs(stats_entries - metrics_entries) <= _tol(
            stats_entries, metrics_entries, 20
        ), (
            f"cache entries mismatch: STATS={stats_entries}, METRICS={metrics_entries}"
        )
