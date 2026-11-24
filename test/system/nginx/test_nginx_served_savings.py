#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""nginx: the sizes of optimized responses are counted each time one is sent,
from the cache as well, across a reload of the server.

The same check across a full stop and start of the server, with the cache
kept, is in test_nginx_zz_served_savings_restart.py: it runs from a module
whose name sorts last, so that the restart, which drops everything the
server holds in memory, does not happen in the middle of the suite.

The statistics the filters keep (image_rewrite_total_original_bytes and its
siblings) grow once, when a resource is optimized. After a reload they start
from zero again and stay there while every optimized resource is answered
from the cache. The *_served_*_bytes statistics count what was sent instead:
this test warms the cache with one image, one stylesheet and one script,
reloads nginx, fetches each rewritten URL several times with and without
gzip, and checks that each fetch added exactly the size of the source file
and the size of the optimized body, while the filters' own totals did not
move.

It states which path must answer: every fetch after the reload or restart has
to be a cache hit on the rewritten URL. If the first one is not (a cold cache would
rebuild the output instead), the test fails at once rather than measure a
different path.

The same is checked for resources optimized in place: one image, one
stylesheet and one script are requested under their own URLs until the
optimized bytes come back, and after the reload or restart every such
response has to add the size of the source file and the size of the body
sent, without a cache hit on a rewritten URL and without optimizing anything
again.

Not covered here: responses served from the optimizer's cache.

Environment: runs only on nginx and skips unless run_nginx_tests.sh exports
PAGESPEED_NGINX_PID_FILE and PAGESPEED_DOC_ROOT.
"""

import os
import re
import signal
import time
import uuid
from pathlib import Path
from typing import Callable, Dict, List, Set, Tuple

import pytest

from pagespeed_test_framework import PageSpeedClient
from pagespeed_test_framework.stats import settled_stats

FETCHES_PER_ENCODING = 3

# Moved once per optimization, not per response.
REWRITE_TIME_COUNTERS = (
    "image_rewrite_total_original_bytes",
    "image_rewrite_total_bytes_saved",
    "css_filter_total_original_bytes",
    "javascript_total_original_bytes",
    "image_rewrites",
)

SERVED_COUNTERS = (
    "image_served_original_bytes",
    "image_served_optimized_bytes",
    "css_served_original_bytes",
    "css_served_optimized_bytes",
    "javascript_served_original_bytes",
    "javascript_served_optimized_bytes",
)

CACHE_HITS = "resource_fetches_cached"

# (type, example page, filter, pattern of the rewritten URL in the page).
RESOURCES = (
    ("image", "rewrite_images.html", "rewrite_images",
     r'images/[^"\'\s>]*Puzzle\.jpg\.pagespeed\.ic\.[^"\'\s>]+'),
    ("css", "rewrite_css.html", "rewrite_css",
     r'styles/[^"\'\s>]*\.css\.pagespeed\.cf\.[^"\'\s>]+'),
    ("javascript", "rewrite_javascript.html", "rewrite_javascript",
     r'[^"\'\s>/]*rewrite_javascript\.js\.pagespeed\.jm\.[^"\'\s>]+'),
)

# (type, path under the test root) of resources optimized in place, under
# their own URL. Every call of check_served_sizes_across requests them with a
# query of its own (see there), so its cache entries are shared with no other
# test and with no other call: nothing warmed earlier can go stale, and be
# refreshed in the background, while a later call is measuring.
IN_PLACE_RESOURCES = (
    ("image", "ipro/test_image_dont_reuse.png"),
    ("css", "ipro/cc200/example.css"),
    ("javascript", "normal.js"),
)

RELOAD_TIMEOUT_S = 30.0


def _source_file(doc_root: Path, example_root: str, rewritten: str) -> Path:
    """The file on disk a rewritten URL (relative to example_root) was made from."""
    directory, _, leaf = rewritten.rpartition("/")
    name = leaf.split(".pagespeed.", 1)[0]
    filter_id = leaf.split(".pagespeed.", 1)[1].split(".", 1)[0]
    if filter_id == "cf":
        # A rewritten stylesheet's name starts with "A.".
        assert name.startswith("A."), leaf
        name = name[2:]
    elif filter_id == "ic":
        # An image's name starts with "x", or "<width>x<height>x" when resized.
        match = re.match(r"^(?:\d+x\d+)?x(.+)$", name)
        assert match, leaf
        name = match.group(1)
    return doc_root / example_root.strip("/") / directory / name


def _ps() -> List[Tuple[int, int, str]]:
    import subprocess

    res = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,comm="],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=15,
    )
    rows = []
    for line in res.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3:
            try:
                rows.append((int(parts[0]), int(parts[1]), parts[2].strip()))
            except ValueError:
                continue
    return rows


def _nginx_master_pid() -> int:
    """The pid in the lane's pid file, checked to be an nginx process.

    The pid is signalled below, so it is verified first: a positive pid other
    than 1 whose command is nginx, or the test fails without signalling.
    """
    pid_file = os.environ.get("PAGESPEED_NGINX_PID_FILE", "")
    if not pid_file or not os.path.isfile(pid_file):
        pytest.skip("PAGESPEED_NGINX_PID_FILE not set (run via run_nginx_tests.sh)")
    with open(pid_file, "r") as f:
        text = f.read().strip()
    assert text.isdigit(), f"pid file {pid_file} does not hold a pid: {text!r}"
    pid = int(text)
    assert pid > 1, f"refusing to signal pid {pid}"
    commands = {p: comm for p, _, comm in _ps()}
    assert "nginx" in commands.get(pid, ""), (
        f"pid {pid} from {pid_file} is not an nginx process: {commands.get(pid)!r}"
    )
    return pid


def _worker_pids(master_pid: int) -> Set[int]:
    return {pid for pid, ppid, comm in _ps() if ppid == master_pid and "nginx" in comm}


def _reload_nginx(client: PageSpeedClient, probe_path: str) -> None:
    """Reload nginx and wait until a new set of workers answers."""
    master = _nginx_master_pid()
    old_workers = _worker_pids(master)
    assert old_workers, "no nginx workers found before the reload"
    os.kill(master, signal.SIGHUP)
    deadline = time.monotonic() + RELOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        workers = _worker_pids(master)
        if workers and not (workers & old_workers):
            break
        time.sleep(0.25)
    else:
        pytest.fail(
            f"nginx workers {sorted(old_workers)} were not all replaced within "
            f"{RELOAD_TIMEOUT_S:.0f}s of the reload"
        )
    client.fetch_until(probe_path, lambda r: r.status == 200, timeout=RELOAD_TIMEOUT_S)


def restart_nginx(client: PageSpeedClient, probe_path: str) -> None:
    """Stop nginx completely and start it again with its cache directory kept.

    The start repeats the lane's own command line (run_nginx_tests.sh): the
    prefix is the directory of the configuration file.
    """
    import subprocess

    binary = os.environ.get("PAGESPEED_NGINX_BINARY", "")
    conf = os.environ.get("PAGESPEED_NGINX_CONFIG_FILE", "")
    error_log = os.environ.get("PAGESPEED_NGINX_ERROR_LOG", "")
    if not (binary and conf and error_log and os.path.isfile(binary) and os.path.isfile(conf)):
        pytest.skip(
            "PAGESPEED_NGINX_BINARY / _CONFIG_FILE / _ERROR_LOG not set "
            "(run via run_nginx_tests.sh)"
        )
    master = _nginx_master_pid()
    os.kill(master, signal.SIGQUIT)
    deadline = time.monotonic() + RELOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        if not any(pid == master or ppid == master for pid, ppid, _ in _ps()):
            break
        time.sleep(0.25)
    else:
        pytest.fail(f"nginx (master {master}) did not stop within {RELOAD_TIMEOUT_S:.0f}s")

    started = subprocess.run(
        [binary, "-p", os.path.dirname(conf), "-e", error_log, "-c", conf],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=120,
    )
    assert started.returncode == 0, f"nginx did not start again: {started.stdout[-2000:]}"
    pid_file = os.environ.get("PAGESPEED_NGINX_PID_FILE", "")
    deadline = time.monotonic() + RELOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        if os.path.isfile(pid_file) and open(pid_file).read().strip().isdigit():
            break
        time.sleep(0.25)
    else:
        pytest.fail("nginx wrote no pid file after it was started again")
    assert _nginx_master_pid() != master
    client.fetch_until(probe_path, lambda r: r.status == 200, timeout=RELOAD_TIMEOUT_S)


def _delta(before: Dict[str, int], after: Dict[str, int], name: str) -> int:
    return after.get(name, 0) - before.get(name, 0)


def check_served_sizes_across(
    bounce: Callable[[PageSpeedClient, str], None],
    client: PageSpeedClient,
    example_root: str,
    test_root: str,
    doc_root: Path,
    stats_snapshot: Callable[[], Dict[str, int]],
) -> None:
    first = stats_snapshot()
    missing = [name for name in SERVED_COUNTERS if name not in first]
    assert not missing, f"statistics missing from the server: {missing}"

    # Warm the cache: rewrite one resource of each type and fetch it once.
    urls: Dict[str, str] = {}
    originals: Dict[str, int] = {}
    optimized: Dict[str, int] = {}
    for kind, page, filter_name, pattern in RESOURCES:
        response = client.fetch_until(
            f"{example_root}/{page}?PageSpeedFilters={filter_name}",
            lambda r, pattern=pattern: re.search(pattern, r.text) is not None,
            timeout=100.0,
        )
        rewritten = re.search(pattern, response.text).group(0)
        url = f"{example_root}/{rewritten}"
        source = _source_file(doc_root, example_root, rewritten)
        assert source.is_file(), f"{kind}: source file {source} for {url} not found"
        body = client.get(url)
        assert body.status == 200, f"{kind}: {url} -> {body.status}"
        assert body.header("Content-Encoding") == "", f"{kind}: {url} came compressed"
        urls[kind] = url
        originals[kind] = source.stat().st_size
        optimized[kind] = len(body.body)
        assert optimized[kind] > 0

    # Warm the in-place half: request each resource under its own URL until
    # the optimized bytes come back.
    in_place_query = f"?served_sizes={uuid.uuid4().hex}"
    in_place_urls: Dict[str, str] = {}
    in_place_originals: Dict[str, int] = {}
    in_place_optimized: Dict[str, int] = {}
    for kind, path in IN_PLACE_RESOURCES:
        source = doc_root / test_root.strip("/") / path
        assert source.is_file(), f"{kind}: source file {source} not found"
        original_size = source.stat().st_size
        url = f"{test_root}/{path}{in_place_query}"
        response = client.fetch_until(
            url,
            lambda r, original_size=original_size: r.status == 200
            and 0 < len(r.body) < original_size,
            timeout=100.0,
        )
        assert response.header("Content-Encoding") == "", f"{kind}: {url} came compressed"
        in_place_urls[kind] = url
        in_place_originals[kind] = original_size
        in_place_optimized[kind] = len(response.body)

    bounce(client, f"{example_root}/rewrite_css.html?PageSpeed=off")

    watched = REWRITE_TIME_COUNTERS + SERVED_COUNTERS + (CACHE_HITS,)
    before = settled_stats(stats_snapshot, watched)

    # The first fetch after the reload decides whether this test measures the
    # path it means to: a cache hit on the rewritten URL, with nothing rebuilt.
    probe = client.get(urls["image"])
    assert probe.status == 200
    after_probe = stats_snapshot()
    if _delta(before, after_probe, CACHE_HITS) != 1 or any(
        _delta(before, after_probe, name) != 0 for name in REWRITE_TIME_COUNTERS
    ):
        moved = {
            name: _delta(before, after_probe, name)
            for name in watched
            if _delta(before, after_probe, name) != 0
        }
        pytest.fail(
            "the first fetch after the reload or restart was not answered from "
            f"the cache (the cache did not survive it?): counters moved {moved}"
        )
    assert _delta(before, after_probe, "image_served_original_bytes") == originals["image"]
    assert _delta(before, after_probe, "image_served_optimized_bytes") == len(probe.body)

    # Each type, with and without gzip: every response adds the size of the
    # source file and the uncompressed size of the optimized body.
    for kind in ("image", "css", "javascript"):
        start = stats_snapshot()
        for _ in range(FETCHES_PER_ENCODING):
            plain = client.get(urls[kind])
            assert plain.status == 200
            assert len(plain.body) == optimized[kind], f"{kind}: body changed"
            gzipped = client.get_gzip(urls[kind])
            assert gzipped.status == 200
            assert len(gzipped.body) == optimized[kind], f"{kind}: decoded body differs"
        end = stats_snapshot()
        fetches = 2 * FETCHES_PER_ENCODING
        assert _delta(start, end, CACHE_HITS) == fetches, f"{kind}: not all cache hits"
        for other in ("image", "css", "javascript"):
            expected_original = fetches * originals[kind] if other == kind else 0
            expected_optimized = fetches * optimized[kind] if other == kind else 0
            assert _delta(start, end, f"{other}_served_original_bytes") == expected_original, (
                f"{kind} fetches moved {other}_served_original_bytes"
            )
            assert _delta(start, end, f"{other}_served_optimized_bytes") == expected_optimized, (
                f"{kind} fetches moved {other}_served_optimized_bytes"
            )

    # Resources optimized in place, under their own URL: every response adds
    # the size of the source file and the uncompressed size of the body sent.
    # None of them is a cache hit on a rewritten URL.
    for kind in ("image", "css", "javascript"):
        start = stats_snapshot()
        for _ in range(FETCHES_PER_ENCODING):
            plain = client.get(in_place_urls[kind])
            assert plain.status == 200
            assert len(plain.body) == in_place_optimized[kind], (
                f"{kind}: {in_place_urls[kind]} was not answered with the optimized "
                "bytes after the reload or restart"
            )
            gzipped = client.get_gzip(in_place_urls[kind])
            assert gzipped.status == 200
            assert len(gzipped.body) == in_place_optimized[kind], (
                f"{kind}: decoded in-place body differs"
            )
        end = stats_snapshot()
        fetches = 2 * FETCHES_PER_ENCODING
        assert _delta(start, end, CACHE_HITS) == 0, f"{kind}: in-place counted as a cache hit"
        for other in ("image", "css", "javascript"):
            expected_original = fetches * in_place_originals[kind] if other == kind else 0
            expected_optimized = fetches * in_place_optimized[kind] if other == kind else 0
            assert _delta(start, end, f"{other}_served_original_bytes") == expected_original, (
                f"in-place {kind} fetches moved {other}_served_original_bytes"
            )
            assert _delta(start, end, f"{other}_served_optimized_bytes") == expected_optimized, (
                f"in-place {kind} fetches moved {other}_served_optimized_bytes"
            )

    # Requests that do not send a whole body to a visitor add nothing.
    start = stats_snapshot()
    conditional = client.get(
        urls["image"], headers={"If-Modified-Since": "Fri, 01 Jan 2016 00:00:00 GMT"}
    )
    assert conditional.status == 304
    assert client.head(urls["css"]).status == 200
    ranged = client.get(urls["javascript"], headers={"Range": "bytes=0-9"})
    assert ranged.status in (200, 206)
    assert client.head(in_place_urls["image"]).status == 200
    ranged = client.get(in_place_urls["css"], headers={"Range": "bytes=0-9"})
    assert ranged.status in (200, 206)
    end = stats_snapshot()
    for name in SERVED_COUNTERS:
        assert _delta(start, end, name) == 0, f"{name} moved for a request without a full body"

    # From the reload or restart to here nothing was optimized again.
    for name in REWRITE_TIME_COUNTERS:
        assert _delta(before, end, name) == 0, f"{name} moved after the reload"


@pytest.mark.nginx_only
@pytest.mark.requires_module
@pytest.mark.requires_stats
def test_served_sizes_are_counted_from_the_cache_across_a_reload(
    client: PageSpeedClient,
    example_root: str,
    test_root: str,
    doc_root: Path,
    stats_snapshot: Callable[[], Dict[str, int]],
):
    check_served_sizes_across(
        _reload_nginx, client, example_root, test_root, doc_root, stats_snapshot
    )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
