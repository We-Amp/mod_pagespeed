# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""HTTP throughput and latency stress tests.

Tests nginx + cache serving performance under various load patterns,
including ramp-up, steady state, cold/warm cache, and mixed workloads.
"""

import random
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import requests

# Test image/content paths available in the stress testdata
IMAGE_PATHS = [
    "/images/img-100k.jpg",
    "/images/img-500k.jpg",
    "/images/img-1m.jpg",
]

MIXED_PATHS = [
    ("/css/style-1k.css", None),
    ("/js/app-1k.js", None),
    ("/html/minimal.html", None),
    ("/images/img-100k.jpg", None),
    ("/images/img-100k.jpg", "image/webp,*/*"),
    ("/images/img-100k.jpg", "image/avif,*/*"),
    ("/images/img-500k.jpg", None),
    ("/images/img-500k.jpg", "image/webp,*/*"),
]


def make_request(base_url, path, accept_header=None, timeout=10):
    """Send a single HTTP request and return (latency_ms, status, error)."""
    url = base_url.rstrip("/") + path
    headers = {}
    if accept_header:
        headers["Accept"] = accept_header
    start = time.monotonic()
    try:
        r = requests.get(url, headers=headers, timeout=timeout)
        latency_ms = (time.monotonic() - start) * 1000.0
        return latency_ms, r.status_code, r.headers.get("X-PageSpeed", ""), None
    except requests.RequestException as e:
        latency_ms = (time.monotonic() - start) * 1000.0
        return latency_ms, 0, "", str(e)


def percentile(sorted_data, p):
    """Return the p-th percentile from sorted data."""
    if not sorted_data:
        return 0.0
    k = (len(sorted_data) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1
    if c >= len(sorted_data):
        return sorted_data[f]
    return sorted_data[f] + (k - f) * (sorted_data[c] - sorted_data[f])


def run_load(base_url, paths, rps, duration_s, timeout=10):
    """Run sustained load at target RPS for the given duration.

    Returns (latencies_ms, statuses, errors, pagespeed_headers).
    """
    total = rps * duration_s
    interval = 1.0 / rps if rps > 0 else 0.0
    workers = min(rps * 3, 500)

    latencies = []
    statuses = []
    errors = []
    ps_headers = []

    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(workers, 10)) as pool:
        futures = []
        for i in range(total):
            target_time = start + i * interval
            now = time.monotonic()
            sleep_for = target_time - now
            if sleep_for > 0:
                time.sleep(sleep_for)

            if isinstance(paths[0], tuple):
                path, accept = paths[i % len(paths)]
            else:
                path = paths[i % len(paths)]
                accept = None

            futures.append(pool.submit(make_request, base_url, path, accept, timeout))

            # Drain periodically
            if len(futures) >= 500:
                done = [f for f in futures if f.done()]
                for f in done:
                    lat, st, ps, err = f.result()
                    latencies.append(lat)
                    statuses.append(st)
                    ps_headers.append(ps)
                    if err:
                        errors.append(err)
                futures = [f for f in futures if not f.done()]

        # Drain remaining
        for f in as_completed(futures):
            lat, st, ps, err = f.result()
            latencies.append(lat)
            statuses.append(st)
            ps_headers.append(ps)
            if err:
                errors.append(err)

    return latencies, statuses, errors, ps_headers


def run_burst(base_url, paths, count, timeout=10):
    """Send count requests concurrently."""
    latencies = []
    statuses = []
    errors = []
    ps_headers = []

    with ThreadPoolExecutor(max_workers=min(count, 200)) as pool:
        futures = []
        for i in range(count):
            if isinstance(paths[0], tuple):
                path, accept = paths[i % len(paths)]
            else:
                path = paths[i % len(paths)]
                accept = None
            futures.append(pool.submit(make_request, base_url, path, accept, timeout))
        for f in as_completed(futures):
            lat, st, ps, err = f.result()
            latencies.append(lat)
            statuses.append(st)
            ps_headers.append(ps)
            if err:
                errors.append(err)

    return latencies, statuses, errors, ps_headers


class TestHttpLoad:
    """HTTP load and latency tests."""

    def test_throughput_ramp(self, stress_services):
        """Ramp from 10 to 200 req/s, identify ceiling."""
        base_url = stress_services.nginx_url
        paths = [p for p, _ in MIXED_PATHS]
        ceiling_rps = 0

        for rps in range(10, 210, 20):
            lats, statuses, errs, _ = run_load(base_url, paths, rps, 3)
            if not lats:
                continue
            lats.sort()
            p99 = percentile(lats, 99)
            error_rate = len(errs) / len(lats) if lats else 1.0

            if rps <= 50:
                # Low load should be clean
                assert error_rate < 0.05, f"Error rate {error_rate:.1%} at {rps} rps"
            if p99 > 2000 or error_rate > 0.10:
                ceiling_rps = rps - 20
                break
            ceiling_rps = rps

        assert ceiling_rps >= 10, "Could not sustain even 10 req/s"

    def test_throughput_steady_state(self, stress_services, warm_cache):
        """Run at moderate throughput for 30s."""
        base_url = stress_services.nginx_url
        paths = [p for p, _ in MIXED_PATHS]

        lats, statuses, errs, _ = run_load(base_url, paths, 50, 30)
        assert lats, "No responses received"
        lats.sort()

        error_rate = len(errs) / len(lats)
        p99 = percentile(lats, 99)
        five_xx = sum(1 for s in statuses if 500 <= s < 600)

        assert error_rate < 0.01, f"Error rate {error_rate:.1%} exceeds 1%"
        assert p99 < 3000, f"p99 latency {p99:.0f}ms exceeds 3000ms"
        assert five_xx == 0, f"{five_xx} 5xx responses"

    def test_cold_cache_latency(self, stress_services):
        """100 concurrent requests to unique URLs, empty cache."""
        base_url = stress_services.nginx_url
        # Use unique paths that won't be cached
        paths = [f"/images/img-100k.jpg?cold={i}" for i in range(100)]

        lats, statuses, errs, ps_headers = run_burst(base_url, paths, 100)
        assert lats, "No responses received"
        lats.sort()

        p99 = percentile(lats, 99)
        five_xx = sum(1 for s in statuses if 500 <= s < 600)
        miss_count = sum(1 for h in ps_headers if h == "MISS")

        assert p99 < 5000, f"p99 latency {p99:.0f}ms exceeds 5000ms"
        assert five_xx == 0, f"{five_xx} 5xx responses"
        assert miss_count == 100, f"Expected 100 MISSes, got {miss_count}"

    def test_warm_cache_latency(self, stress_services, warm_cache):
        """Requests to pre-cached URLs should be fast."""
        base_url = stress_services.nginx_url
        paths = warm_cache

        # Ensure cache is warm by polling with retry on connection errors
        client = requests.Session()
        for path in paths:
            for _ in range(10):
                try:
                    r = client.get(base_url + path, timeout=5)
                    if r.headers.get("X-PageSpeed") == "HIT":
                        break
                except requests.RequestException:
                    pass
                time.sleep(0.5)

        lats, statuses, errs, ps_headers = run_load(base_url, paths, 50, 10)
        assert lats, "No responses received"
        lats.sort()

        p50 = percentile(lats, 50)
        p95 = percentile(lats, 95)
        p99 = percentile(lats, 99)
        hit_rate = sum(1 for h in ps_headers if h == "HIT") / len(ps_headers)

        assert p50 < 50, f"p50 latency {p50:.0f}ms exceeds 50ms"
        assert p95 < 200, f"p95 latency {p95:.0f}ms exceeds 200ms"
        assert p99 < 500, f"p99 latency {p99:.0f}ms exceeds 500ms"
        # Some may still be MISS if worker hasn't processed yet
        assert hit_rate > 0.5, f"Hit rate {hit_rate:.1%} below 50%"

    def test_mixed_hit_miss(self, stress_services, warm_cache):
        """70% cached / 30% uncached, moderate throughput for 15s."""
        base_url = stress_services.nginx_url
        cached = warm_cache
        uncached = [f"/images/img-100k.jpg?mix={i}" for i in range(50)]

        # Build 70/30 mix
        paths = []
        for i in range(100):
            if i % 10 < 7:
                paths.append(cached[i % len(cached)])
            else:
                paths.append(uncached[i % len(uncached)])

        lats, statuses, errs, _ = run_load(base_url, paths, 50, 15)
        assert lats, "No responses received"
        lats.sort()

        p95 = percentile(lats, 95)
        error_rate = len(errs) / len(lats)

        assert p95 < 2000, f"p95 latency {p95:.0f}ms exceeds 2000ms"
        assert error_rate < 0.01, f"Error rate {error_rate:.1%} exceeds 1%"

    def test_image_heavy_load(self, stress_services):
        """80% images at various sizes, moderate throughput."""
        base_url = stress_services.nginx_url
        image_paths = [
            ("/images/img-100k.jpg", None),
            ("/images/img-100k.jpg", "image/webp,*/*"),
            ("/images/img-500k.jpg", None),
            ("/images/img-500k.jpg", "image/avif,*/*"),
            ("/images/img-1m.jpg", None),
            ("/images/img-1m.jpg", "image/webp,*/*"),
            ("/images/test.png", None),
            ("/images/static.gif", None),
        ]
        text_paths = [
            ("/css/style-1k.css", None),
            ("/html/minimal.html", None),
        ]
        # 80% images, 20% text
        paths = image_paths * 4 + text_paths

        lats, statuses, errs, _ = run_load(base_url, paths, 30, 15)
        assert lats, "No responses received"

        five_xx = sum(1 for s in statuses if 500 <= s < 600)
        assert five_xx == 0, f"{five_xx} 5xx responses"
        assert len(errs) < len(lats) * 0.05, f"Too many errors: {len(errs)}/{len(lats)}"

    def test_capability_diversity(self, stress_services, stats_client):
        """Random Accept/Save-Data/UA combos, verify variant generation."""
        base_url = stress_services.nginx_url
        accept_options = [
            None,
            "image/webp,*/*",
            "image/avif,*/*",
        ]
        save_data_options = [None, "on"]

        paths_with_headers = []
        for i in range(200):
            accept = random.choice(accept_options)
            save_data = random.choice(save_data_options)
            headers = {}
            if accept:
                headers["Accept"] = accept
            if save_data:
                headers["Save-Data"] = save_data
            paths_with_headers.append(("/images/img-100k.jpg", headers))

        with ThreadPoolExecutor(max_workers=50) as pool:
            futures = []
            for path, headers in paths_with_headers:
                futures.append(
                    pool.submit(
                        lambda p, h: requests.get(base_url + p, headers=h, timeout=10),
                        path,
                        headers,
                    )
                )
            for f in as_completed(futures):
                try:
                    f.result()
                except Exception:
                    pass

        # Wait for worker to process by polling for new variants
        raw_before = stats_client.get_stats_via_docker()
        stats_before = stats_client.parse_stats(raw_before)
        variants_before = stats_before.get("variants", {}).get("written", 0)

        try:
            stats_client.wait_for_stats_condition(
                lambda s: s.get("variants", {}).get("written", 0) > variants_before,
                timeout=15,
                interval=1.0,
            )
        except TimeoutError:
            pass

        raw = stats_client.get_stats_via_docker()
        stats = stats_client.parse_stats(raw)
        by_format = stats.get("by_format", {})
        formats_with_output = sum(1 for v in by_format.values() if v > 0)
        assert formats_with_output >= 2, (
            f"Expected >= 2 formats generated, got {formats_with_output}: {by_format}"
        )

    def test_connection_flood(self, stress_services):
        """1000 concurrent HTTP connections to nginx."""
        base_url = stress_services.nginx_url
        paths = ["/html/minimal.html"] * 1000

        lats, statuses, errs, _ = run_burst(base_url, paths, 1000, timeout=15)

        # Count successes (2xx/3xx)
        successes = sum(1 for s in statuses if 200 <= s < 400)
        five_xx = sum(1 for s in statuses if 500 <= s < 600)

        assert successes >= 200, f"Only {successes} succeeded out of 1000 connections"
        # Nginx should not crash -- verify it's still responsive
        r = requests.get(base_url + "/index.html", timeout=5)
        assert r.status_code < 500, "Nginx unresponsive after connection flood"
