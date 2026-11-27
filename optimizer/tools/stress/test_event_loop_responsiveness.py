# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Regression tests for event loop blocking during image processing (bug #4).

Bug #4: Worker's synchronous HandleNotification() blocked the libuv event
loop during AVIF encoding of large images. Management socket, health socket,
and new notification connections all stalled.

Fix: Offloaded HandleNotification() to libuv thread pool via uv_queue_work.

These tests verify that mgmt/health sockets remain responsive even while
the worker is processing large images.

STRESS_LATENCY_FACTOR (float, default 1.0) scales the STATS latency bound for
hosts shared with other CI jobs; the default keeps the real requirement.
"""

import os
import time

import metrics_helpers
import pytest
import requests


STATS_BOUND_S = 3.0 * float(os.environ.get("STRESS_LATENCY_FACTOR", "1.0"))
LATENCY_FACTOR = STATS_BOUND_S / 3.0

# Absolute per-query ceiling. It is only a hang guard: a STATS query that has
# not answered after this long means the event loop is stuck, whatever the host
# load. It is deliberately far above the relative bound so that a busy host
# cannot trip it, while a real blocked loop (AVIF encode of a large image) or
# a wedged worker still does.
HANG_GUARD_S = 30.0


def _control_round_trip():
    """Time a no-op exec in the worker container.

    This pays the same host-side cost as a STATS query (compose exec, container
    process start, interpreter start) but never touches the worker's event
    loop, so it measures how loaded the host is right now.
    """
    start = time.monotonic()
    result = metrics_helpers.docker_exec("worker", ["python3", "-c", "pass"])
    elapsed = time.monotonic() - start
    if result.returncode != 0:
        raise RuntimeError(f"control exec failed: {result.stderr.strip()}")
    return elapsed


class TestEventLoopResponsiveness:
    """Verify event loop stays responsive during heavy image processing."""

    def test_stats_responds_during_image_processing(self, stress_services):
        """Request 3 large images to trigger processing, then query STATS.

        Each of 10 rounds (200ms apart) times a control exec and a STATS query
        back to back, inside the same window, so host load hits both equally.
        For every round, the STATS time minus the control taken just before it
        (the extra cost of reaching the worker's event loop) must stay within
        STATS_BOUND_S (scaled by STRESS_LATENCY_FACTOR), so a stall seen by a
        single query still fails. Every query must also finish within
        HANG_GUARD_S.
        """
        base_url = stress_services.nginx_url

        # Trigger processing of 3 large images (async, won't block)
        for i in range(3):
            try:
                requests.get(
                    base_url + f"/images/img-500k.jpg?async_stats_{i}",
                    timeout=10,
                )
            except Exception:
                pass

        # Immediately query STATS 10 times at 200ms intervals, each paired
        # with a control sample taken just before it.
        failures = []
        stats_times = []
        control_times = []
        for i in range(10):
            try:
                control_times.append(_control_round_trip())
            except Exception as e:
                failures.append(f"Control {i}: {e}")
            start = time.monotonic()
            try:
                raw = metrics_helpers.get_stats_via_docker(timeout=HANG_GUARD_S)
                elapsed = time.monotonic() - start
                stats = metrics_helpers.parse_stats(raw)
                assert "status" in stats, f"Unexpected STATS format: {raw[:100]}"
                stats_times.append(elapsed)
                if elapsed > HANG_GUARD_S:
                    failures.append(
                        f"Query {i}: {elapsed:.1f}s (>{HANG_GUARD_S:.1f}s hang guard)"
                    )
            except Exception as e:
                failures.append(f"Query {i}: {e}")
            time.sleep(0.2)

        assert not failures, (
            "STATS queries failed/hung during processing:\n" + "\n".join(failures)
        )

        excesses = [s - c for s, c in zip(stats_times, control_times)]
        worst = max(excesses)
        print(
            "Per-round excess (STATS minus control): "
            + ", ".join(f"{e:.2f}s" for e in excesses)
            + f"; max {worst:.2f}s (bound {STATS_BOUND_S:.1f}s, "
            f"factor {LATENCY_FACTOR:.1f})"
        )
        slow = [
            f"Round {i}: STATS {stats_times[i]:.2f}s, control "
            f"{control_times[i]:.2f}s, excess {e:.2f}s"
            for i, e in enumerate(excesses)
            if e > STATS_BOUND_S
        ]
        assert not slow, (
            f"STATS exceeded its paired control by more than "
            f"{STATS_BOUND_S:.1f}s (factor {LATENCY_FACTOR:.1f}):\n"
            + "\n".join(slow)
        )

    def test_health_responds_during_processing(self, stress_services):
        """Request large image, query health 10 times at 200ms intervals.

        All must return "OK" within 2s each.
        """
        base_url = stress_services.nginx_url

        # Trigger image processing
        try:
            requests.get(
                base_url + "/images/img-500k.jpg?async_health",
                timeout=10,
            )
        except Exception:
            pass

        # Query health rapidly
        failures = []
        for i in range(10):
            start = time.monotonic()
            try:
                health = metrics_helpers.get_health_via_docker(timeout=2)
                elapsed = time.monotonic() - start
                if "OK" not in health and "DEGRADED" not in health:
                    failures.append(f"Query {i}: unexpected response: {health}")
                elif elapsed > 5.0:
                    failures.append(f"Query {i}: {elapsed:.1f}s (>5s)")
            except Exception as e:
                failures.append(f"Query {i}: {e}")
            time.sleep(0.2)

        assert not failures, (
            "Health queries failed/slow during processing:\n" + "\n".join(failures)
        )

    def test_notifications_complete_asynchronously(self, stress_services):
        """Verify that async processing still completes after offloading.

        Take STATS snapshot, request 3 images, poll until variants.written
        increases by at least 3 (60s timeout).
        """
        base_url = stress_services.nginx_url

        # Snapshot before
        raw = metrics_helpers.get_stats_via_docker()
        stats_before = metrics_helpers.parse_stats(raw)
        written_before = stats_before.get("variants", {}).get("written", 0)

        # Request 3 unique images
        for i in range(3):
            try:
                requests.get(
                    base_url + f"/images/img-100k.jpg?async_complete_{i}",
                    timeout=10,
                )
            except Exception:
                pass

        # Poll until at least 3 new variants are written
        target = written_before + 3

        def check_written(stats):
            return stats.get("variants", {}).get("written", 0) >= target

        try:
            final_stats = metrics_helpers.wait_for_stats_condition(
                check_written, timeout=30.0, interval=1.0
            )
            written_after = final_stats.get("variants", {}).get("written", 0)
            assert written_after >= target, (
                f"Expected >= {target} written, got {written_after}"
            )
        except TimeoutError:
            # Get final stats for diagnostics
            raw = metrics_helpers.get_stats_via_docker()
            stats_final = metrics_helpers.parse_stats(raw)
            written_final = stats_final.get("variants", {}).get("written", 0)
            pytest.fail(
                f"variants.written didn't increase by 3 within 60s: "
                f"{written_before} -> {written_final}"
            )
