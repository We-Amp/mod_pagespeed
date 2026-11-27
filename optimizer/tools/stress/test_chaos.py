# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Chaos tests: kill/restart, corruption, and socket loss.

Tests system resilience when components fail, restart, or encounter
corrupted state. Verifies graceful degradation and recovery.
"""

import time

import metrics_helpers
import requests
from conftest import compose_run, docker_exec


class TestWorkerRecovery:
    """Worker kill and restart tests."""

    def test_worker_kill9_recovery(self, stress_services):
        """SIGKILL worker, verify nginx still serves, restart worker."""
        base_url = stress_services.nginx_url

        # Ensure nginx is working first
        r = requests.get(base_url + "/images/img-100k.jpg?kill9", timeout=10)
        assert r.status_code == 200

        # Kill the worker
        compose_run("kill", "-s", "KILL", "worker")
        time.sleep(1)

        # Nginx should still serve (via origin proxy, as MISS)
        r = requests.get(base_url + "/images/img-100k.jpg?kill9_after", timeout=10)
        assert r.status_code < 500, f"Nginx returned {r.status_code} after worker kill"

        # Restart worker and poll until healthy
        compose_run("start", "worker")
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker()
                if "OK" in str(health):
                    break
            except Exception:
                pass
            time.sleep(1)

        # Worker should recover and process new requests
        r = requests.get(base_url + "/images/img-100k.jpg?kill9_recovery", timeout=10)
        assert r.status_code == 200

        health = metrics_helpers.get_health_via_docker()
        assert "OK" in health, f"Worker unhealthy after recovery: {health}"

    def test_worker_restart_preserves_cache(self, stress_services, warm_cache):
        """Warm 5 URLs, restart worker, verify cache survives."""
        base_url = stress_services.nginx_url
        paths = warm_cache

        # Verify at least some are HITs
        initial_hits = 0
        for path in paths:
            try:
                r = requests.get(base_url + path, timeout=5)
                if r.headers.get("X-PageSpeed") == "HIT":
                    initial_hits += 1
            except Exception:
                pass

        # Restart worker (graceful) and poll until healthy
        compose_run("restart", "worker")
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker()
                if "OK" in str(health):
                    break
            except Exception:
                pass
            time.sleep(1)

        # Re-check: cache file should persist across worker restart
        post_hits = 0
        for path in paths:
            try:
                r = requests.get(base_url + path, timeout=5)
                if r.headers.get("X-PageSpeed") == "HIT":
                    post_hits += 1
            except Exception:
                pass

        # Cache file is shared via volume, so HITs should persist
        assert post_hits >= initial_hits - 1, (
            f"Cache lost: had {initial_hits} HITs before, {post_hits} after restart"
        )


class TestCacheCorruption:
    """Cache file corruption recovery tests."""

    def test_missing_cache_file(self, stress_services):
        """Delete cache.vol, restart both, verify recovery."""
        base_url = stress_services.nginx_url

        # Delete cache file
        docker_exec("worker", "rm -f /shared/cache.vol")

        # Restart both services and poll until healthy
        compose_run("restart", "worker")
        compose_run("restart", "nginx")
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker()
                if "OK" in str(health):
                    break
            except Exception:
                pass
            time.sleep(1)

        # Should be able to serve new requests
        r = requests.get(base_url + "/images/img-100k.jpg?missing_cache", timeout=10)
        assert r.status_code < 500, f"Failed after cache deletion: {r.status_code}"

    def test_truncated_cache_file(self, stress_services):
        """Truncate cache.vol to 0 bytes, restart, verify recovery."""
        base_url = stress_services.nginx_url

        # Truncate cache
        docker_exec("worker", ["sh", "-c", "> /shared/cache.vol"])

        # Restart both and poll until healthy
        compose_run("restart", "worker")
        compose_run("restart", "nginx")
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker()
                if "OK" in str(health):
                    break
            except Exception:
                pass
            time.sleep(1)

        # System should either re-initialize or fail gracefully
        try:
            r = requests.get(
                base_url + "/images/img-100k.jpg?truncated_cache", timeout=10
            )
            # Either works (re-initialized) or returns a proxy response
            assert r.status_code < 500, f"5xx after truncated cache: {r.status_code}"
        except requests.ConnectionError:
            # Nginx might need more time to recover
            time.sleep(5)
            r = requests.get(
                base_url + "/images/img-100k.jpg?truncated_cache2", timeout=10
            )
            assert r.status_code < 500


class TestSocketRecovery:
    """Unix socket failure tests."""

    def test_worker_socket_missing(self, stress_services):
        """Remove worker socket, make requests -- nginx should proxy to origin."""
        base_url = stress_services.nginx_url

        # Remove the worker socket
        docker_exec("worker", "rm -f /shared/pagespeed.sock")
        time.sleep(1)

        # Nginx should still serve via origin proxy
        r = requests.get(base_url + "/images/img-100k.jpg?no_socket", timeout=10)
        assert r.status_code < 500, (
            f"Nginx returned {r.status_code} without worker socket"
        )

        # Restart worker to restore socket, poll until healthy
        compose_run("restart", "worker")
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                health = metrics_helpers.get_health_via_docker()
                if "OK" in str(health):
                    break
            except Exception:
                pass
            time.sleep(1)

        # Verify recovery
        r = requests.get(base_url + "/images/img-100k.jpg?socket_restored", timeout=10)
        assert r.status_code == 200


class TestNginxResilience:
    """Nginx resilience tests."""

    def test_nginx_reload_under_load(self, stress_services):
        """nginx -s reload during sustained load."""
        base_url = stress_services.nginx_url
        import threading
        from concurrent.futures import ThreadPoolExecutor, as_completed

        errors_before = 0
        errors_during = 0
        errors_after = 0
        total = 0
        stop = threading.Event()

        def send_requests(phase_name):
            nonlocal total
            count = 0
            errs = 0
            while not stop.is_set() and count < 100:
                try:
                    r = requests.get(
                        base_url + f"/images/img-100k.jpg?reload={count}",
                        timeout=5,
                    )
                    if r.status_code >= 500:
                        errs += 1
                except Exception:
                    errs += 1
                count += 1
                total += 1
                time.sleep(0.01)
            return errs

        # Phase 1: Baseline load
        with ThreadPoolExecutor(max_workers=10) as pool:
            futures = [pool.submit(send_requests, "before") for _ in range(5)]
            time.sleep(1)

            # Phase 2: Trigger reload
            docker_exec("nginx", "nginx -s reload")

            # Phase 3: Continue during reload
            time.sleep(2)
            stop.set()

            for f in as_completed(futures):
                errors_during += f.result()

        time.sleep(2)

        # After reload, normal operation should resume
        for i in range(10):
            try:
                r = requests.get(
                    base_url + f"/images/img-100k.jpg?post_reload={i}", timeout=5
                )
                if r.status_code >= 500:
                    errors_after += 1
            except Exception:
                errors_after += 1

        # Error rate during reload should be low (< 2% with the fix)
        assert errors_during < total * 0.02, (
            f"Too many errors during reload: {errors_during}/{total} "
            f"({errors_during / max(total, 1) * 100:.1f}%)"
        )
        assert errors_after == 0, f"{errors_after} errors after reload completed"

    def test_nginx_reload_preserves_cache_hits(self, stress_services, warm_cache):
        """Warm cache, reload, verify HITs still work immediately after reload.

        Regression test for bug #8: inherited cache handles should allow
        the new worker process to serve HITs without a cold-start gap.
        """
        base_url = stress_services.nginx_url
        paths = warm_cache

        # Verify we have some HITs before reload
        hits_before = 0
        for path in paths:
            try:
                r = requests.get(base_url + path, timeout=5)
                if r.headers.get("X-PageSpeed") == "HIT":
                    hits_before += 1
            except Exception:
                pass

        # Trigger reload
        docker_exec("nginx", "nginx -s reload")
        time.sleep(1)

        # Check HITs immediately after reload
        hits_after = 0
        for path in paths:
            try:
                r = requests.get(base_url + path, timeout=5)
                if r.headers.get("X-PageSpeed") == "HIT":
                    hits_after += 1
            except Exception:
                pass

        # With the fix, inherited cache handles mean no cold-start gap
        assert hits_after >= hits_before - 1, (
            f"Cache HITs dropped after reload: {hits_before} -> {hits_after}"
        )
