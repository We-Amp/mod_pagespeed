#!/usr/bin/env python3
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Stress and concurrency tests for IIS PageSpeed module.

These tests verify module stability under concurrent load and rapid
sequential requests. They help identify race conditions, resource
exhaustion, and memory leak issues.
"""

import concurrent.futures
import os
import statistics
import subprocess
import threading
import time
import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
)


@pytest.mark.slow
@pytest.mark.iis_only
class TestConcurrentRequests:
    """Test module stability under concurrent load."""

    def test_concurrent_html_rewriting(
        self, client: PageSpeedClient, example_root: str
    ):
        """10 concurrent HTML rewrite requests should complete without error."""
        # Use unique query params to avoid caching
        urls = [f"{example_root}/combine_css.html?t={i}" for i in range(10)]

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(client.get, url) for url in urls]
            responses = [f.result() for f in concurrent.futures.as_completed(futures)]

        # All requests should complete successfully
        for i, response in enumerate(responses):
            assert_http_status(response, 200)
            # Verify we got HTML content back
            assert "</html>" in response.text.lower() or "</body>" in response.text.lower(), \
                f"Request {i} returned invalid HTML content"

    def test_concurrent_ipro_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """10 concurrent IPRO requests should complete without error."""
        # Request CSS and JS files that trigger IPRO
        css_urls = [f"{example_root}/styles/yellow.css?t={i}" for i in range(5)]
        js_urls = [f"{example_root}/scripts/example.js?t={i}" for i in range(5)]
        urls = css_urls + js_urls

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(client.get, url) for url in urls]
            responses = [f.result() for f in concurrent.futures.as_completed(futures)]

        # All requests should complete successfully
        for response in responses:
            assert_http_status(response, 200)
            # Verify we got content back
            assert len(response.body) > 0, "Response body should not be empty"

    def test_concurrent_mixed_workload(
        self, client: PageSpeedClient, example_root: str
    ):
        """Mixed HTML/CSS/JS/image requests in parallel should all complete."""
        # Create a mixed workload of different resource types
        html_urls = [f"{example_root}/index.html?t={i}" for i in range(3)]
        css_urls = [f"{example_root}/styles/yellow.css?t={i}" for i in range(3)]
        js_urls = [f"{example_root}/scripts/example.js?t={i}" for i in range(2)]
        image_urls = [f"{example_root}/images/sample.png?t={i}" for i in range(2)]

        urls = html_urls + css_urls + js_urls + image_urls

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(client.get, url) for url in urls]
            responses = [f.result() for f in concurrent.futures.as_completed(futures)]

        # All requests should complete successfully
        success_count = sum(1 for r in responses if r.status == 200)
        assert success_count == len(urls), \
            f"Expected all {len(urls)} requests to succeed, got {success_count}"

    def test_concurrent_filter_combinations(
        self, client: PageSpeedClient, example_root: str
    ):
        """Concurrent requests with different filter combinations."""
        # Different filter combinations
        filter_sets = [
            "combine_css",
            "combine_javascript",
            "rewrite_css,rewrite_javascript",
            "collapse_whitespace",
            "remove_comments",
            "extend_cache",
            "+core",
            "+optimize_for_bandwidth",
        ]

        urls = [
            f"{example_root}/combine_css.html?PageSpeedFilters={filters}&t={i}"
            for i, filters in enumerate(filter_sets)
        ]

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            futures = [executor.submit(client.get, url) for url in urls]
            responses = [f.result() for f in concurrent.futures.as_completed(futures)]

        # All requests should complete successfully
        for response in responses:
            assert_http_status(response, 200)


@pytest.mark.slow
@pytest.mark.iis_only
class TestRapidSequentialRequests:
    """Test rapid sequential requests don't cause issues."""

    def test_rapid_html_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """50 rapid sequential HTML requests should all succeed."""
        url = f"{example_root}/index.html"
        success_count = 0
        error_messages = []

        for i in range(50):
            try:
                response = client.get(f"{url}?seq={i}")
                if response.status == 200:
                    success_count += 1
                else:
                    error_messages.append(f"Request {i}: status {response.status}")
            except Exception as e:
                error_messages.append(f"Request {i}: {e}")

        # All requests should succeed
        assert success_count == 50, \
            f"Expected 50 successful requests, got {success_count}. Errors: {error_messages[:5]}"

    def test_rapid_css_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """50 rapid sequential CSS requests should all succeed."""
        url = f"{example_root}/styles/yellow.css"
        success_count = 0
        error_messages = []

        for i in range(50):
            try:
                response = client.get(f"{url}?seq={i}")
                if response.status == 200:
                    success_count += 1
                else:
                    error_messages.append(f"Request {i}: status {response.status}")
            except Exception as e:
                error_messages.append(f"Request {i}: {e}")

        # All requests should succeed
        assert success_count == 50, \
            f"Expected 50 successful requests, got {success_count}. Errors: {error_messages[:5]}"

    def test_rapid_js_requests(
        self, client: PageSpeedClient, example_root: str
    ):
        """50 rapid sequential JavaScript requests should all succeed."""
        url = f"{example_root}/scripts/example.js"
        success_count = 0
        error_messages = []

        for i in range(50):
            try:
                response = client.get(f"{url}?seq={i}")
                if response.status == 200:
                    success_count += 1
                else:
                    error_messages.append(f"Request {i}: status {response.status}")
            except Exception as e:
                error_messages.append(f"Request {i}: {e}")

        # All requests should succeed
        assert success_count == 50, \
            f"Expected 50 successful requests, got {success_count}. Errors: {error_messages[:5]}"

    def test_rapid_requests_with_html_rewrite(
        self, client: PageSpeedClient, example_root: str
    ):
        """50 rapid HTML requests with filters enabled should all succeed."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        success_count = 0

        for i in range(50):
            try:
                response = client.get(f"{url}&seq={i}")
                if response.status == 200:
                    success_count += 1
            except Exception:
                pass

        # Allow some failures under high load, but most should succeed
        assert success_count >= 45, \
            f"Expected at least 45/50 successful requests, got {success_count}"


@pytest.mark.slow
@pytest.mark.iis_only
class TestResourceExhaustion:
    """Test the module doesn't exhaust resources over time."""

    def test_many_requests_no_memory_leak(
        self, client: PageSpeedClient, example_root: str
    ):
        """Many requests shouldn't cause growing response times.

        This is a heuristic test - if there's a memory leak or resource
        exhaustion, response times will typically increase over time.
        """
        url = f"{example_root}/index.html"
        num_batches = 5
        requests_per_batch = 20
        batch_latencies = []

        for batch in range(num_batches):
            batch_times = []
            for i in range(requests_per_batch):
                start = time.time()
                response = client.get(f"{url}?batch={batch}&req={i}")
                elapsed = time.time() - start
                batch_times.append(elapsed)

                assert_http_status(response, 200)

            # Calculate average latency for this batch
            avg_latency = statistics.mean(batch_times)
            batch_latencies.append(avg_latency)

        # The last batch shouldn't be significantly slower than the first
        # Allow 3x degradation as threshold (generous for flaky tests)
        first_batch_avg = batch_latencies[0]
        last_batch_avg = batch_latencies[-1]

        assert last_batch_avg < first_batch_avg * 3, \
            f"Response time degraded significantly: first batch {first_batch_avg:.3f}s, " \
            f"last batch {last_batch_avg:.3f}s. Possible resource leak."

    def test_sustained_load_stability(
        self, client: PageSpeedClient, example_root: str
    ):
        """Server should remain stable under sustained moderate load."""
        url = f"{example_root}/combine_css.html?PageSpeedFilters=combine_css"
        duration_seconds = 10
        interval = 0.1  # 10 requests per second

        start_time = time.time()
        success_count = 0
        total_count = 0
        errors = []

        while time.time() - start_time < duration_seconds:
            total_count += 1
            try:
                response = client.get(f"{url}&t={total_count}")
                if response.status == 200:
                    success_count += 1
                else:
                    errors.append(f"Request {total_count}: status {response.status}")
            except Exception as e:
                errors.append(f"Request {total_count}: {e}")

            time.sleep(interval)

        success_rate = success_count / total_count if total_count > 0 else 0

        # Expect at least 90% success rate under sustained load
        assert success_rate >= 0.9, \
            f"Success rate {success_rate:.1%} below threshold. " \
            f"{success_count}/{total_count} succeeded. First errors: {errors[:3]}"

    def test_burst_load_recovery(
        self, client: PageSpeedClient, example_root: str
    ):
        """Server should recover after a burst of concurrent requests."""
        url = f"{example_root}/index.html"

        # Phase 1: Burst of concurrent requests
        burst_urls = [f"{url}?burst={i}" for i in range(20)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=20) as executor:
            futures = [executor.submit(client.get, u) for u in burst_urls]
            burst_responses = [f.result() for f in concurrent.futures.as_completed(futures)]

        burst_success = sum(1 for r in burst_responses if r.status == 200)

        # Brief pause for recovery
        time.sleep(1)

        # Phase 2: Single sequential requests should work normally
        recovery_success = 0
        for i in range(5):
            response = client.get(f"{url}?recovery={i}")
            if response.status == 200:
                recovery_success += 1

        # All recovery requests should succeed
        assert recovery_success == 5, \
            f"Expected all 5 recovery requests to succeed, got {recovery_success}. " \
            f"Burst success: {burst_success}/20"


@pytest.mark.slow
@pytest.mark.iis_only
class TestConcurrentFilterInteractions:
    """Test concurrent requests with different filters don't interfere."""

    def test_concurrent_pagespeed_on_off(
        self, client: PageSpeedClient, example_root: str
    ):
        """Concurrent requests with PageSpeed=on and PageSpeed=off."""
        on_urls = [f"{example_root}/index.html?t={i}" for i in range(5)]
        off_urls = [f"{example_root}/index.html?PageSpeed=off&t={i}" for i in range(5, 10)]

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            on_futures = [executor.submit(client.get, url) for url in on_urls]
            off_futures = [executor.submit(client.get, url) for url in off_urls]

            on_responses = [f.result() for f in concurrent.futures.as_completed(on_futures)]
            off_responses = [f.result() for f in concurrent.futures.as_completed(off_futures)]

        # All should succeed
        for response in on_responses + off_responses:
            assert_http_status(response, 200)

        # PageSpeed=off responses should not have .pagespeed. URLs
        for response in off_responses:
            assert ".pagespeed." not in response.text, \
                "PageSpeed=off response should not contain .pagespeed. URLs"

    def test_concurrent_different_content_types(
        self, client: PageSpeedClient, example_root: str
    ):
        """Concurrent requests for HTML, CSS, JS shouldn't interfere."""
        # URLs of different content types
        requests = [
            (f"{example_root}/index.html", "text/html"),
            (f"{example_root}/styles/yellow.css", "text/css"),
            (f"{example_root}/scripts/example.js", "javascript"),
            (f"{example_root}/images/sample.png", "image/"),
        ]

        # Make multiple concurrent requests of each type
        urls_and_types = []
        for url, expected_type in requests:
            for i in range(3):
                urls_and_types.append((f"{url}?c={i}", expected_type))

        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
            futures = {
                executor.submit(client.get, url): expected_type
                for url, expected_type in urls_and_types
            }
            results = []
            for future in concurrent.futures.as_completed(futures):
                expected_type = futures[future]
                response = future.result()
                results.append((response, expected_type))

        # Verify content types are correct
        for response, expected_type in results:
            assert_http_status(response, 200)
            content_type = response.header("Content-Type").lower()
            assert expected_type.lower() in content_type, \
                f"Expected content type containing '{expected_type}', got '{content_type}'"


@pytest.mark.slow
@pytest.mark.iis_only
class TestHeavyConcurrentLoad:
    """Test module stability under heavy concurrent load sustained over time."""

    def _make_request(self, client, url, results, index):
        """Make a single request and record the outcome."""
        try:
            response = client.get(url)
            results[index] = ("ok", response.status)
        except Exception as e:
            results[index] = ("error", str(e))

    def test_sustained_heavy_concurrent_load(
        self, client: PageSpeedClient, example_root: str, server_config
    ):
        """200 parallel workers hammering the server for 30 seconds.

        Each worker makes sequential requests as fast as possible for the
        full duration. This tests thread safety, connection handling, and
        resource management under heavy concurrent load.
        """
        num_workers = 200
        duration_seconds = 30

        # Mixed URLs to exercise different code paths
        url_templates = [
            f"{example_root}/index.html",
            f"{example_root}/combine_css.html",
            f"{example_root}/styles/yellow.css",
            f"{example_root}/scripts/example.js",
            f"{example_root}/images/sample.png",
        ]

        # Shared counters protected by lock
        lock = threading.Lock()
        counters = {
            "success": 0,
            "http_error": 0,
            "conn_error": 0,
            "total": 0,
        }
        # Track per-second throughput
        per_second = {}
        stop_event = threading.Event()

        def worker(worker_id):
            # Each worker creates its own client to avoid connection sharing
            w_client = PageSpeedClient(
                server_config.host, server_config.port, timeout=10.0
            )
            local_success = 0
            local_http_error = 0
            local_conn_error = 0
            local_total = 0

            while not stop_event.is_set():
                url_idx = local_total % len(url_templates)
                url = f"{url_templates[url_idx]}?w={worker_id}&r={local_total}"
                local_total += 1
                try:
                    response = w_client.get(url)
                    if response.status == 200:
                        local_success += 1
                    else:
                        local_http_error += 1
                except Exception:
                    local_conn_error += 1

                # Record per-second bucket
                sec = int(time.time() - start_time)
                with lock:
                    per_second[sec] = per_second.get(sec, 0) + 1

            with lock:
                counters["success"] += local_success
                counters["http_error"] += local_http_error
                counters["conn_error"] += local_conn_error
                counters["total"] += local_total

        start_time = time.time()
        threads = []
        for i in range(num_workers):
            t = threading.Thread(target=worker, args=(i,), daemon=True)
            threads.append(t)
            t.start()

        # Let the load run for the full duration
        time.sleep(duration_seconds)
        stop_event.set()

        # Wait for all workers to finish
        for t in threads:
            t.join(timeout=15.0)

        elapsed = time.time() - start_time
        total = counters["total"]
        success = counters["success"]
        http_error = counters["http_error"]
        conn_error = counters["conn_error"]
        success_rate = success / total if total > 0 else 0

        print(f"\n--- Heavy Concurrent Load Results ---")
        print(f"Duration: {elapsed:.1f}s, Workers: {num_workers}")
        print(f"Total requests: {total}")
        print(f"Success: {success} ({success_rate:.1%})")
        print(f"HTTP errors: {http_error}, Connection errors: {conn_error}")
        print(f"Throughput: {total / elapsed:.0f} req/s")

        # Print per-second throughput for the first and last 5 seconds
        sorted_secs = sorted(per_second.keys())
        if sorted_secs:
            print(f"Per-second throughput (first 5s): "
                  f"{[per_second.get(s, 0) for s in sorted_secs[:5]]}")
            print(f"Per-second throughput (last 5s): "
                  f"{[per_second.get(s, 0) for s in sorted_secs[-5:]]}")

        # Expect at least 90% success rate
        assert success_rate >= 0.9, (
            f"Success rate {success_rate:.1%} below 90% threshold. "
            f"{success}/{total} succeeded, {http_error} HTTP errors, "
            f"{conn_error} connection errors"
        )

        # Verify server is still alive after the onslaught
        response = client.get(f"{example_root}/index.html?post_heavy_load=1")
        assert_http_status(response, 200)


@pytest.mark.slow
@pytest.mark.iis_only
class TestRestartUnderLoad:
    """Test module behavior when IIS Express is restarted during active load."""

    @staticmethod
    def _run_ps_script(script_name, *args):
        """Run a PowerShell script from the test/iis directory."""
        script_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)))
        script_path = os.path.join(script_dir, script_name)
        cmd = [
            "powershell", "-ExecutionPolicy", "Bypass",
            "-File", script_path,
        ] + list(args)
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=60
        )
        return result

    @staticmethod
    def _wait_for_server(host, port, timeout=30):
        """Poll until the server responds on the given port."""
        import http.client
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                conn = http.client.HTTPConnection(host, port, timeout=3)
                conn.request("GET", "/index.html")
                resp = conn.getresponse()
                conn.close()
                if resp.status == 200:
                    return True
            except Exception:
                pass
            time.sleep(0.5)
        return False

    @staticmethod
    def _kill_all_iisexpress():
        """Kill all IIS Express processes using taskkill."""
        subprocess.run(
            ["taskkill", "/F", "/IM", "iisexpress.exe"],
            capture_output=True, timeout=15
        )
        # Also kill the tray app
        subprocess.run(
            ["taskkill", "/F", "/IM", "iisexpresstray.exe"],
            capture_output=True, timeout=15
        )

    @staticmethod
    def _wait_for_port_free(port, timeout=30):
        """Wait until nothing is listening on the given port."""
        import socket
        deadline = time.time() + timeout
        while time.time() < deadline:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1)
            try:
                sock.connect(("localhost", port))
                sock.close()
                # Still listening - wait
                time.sleep(1)
            except (ConnectionRefusedError, OSError):
                sock.close()
                return True
        return False

    def test_restart_under_concurrent_load(
        self, client: PageSpeedClient, example_root: str, server_config
    ):
        """Kill IIS Express while 50 concurrent workers are active, then restart.

        Verifies that:
        1. In-flight requests fail gracefully (no crashes/hangs in workers)
        2. IIS Express comes back cleanly after restart
        3. Post-restart requests succeed normally
        """
        if os.environ.get("IIS_EXPRESS", "0") != "1":
            pytest.skip("Restart test only works with IIS Express")

        num_workers = 50
        lock = threading.Lock()
        pre_counters = {"success": 0, "error": 0}
        stop_event = threading.Event()

        url_templates = [
            f"{example_root}/index.html",
            f"{example_root}/combine_css.html",
            f"{example_root}/styles/yellow.css",
        ]

        def worker(worker_id):
            w_client = PageSpeedClient(
                server_config.host, server_config.port, timeout=5.0
            )
            local_ok = 0
            local_err = 0
            req_num = 0

            while not stop_event.is_set():
                url_idx = req_num % len(url_templates)
                url = f"{url_templates[url_idx]}?w={worker_id}&r={req_num}"
                req_num += 1
                try:
                    response = w_client.get(url)
                    if response.status == 200:
                        local_ok += 1
                    else:
                        local_err += 1
                except Exception:
                    local_err += 1

            with lock:
                pre_counters["success"] += local_ok
                pre_counters["error"] += local_err

        # Phase 1: Start concurrent load
        threads = []
        for i in range(num_workers):
            t = threading.Thread(target=worker, args=(i,), daemon=True)
            threads.append(t)
            t.start()

        # Let load establish for 3 seconds
        time.sleep(3)

        # Phase 2: Kill IIS Express while load is active
        print("\n--- Phase 2: Killing IIS Express under load ---")
        self._kill_all_iisexpress()

        # Signal workers to stop (they'll hit errors since server is dead)
        stop_event.set()

        # Wait for all worker threads to finish
        for t in threads:
            t.join(timeout=15.0)

        pre_ok = pre_counters["success"]
        pre_err = pre_counters["error"]
        print(f"Pre-kill load: {pre_ok} ok, {pre_err} errors")

        # Verify workers didn't hang - all threads should have exited
        alive = sum(1 for t in threads if t.is_alive())
        assert alive == 0, f"{alive} worker threads still alive after stop"

        # Phase 3: Wait for port to become free, then restart
        port = server_config.port
        print(f"Waiting for port {port} to be free...")
        port_free = self._wait_for_port_free(port, timeout=30)
        print(f"Port free: {port_free}")
        assert port_free, f"Port {port} still occupied after 30s"

        print("--- Phase 3: Restarting IIS Express ---")
        start_result = self._run_ps_script(
            "Start-IISExpress.ps1", "-Background", "-Wait",
            "-Port", str(port)
        )
        print(f"Start exit={start_result.returncode}")
        for line in start_result.stdout.strip().splitlines():
            print(f"  [start] {line}")
        if start_result.stderr.strip():
            for line in start_result.stderr.strip().splitlines():
                print(f"  [start stderr] {line}")

        # Ensure server is actually up
        assert self._wait_for_server(server_config.host, port, timeout=30), \
            "IIS Express failed to restart"

        # Phase 4: Post-restart load test
        print("--- Phase 4: Post-restart load verification ---")
        post_counters = {"success": 0, "error": 0}
        stop_event_2 = threading.Event()

        def post_worker(worker_id):
            w_client = PageSpeedClient(
                server_config.host, server_config.port, timeout=5.0
            )
            local_ok = 0
            local_err = 0
            req_num = 0

            while not stop_event_2.is_set():
                url_idx = req_num % len(url_templates)
                url = f"{url_templates[url_idx]}?pw={worker_id}&r={req_num}"
                req_num += 1
                try:
                    response = w_client.get(url)
                    if response.status == 200:
                        local_ok += 1
                    else:
                        local_err += 1
                except Exception:
                    local_err += 1

            with lock:
                post_counters["success"] += local_ok
                post_counters["error"] += local_err

        post_threads = []
        for i in range(num_workers):
            t = threading.Thread(target=post_worker, args=(i,), daemon=True)
            post_threads.append(t)
            t.start()

        time.sleep(5)
        stop_event_2.set()
        for t in post_threads:
            t.join(timeout=10.0)

        post_ok = post_counters["success"]
        post_err = post_counters["error"]

        print(f"\n--- Restart Under Load Results ---")
        print(f"Pre-kill:  {pre_ok} ok, {pre_err} errors")
        print(f"Post-restart: {post_ok} ok, {post_err} errors")

        # Post-restart: server must be functional
        assert post_ok > 0, "No successful post-restart requests"
        if post_ok + post_err > 0:
            post_rate = post_ok / (post_ok + post_err)
            assert post_rate >= 0.9, (
                f"Post-restart success rate {post_rate:.1%} too low "
                f"({post_ok} ok, {post_err} errors). "
                f"Server may not have recovered properly."
            )

        # Final verification: single clean request
        response = client.get(f"{example_root}/index.html?post_restart=1")
        assert_http_status(response, 200)

    def test_restart_preserves_functionality(
        self, client: PageSpeedClient, example_root: str, server_config
    ):
        """After restart, all major features should still work.

        This test runs after the restart test above and verifies that HTML
        rewriting, IPRO, and admin endpoints all function correctly.
        """
        if os.environ.get("IIS_EXPRESS", "0") != "1":
            pytest.skip("Restart test only works with IIS Express")

        # Verify HTML rewriting works
        response = client.get(
            f"{example_root}/combine_css.html"
            f"?PageSpeedFilters=combine_css&post_restart_verify=1"
        )
        assert_http_status(response, 200)
        assert "</html>" in response.text.lower(), "HTML rewriting broken after restart"

        # Verify IPRO works
        response = client.get(
            f"{example_root}/styles/yellow.css?post_restart_verify=1"
        )
        assert_http_status(response, 200)
        assert len(response.body) > 0, "IPRO response empty after restart"

        # Verify admin endpoint works
        response = client.get("/pagespeed_admin/")
        assert_http_status(response, 200)

        # Verify image serving works
        response = client.get(
            f"{example_root}/images/sample.png?post_restart_verify=1"
        )
        assert_http_status(response, 200)
        assert len(response.body) > 100, "Image response too small after restart"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
