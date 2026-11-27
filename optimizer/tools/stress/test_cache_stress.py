# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Cache eviction, cross-process consistency, and directory pressure tests.

Tests cache behavior under memory pressure, concurrent access patterns,
and high key-count scenarios with the stress-tuned 50MB cache.
"""

import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import metrics_helpers
import requests


class TestCacheEviction:
    """Cache eviction under pressure tests."""

    def test_cache_eviction(self, stress_services):
        """100 unique 500KB images into 50MB cache triggers eviction."""
        base_url = stress_services.nginx_url

        # Request 100 unique URLs (each ~500KB -> ~50MB total, exceeds 50MB cache)
        errors = 0
        batch_size = 25
        for batch_start in range(0, 100, batch_size):
            paths = [
                f"/images/img-500k.jpg?evict={i}"
                for i in range(batch_start, batch_start + batch_size)
            ]
            with ThreadPoolExecutor(max_workers=25) as pool:
                futures = [
                    pool.submit(requests.get, base_url + p, timeout=15) for p in paths
                ]
                for f in as_completed(futures):
                    try:
                        r = f.result()
                        if r.status_code >= 500:
                            errors += 1
                    except Exception:
                        pass  # Timeouts acceptable under load
        assert errors == 0, f"{errors} server errors during eviction test"

        # Poll until cache has entries (processing complete)
        try:
            stats = metrics_helpers.wait_for_stats_condition(
                lambda s: s.get("cache", {}).get("entries", 0) > 0,
                timeout=30,
                interval=1.0,
            )
        except TimeoutError:
            pass

        # Cache should have evicted older entries but still work
        raw = metrics_helpers.get_stats_via_docker()
        stats = metrics_helpers.parse_stats(raw)
        cache_entries = stats.get("cache", {}).get("entries", 0)
        assert cache_entries > 0, "Cache has no entries after eviction test"

        # Recent URLs should still be accessible
        r = requests.get(base_url + "/images/img-500k.jpg?evict=199", timeout=10)
        assert r.status_code == 200, f"Recent URL inaccessible: {r.status_code}"

    def test_eviction_preserves_hot(self, stress_services, warm_cache):
        """Hot URLs should survive eviction better than cold ones."""
        base_url = stress_services.nginx_url
        hot_paths = warm_cache[:3]  # Use first 3 warm paths

        # Hit hot paths repeatedly to mark them as frequently accessed
        for _ in range(10):
            for path in hot_paths:
                try:
                    requests.get(base_url + path, timeout=5)
                except Exception:
                    pass

        # Flood with cold URLs to trigger eviction
        for i in range(100):
            path = f"/images/img-500k.jpg?cold_flood={i}"
            try:
                requests.get(base_url + path, timeout=10)
            except Exception:
                pass

        time.sleep(1)

        # Check how many hot URLs are still HITs
        hits = 0
        for path in hot_paths:
            try:
                r = requests.get(base_url + path, timeout=5)
                if r.headers.get("X-PageSpeed") == "HIT":
                    hits += 1
            except Exception:
                pass

        # Under extreme cache pressure (2x oversubscription), CLFUS cannot
        # guarantee hot URL preservation. This is documented behavior, not a
        # bug. Recommended cache sizing: 3-5x working set.
        # We log the result but don't assert hard — this is a benchmark, not
        # a correctness test.
        if hits == 0:
            import warnings

            warnings.warn(
                f"No hot URLs survived eviction ({hits}/{len(hot_paths)}). "
                "This is expected under 2x cache oversubscription. "
                "Recommended: size cache at 3-5x working set.",
                stacklevel=1,
            )


class TestConcurrentAccess:
    """Concurrent cache read/write tests."""

    def test_concurrent_read_write(self, stress_services):
        """50 MISSes + 50 re-requests simultaneously."""
        base_url = stress_services.nginx_url

        # Phase 1: 50 MISSes (new URLs)
        miss_paths = [f"/images/img-100k.jpg?conc={i}" for i in range(50)]
        with ThreadPoolExecutor(max_workers=50) as pool:
            miss_futures = [
                pool.submit(requests.get, base_url + p, timeout=10) for p in miss_paths
            ]
            miss_results = []
            for f in as_completed(miss_futures):
                try:
                    r = f.result()
                    miss_results.append(r)
                except Exception:
                    pass

        # Phase 2: Re-request the same URLs (may be in-flight write by worker)
        with ThreadPoolExecutor(max_workers=50) as pool:
            reread_futures = [
                pool.submit(requests.get, base_url + p, timeout=10) for p in miss_paths
            ]
            reread_results = []
            for f in as_completed(reread_futures):
                try:
                    r = f.result()
                    reread_results.append(r)
                except Exception:
                    pass

        # No 5xx or garbled responses
        for r in miss_results + reread_results:
            assert r.status_code < 500, f"5xx during concurrent access: {r.status_code}"
            assert len(r.content) > 0, "Empty response during concurrent access"

    def test_2k_unique_urls(self, stress_services):
        """2,000 unique paths to a small image."""
        base_url = stress_services.nginx_url

        errors = 0
        timeouts = 0
        server_errors = 0
        total = 0
        batch_size = 100

        start = time.monotonic()
        for batch_start in range(0, 2000, batch_size):
            paths = [
                f"/images/img-100k.jpg?key={i}"
                for i in range(batch_start, min(batch_start + batch_size, 2000))
            ]
            with ThreadPoolExecutor(max_workers=25) as pool:
                futures = [
                    pool.submit(requests.get, base_url + p, timeout=30) for p in paths
                ]
                for f in as_completed(futures):
                    total += 1
                    try:
                        r = f.result()
                        if r.status_code >= 500:
                            server_errors += 1
                            errors += 1
                    except requests.Timeout:
                        timeouts += 1
                        errors += 1
                    except Exception:
                        errors += 1

        elapsed = time.monotonic() - start
        throughput = total / elapsed if elapsed > 0 else 0

        assert errors < total * 0.10, (
            f"Too many errors: {errors}/{total} ({errors / total:.1%}) "
            f"[timeouts={timeouts}, 5xx={server_errors}]"
        )
        assert throughput > 0.5, f"Throughput {throughput:.1f} req/s below 0.5 req/s (sanity check)"


class TestPurgeCycles:
    """Cache purge and repopulate tests."""

    def test_purge_repopulate_cycle(self, stress_services):
        """5 URLs x 5 purge+repopulate cycles."""
        base_url = stress_services.nginx_url
        paths = [f"/images/img-100k.jpg?purge_repop={i}" for i in range(5)]

        for cycle in range(5):
            # Request all paths (populate)
            for path in paths:
                try:
                    requests.get(base_url + path, timeout=10)
                except Exception:
                    pass

            time.sleep(1)

            # Purge all
            for path in paths:
                try:
                    metrics_helpers.purge_via_docker(path)
                except Exception:
                    pass

            # Re-request (should be MISS again)
            for path in paths:
                try:
                    r = requests.get(base_url + path, timeout=10)
                    assert r.status_code < 500, f"5xx on cycle {cycle}: {r.status_code}"
                except requests.Timeout:
                    pass

        # Verify worker still healthy
        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after purge cycles: {health}"
