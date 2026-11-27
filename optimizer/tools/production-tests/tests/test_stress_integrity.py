# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Stress and data integrity tests for the caching reverse proxy."""

import hashlib
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from uuid import uuid4

import pytest


class TestStressIntegrity:
    """Verify data integrity, concurrency safety, and recovery under load."""

    # ------------------------------------------------------------------
    # P0: Hard gate for production
    # ------------------------------------------------------------------

    @pytest.mark.p0
    def test_response_body_hash_under_load(self, client):
        """SHA-256 of cached responses stays consistent across concurrent reads."""
        num_paths = 10
        readers = 20
        reads_per_reader = 10

        # Pre-populate cache with unique paths and record expected hashes.
        paths = [f"/content/hash-load-{uuid4().hex}" for _ in range(num_paths)]
        expected = {}
        for p in paths:
            client.get(p)
        for p in paths:
            hit = client.poll_for_hit(p, max_attempts=60)
            expected[p] = hashlib.sha256(hit.content).hexdigest()

        # Concurrent reads verifying hash.
        errors = []

        def _read_many(path):
            local_errors = []
            for _ in range(reads_per_reader):
                try:
                    r = client.get(path)
                    if r.status_code != 200:
                        local_errors.append(f"{path}: status {r.status_code}")
                        continue
                    h = hashlib.sha256(r.content).hexdigest()
                    if h != expected[path]:
                        local_errors.append(
                            f"{path}: hash mismatch {h[:16]} != {expected[path][:16]}"
                        )
                except Exception as exc:
                    local_errors.append(f"{path}: {exc}")
            return local_errors

        with ThreadPoolExecutor(max_workers=readers) as pool:
            futures = [
                pool.submit(_read_many, paths[i % num_paths]) for i in range(readers)
            ]
            for f in as_completed(futures):
                errors.extend(f.result())

        assert errors == [], f"{len(errors)} hash errors:\n" + "\n".join(errors[:20])

    @pytest.mark.p0
    def test_content_length_accuracy_under_load(self, client):
        """Content-Length matches body on 500 concurrent requests."""
        path = f"/status/200?cache=3600&body=cl-load-{uuid4().hex}"
        # Pin Accept-Encoding: identity so Content-Length matches the actual
        # body (requests auto-decompresses gzip, causing CL mismatch).
        ae = {"Accept-Encoding": "identity"}
        client.get(path, headers=ae)
        client.poll_for_hit(path, headers=ae)

        errors = []

        def _check_cl(_i):
            try:
                r = client.get(path, headers=ae)
                cl = r.headers.get("Content-Length")
                if cl is not None and int(cl) != len(r.content):
                    return f"req {_i}: Content-Length {cl} != body {len(r.content)}"
            except Exception as exc:
                return f"req {_i}: {exc}"
            return None

        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(_check_cl, i) for i in range(500)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} CL errors:\n" + "\n".join(errors[:20])

    @pytest.mark.p0
    def test_response_mixing_isolation(self, client):
        """10 unique paths with unique content never return wrong content."""
        num_paths = 10
        readers_per_path = 10

        paths = [f"/content/mix-{uuid4().hex}" for _ in range(num_paths)]
        expected_bodies = {}
        for p in paths:
            client.get(p)
        for p in paths:
            hit = client.poll_for_hit(p, max_attempts=60)
            expected_bodies[p] = hit.content

        errors = []

        def _verify(path):
            local_errors = []
            for _ in range(readers_per_path):
                try:
                    r = client.get(path)
                    if r.status_code != 200:
                        local_errors.append(f"{path}: status {r.status_code}")
                        continue
                    if r.content != expected_bodies[path]:
                        local_errors.append(
                            f"{path}: body mismatch "
                            f"(got {len(r.content)}B, "
                            f"want {len(expected_bodies[path])}B)"
                        )
                except Exception as exc:
                    local_errors.append(f"{path}: {exc}")
            return local_errors

        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(_verify, p) for p in paths]
            for f in as_completed(futures):
                errors.extend(f.result())

        assert errors == [], f"{len(errors)} mixing errors:\n" + "\n".join(errors[:20])

    @pytest.mark.p0
    def test_thundering_herd_coalescing(self, client):
        """50 concurrent requests for same uncached URL all get valid responses."""
        path = f"/content/herd-{uuid4().hex}"
        errors = []

        def _request(i):
            try:
                r = client.get(path)
                if r.status_code != 200:
                    return f"req {i}: status {r.status_code}"
                if len(r.content) == 0:
                    return f"req {i}: empty body"
            except Exception as exc:
                return f"req {i}: {exc}"
            return None

        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(_request, i) for i in range(50)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} herd errors:\n" + "\n".join(errors[:20])

    @pytest.mark.p0
    def test_concurrent_variant_read_write(self, client):
        """Reads during worker variant processing all succeed."""
        path = f"/content/variantrace-{uuid4().hex}"
        # Trigger caching so worker starts processing variants.
        client.get(path)

        errors = []

        def _read(i):
            try:
                r = client.get(path)
                if r.status_code != 200:
                    return f"reader {i}: status {r.status_code}"
                if len(r.content) == 0:
                    return f"reader {i}: empty body"
            except Exception as exc:
                return f"reader {i}: {exc}"
            return None

        # Immediately start readers while worker is processing.
        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(_read, i) for i in range(20)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} variant read errors:\n" + "\n".join(
            errors[:20]
        )

    @pytest.mark.p0
    def test_compressed_variant_fidelity(self, client):
        """Gzip variant content matches identity content after decompression."""
        path = f"/content/gzfidelity-{uuid4().hex}"
        ae_identity = {"Accept-Encoding": "identity"}
        client.get(path, headers=ae_identity)

        # Wait for the worker to process and write the minified identity +
        # compressed variants.  Use explicit Accept-Encoding: identity so
        # we don't accidentally get a compressed variant that was already
        # written by the worker.
        time.sleep(3)
        hit_identity = client.poll_for_hit(path, headers=ae_identity)
        identity_body = hit_identity.content

        # Request gzip variant. The `requests` library auto-decompresses
        # gzip, so hit_gzip.content should already be decompressed and
        # match the identity body directly.
        hit_gzip = client.poll_for_hit(path, headers={"Accept-Encoding": "gzip"})
        # After auto-decompression by requests, content should match
        assert hit_gzip.content == identity_body, (
            f"Gzip variant body ({len(hit_gzip.content)}B) != "
            f"identity body ({len(identity_body)}B)"
        )

    @pytest.mark.p0
    def test_etag_consistency_under_load(self, client):
        """50 concurrent requests for same cached path all return same ETag."""
        path = f"/content/etagload-{uuid4().hex}"
        # Pin Accept-Encoding: identity so all requests hit the same variant.
        # Without this, the `requests` library sends "gzip, deflate, br" by
        # default, potentially selecting a compressed variant with a different
        # ETag than the identity variant.
        ae = {"Accept-Encoding": "identity"}
        client.get(path, headers=ae)
        hit = client.poll_for_hit(path, headers=ae)
        expected_etag = hit.headers.get("ETag")

        errors = []

        def _check_etag(i):
            try:
                r = client.get(path, headers=ae)
                etag = r.headers.get("ETag")
                if expected_etag is not None and etag != expected_etag:
                    return f"req {i}: ETag {etag!r} != expected {expected_etag!r}"
            except Exception as exc:
                return f"req {i}: {exc}"
            return None

        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(_check_etag, i) for i in range(50)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} ETag errors:\n" + "\n".join(errors[:20])

    @pytest.mark.p0
    def test_alternate_list_integrity(self, client):
        """Identity, gzip, and brotli variants all return valid content."""
        path = f"/content/altlist-{uuid4().hex}"
        client.get(path)
        hit_identity = client.poll_for_hit(path)
        identity_body = hit_identity.content
        assert len(identity_body) > 0, "Identity body is empty"

        # Wait for compressed variants to be produced.
        hit_gzip = client.poll_for_hit(path, headers={"Accept-Encoding": "gzip"})
        assert hit_gzip.status_code == 200
        assert len(hit_gzip.content) > 0, "Gzip variant body is empty"

        hit_br = client.poll_for_hit(path, headers={"Accept-Encoding": "br"})
        assert hit_br.status_code == 200
        assert len(hit_br.content) > 0, "Brotli variant body is empty"

    # ------------------------------------------------------------------
    # P1: Soft gate, review exceptions
    # ------------------------------------------------------------------

    @pytest.mark.p1
    def test_cache_full_continuous_write(self, client):
        """Cache serves reads correctly even after eviction pressure."""
        # Write many URLs to create eviction pressure.
        base = uuid4().hex
        num_urls = 100
        for i in range(num_urls):
            r = client.get(f"/size/100kb?cache=3600&_={base}-fill-{i}")
            assert r.status_code == 200

        # Verify a recently written URL is still readable.
        check_path = f"/size/100kb?cache=3600&_={base}-fill-{num_urls - 1}"
        r = client.get(check_path, timeout=30)
        assert r.status_code == 200
        assert len(r.content) == 100 * 1024

    @pytest.mark.p1
    def test_worker_thread_pool_saturation(self, client, metrics_client):
        """Worker recovers after receiving many notifications rapidly."""
        base = uuid4().hex
        # Cache many new URLs rapidly to flood worker notifications.
        for i in range(50):
            client.get(f"/content/saturate-{base}-{i}")

        # Give the worker time to recover.
        time.sleep(10)

        # Verify worker is still responsive by checking stats.
        stats = metrics_client.get_stats()
        assert isinstance(stats, dict), "Worker stats not a dict"

        # Verify a new request still works end-to-end.
        fresh = f"/content/post-saturate-{uuid4().hex}"
        r = client.get(fresh)
        assert r.status_code == 200

    @pytest.mark.p1
    @pytest.mark.xfail(
        reason="PURGE unreliable under cache pressure (known Cyclone limitation)",
        strict=False,
    )
    def test_miss_storm_after_purge(self, client, purge):
        """Purge + 20 concurrent requests: all get valid responses."""
        path = f"/content/purgestorm-{uuid4().hex}"
        client.get(path)
        client.poll_for_hit(path)

        # Purge the cached entry.
        purge(path)
        time.sleep(1)

        errors = []

        def _request(i):
            try:
                r = client.get(path)
                if r.status_code != 200:
                    return f"req {i}: status {r.status_code}"
                if len(r.content) == 0:
                    return f"req {i}: empty body"
            except Exception as exc:
                return f"req {i}: {exc}"
            return None

        with ThreadPoolExecutor(max_workers=20) as pool:
            futures = [pool.submit(_request, i) for i in range(20)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} post-purge errors:\n" + "\n".join(
            errors[:20]
        )

    @pytest.mark.p1
    def test_slow_origin_cold_cache(self, client):
        """Concurrent requests to a 5-second delayed origin all succeed."""
        path = f"/delay/5?_={uuid4().hex}"
        errors = []

        def _slow_request(i):
            try:
                r = client.get(path, timeout=30)
                if r.status_code != 200:
                    return f"req {i}: status {r.status_code}"
            except Exception as exc:
                return f"req {i}: {exc}"
            return None

        with ThreadPoolExecutor(max_workers=5) as pool:
            futures = [pool.submit(_slow_request, i) for i in range(5)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} slow-origin errors:\n" + "\n".join(
            errors[:20]
        )

    @pytest.mark.p1
    def test_ipc_socket_reconnection(self, metrics_client):
        """Worker stats endpoint responds to sequential queries."""
        for _ in range(3):
            stats = metrics_client.get_stats()
            assert isinstance(stats, dict), "Worker stats not a dict"
            time.sleep(1)

    @pytest.mark.p1
    def test_ram_disk_cache_consistency(self, client):
        """Multiple reads of the same cached path return identical content."""
        path = f"/content/ramdisk-{uuid4().hex}"
        client.get(path)
        hit = client.poll_for_hit(path)
        expected = hit.content

        for i in range(10):
            r = client.get(path)
            assert r.status_code == 200
            assert r.content == expected, (
                f"Read {i}: body mismatch ({len(r.content)}B vs {len(expected)}B)"
            )

    @pytest.mark.p1
    def test_large_file_not_recorded(self, client):
        """11MB response is served but stays MISS (exceeds recording limit)."""
        path = f"/size/11mb?cache=3600&_={uuid4().hex}"
        r = client.get(path, timeout=120)
        assert r.status_code == 200
        assert len(r.content) == 11 * 1024 * 1024
        client.assert_miss(r)

        time.sleep(3)

        r2 = client.get(path, timeout=120)
        assert r2.status_code == 200
        client.assert_miss(r2)

    @pytest.mark.p1
    def test_cache_miss_cascade(self, client):
        """HTML referencing CSS/images triggers processing of sub-resources."""
        uid = uuid4().hex
        html_path = f"/content/html?_={uid}"
        r = client.get(html_path)
        assert r.status_code == 200

        # Wait for HTML to be cached.
        client.poll_for_hit(html_path)

        # The referenced /content/css and /content/image should also
        # eventually be processed by the worker.
        css_hit = client.poll_for_hit("/content/css", max_attempts=30, interval=1.0)
        assert css_hit.status_code == 200
        assert len(css_hit.content) > 0

    # ------------------------------------------------------------------
    # P2: Rare scenarios, completeness
    # ------------------------------------------------------------------

    @pytest.mark.p2
    def test_hot_url_tracker_dedup(self, client, metrics_client):
        """Rapid repeated requests for the same URL are deduplicated by worker."""
        stats_before = metrics_client.get_stats()
        notifications_before = stats_before.get("notifications_received", 0)

        path = f"/content/hoturl-{uuid4().hex}"
        for _ in range(100):
            client.get(path)

        time.sleep(5)
        stats_after = metrics_client.get_stats()
        notifications_after = stats_after.get("notifications_received", 0)

        delta = notifications_after - notifications_before
        # The worker should not have processed 100 separate notifications
        # for the same URL. Some dedup is expected; allow up to 50.
        assert delta < 50, (
            f"Worker received {delta} notifications for 100 requests "
            f"to the same URL (expected deduplication)"
        )

    @pytest.mark.p2
    def test_cache_corruption_recovery(self, client):
        """Cache stays functional after many read/write cycles."""
        base = uuid4().hex
        num_cycles = 30

        for i in range(num_cycles):
            path = f"/content/corrupt-{base}-{i}"
            r = client.get(path)
            assert r.status_code == 200

        # Verify the last few entries are readable.
        for i in range(num_cycles - 5, num_cycles):
            path = f"/content/corrupt-{base}-{i}"
            r = client.get(path)
            assert r.status_code == 200
            assert len(r.content) > 0

    @pytest.mark.p2
    def test_management_socket_concurrent(self, metrics_client):
        """10 concurrent STATS queries all return valid JSON."""
        errors = []

        def _query_stats(i):
            try:
                stats = metrics_client.get_stats()
                if not isinstance(stats, dict):
                    return f"thread {i}: stats not a dict"
            except Exception as exc:
                return f"thread {i}: {exc}"
            return None

        with ThreadPoolExecutor(max_workers=10) as pool:
            futures = [pool.submit(_query_stats, i) for i in range(10)]
            for f in as_completed(futures):
                err = f.result()
                if err:
                    errors.append(err)

        assert errors == [], f"{len(errors)} socket errors:\n" + "\n".join(errors[:10])

    @pytest.mark.p2
    def test_websocket_connection_churn(self, metrics_client):
        """Management socket handles rapid connect/disconnect cycles."""
        errors = []

        for i in range(10):
            try:
                stats = metrics_client.get_stats(timeout=10)
                assert isinstance(stats, dict)
            except Exception as exc:
                errors.append(f"cycle {i}: {exc}")

        assert errors == [], f"{len(errors)} churn errors:\n" + "\n".join(errors[:10])
