#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""mod_pagespeed Load Test Script.

Stress-tests the full nginx -> cache -> worker stack using concurrent
HTTP requests. Supports burst and sustained test modes.

Requires: pip install requests
"""

import argparse
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import requests

# Paths to test, with optional Accept header variants for images.
TEST_PATHS = [
    ("/style.css", None),
    ("/app.js", None),
    ("/index.html", None),
    ("/responsive.css", None),
    ("/interactive.js", None),
    ("/photo.jpg", None),
    ("/photo.jpg", "image/webp,*/*"),
    ("/photo.jpg", "image/avif,*/*"),
    ("/animation.gif", None),
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
        return latency_ms, r.status_code, None
    except requests.RequestException as e:
        latency_ms = (time.monotonic() - start) * 1000.0
        return latency_ms, 0, str(e)


def pick_target(index):
    """Round-robin through TEST_PATHS."""
    path, accept_hdr = TEST_PATHS[index % len(TEST_PATHS)]
    return path, accept_hdr


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


def print_report(latencies, errors, total, elapsed_s, label):
    """Print a summary report for a test run."""
    print(f"\n{'=' * 60}")
    print(f"  {label}")
    print(f"{'=' * 60}")
    print(f"  Total requests:    {total}")
    print(f"  Successful:        {total - errors}")
    print(f"  Errors:            {errors}")
    print(f"  Elapsed time:      {elapsed_s:.2f}s")
    print(f"  Throughput:        {total / elapsed_s:.1f} req/s")
    if latencies:
        latencies.sort()
        print(f"  Latency p50:       {percentile(latencies, 50):.1f}ms")
        print(f"  Latency p95:       {percentile(latencies, 95):.1f}ms")
        print(f"  Latency p99:       {percentile(latencies, 99):.1f}ms")
        print(f"  Latency min:       {latencies[0]:.1f}ms")
        print(f"  Latency max:       {latencies[-1]:.1f}ms")
        print(f"  Latency mean:      {statistics.mean(latencies):.1f}ms")
        if len(latencies) >= 2:
            print(f"  Latency stdev:     {statistics.stdev(latencies):.1f}ms")
    print(f"{'=' * 60}\n")


def run_burst(base_url, concurrency, timeout):
    """Burst mode: fire N concurrent requests and measure results."""
    print(f"[burst] Sending {concurrency} concurrent requests to {base_url}")
    latencies = []
    error_count = 0
    total = concurrency

    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=min(concurrency, 200)) as pool:
        futures = []
        for i in range(concurrency):
            path, accept_hdr = pick_target(i)
            futures.append(
                pool.submit(make_request, base_url, path, accept_hdr, timeout)
            )
        for future in as_completed(futures):
            latency_ms, status, error = future.result()
            if error or status >= 500:
                error_count += 1
            latencies.append(latency_ms)
    elapsed = time.monotonic() - start

    print_report(latencies, error_count, total, elapsed, "Burst Test Results")
    return error_count


def run_sustained(base_url, rps, duration, timeout):
    """Sustained mode: send X requests/second for Y seconds."""
    total_requests = rps * duration
    interval = 1.0 / rps if rps > 0 else 0.0
    print(
        f"[sustained] Target: {rps} req/s for {duration}s "
        f"({total_requests} total) to {base_url}"
    )

    latencies = []
    error_count = 0
    completed = 0

    # Use enough workers to avoid queuing at the thread pool level,
    # but cap to avoid resource exhaustion.
    workers = min(rps * 2, 500)

    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(workers, 10)) as pool:
        futures = []
        for i in range(total_requests):
            # Schedule at the correct time offset.
            target_time = start + i * interval
            now = time.monotonic()
            sleep_for = target_time - now
            if sleep_for > 0:
                time.sleep(sleep_for)

            path, accept_hdr = pick_target(i)
            futures.append(
                pool.submit(make_request, base_url, path, accept_hdr, timeout)
            )

            # Periodically collect completed futures to avoid memory buildup.
            if len(futures) >= 500:
                done = [f for f in futures if f.done()]
                for f in done:
                    latency_ms, status, error = f.result()
                    if error or status >= 500:
                        error_count += 1
                    latencies.append(latency_ms)
                    completed += 1
                futures = [f for f in futures if not f.done()]

        # Drain remaining futures.
        for future in as_completed(futures):
            latency_ms, status, error = future.result()
            if error or status >= 500:
                error_count += 1
            latencies.append(latency_ms)
            completed += 1

    elapsed = time.monotonic() - start
    print_report(latencies, error_count, completed, elapsed, "Sustained Test Results")
    return error_count


def main():
    parser = argparse.ArgumentParser(
        description="mod_pagespeed load test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  %(prog)s --mode burst --concurrency 200\n"
            "  %(prog)s --mode sustained --rps 50 --duration 30\n"
        ),
    )
    parser.add_argument(
        "--mode",
        choices=["burst", "sustained"],
        default="burst",
        help="Test mode (default: burst)",
    )
    parser.add_argument(
        "--concurrency",
        type=int,
        default=100,
        help="Number of concurrent requests for burst mode (default: 100)",
    )
    parser.add_argument(
        "--rps",
        type=int,
        default=20,
        help="Requests per second for sustained mode (default: 20)",
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=10,
        help="Duration in seconds for sustained mode (default: 10)",
    )
    parser.add_argument(
        "--url",
        default="http://localhost:8080",
        help="Base URL of the nginx server (default: http://localhost:8080)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=10,
        help="Per-request timeout in seconds (default: 10)",
    )
    args = parser.parse_args()

    if args.mode == "burst":
        errors = run_burst(args.url, args.concurrency, args.timeout)
    else:
        errors = run_sustained(args.url, args.rps, args.duration, args.timeout)

    sys.exit(1 if errors > 0 else 0)


if __name__ == "__main__":
    main()
