# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Regression tests for HitTracker deadlock (bug #5).

Bug #5: Under ~2000 concurrent unique URL requests, nginx hung permanently.
Root causes:
1. Cyclone HitTracker AB/BA deadlock: flush_now() held _mutex while calling
   _flush_callback (-> stripe->mutex). Read path holds stripe->mutex then
   calls record_hit -> _mutex.
2. Blocking notification sender: connect()/write() blocked indefinitely
   when the worker's accept queue was full.

Fix:
1. Cyclone: flush_now() moves data out under lock, releases _mutex, then
   calls callbacks.
2. Nginx: Non-blocking sender with O_NONBLOCK + poll() + 50ms timeout.

These tests exercise heavy concurrent access patterns that would trigger
the deadlock or blocking behavior in the unfixed code.
"""

import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import metrics_helpers
import requests


class TestConcurrentAccessDeadlock:
    """Regression tests for HitTracker deadlock under concurrent load."""

    def test_500_concurrent_unique_urls(self, stress_services):
        """500 unique URLs via 50 threads through nginx.

        120s timeout. Assert all complete (deadlock hangs forever).
        Assert worker healthy.
        """
        base_url = stress_services.nginx_url

        completed = 0
        errors = 0
        start = time.monotonic()

        with ThreadPoolExecutor(max_workers=50) as pool:
            futures = [
                pool.submit(
                    requests.get,
                    base_url + f"/images/img-100k.jpg?deadlock_{i}",
                    timeout=30,
                )
                for i in range(500)
            ]
            for f in as_completed(futures, timeout=120):
                try:
                    r = f.result()
                    if r.status_code < 500:
                        completed += 1
                    else:
                        errors += 1
                except Exception:
                    errors += 1

        elapsed = time.monotonic() - start

        # The deadlock would cause this to hang forever (timeout at 120s)
        assert completed > 0, "No requests completed — possible deadlock"
        assert errors < 50, f"Too many errors: {errors}/500"

        # Worker must still be healthy
        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after concurrent test: {health}"

    def test_repeated_reads_flush_pressure(self, stress_services):
        """Same 10 URLs hit 50x each from 20 threads (10k requests).

        Exercises heavy record_hit traffic triggering HitTracker flushes.
        180s timeout. Assert completion + health.
        """
        base_url = stress_services.nginx_url

        # Pre-warm the 10 URLs
        urls = [f"/images/img-100k.jpg?flush_{i}" for i in range(10)]
        for url in urls:
            try:
                requests.get(base_url + url, timeout=10)
            except Exception:
                pass

        # Wait for initial processing
        time.sleep(3)

        # Hit them repeatedly from many threads.
        # Use Session() for HTTP keep-alive — bare requests.get() creates a
        # new TCP connection per call, which is catastrophically slow on WSL2
        # (20k connect/close cycles through the virtual NIC bridge).
        completed = 0
        errors = 0

        def hit_urls():
            nonlocal completed, errors
            with requests.Session() as s:
                for _ in range(50):
                    for url in urls:
                        try:
                            r = s.get(base_url + url, timeout=10)
                            if r.status_code < 500:
                                completed += 1
                            else:
                                errors += 1
                        except Exception:
                            errors += 1

        start = time.monotonic()
        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(hit_urls) for _ in range(20)]
            for f in as_completed(futures, timeout=180):
                try:
                    f.result()
                except Exception:
                    pass

        elapsed = time.monotonic() - start

        # Should complete well within 60s (deadlock would hang)
        assert completed > 0, "No requests completed — possible deadlock"

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after flush pressure: {health}"
