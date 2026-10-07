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

"""Statistics parsing utilities for PageSpeed system tests.

This module provides functions for parsing the PageSpeed statistics page
and extracting counter values. It mirrors the scrape_* functions from
the bash system_test_helpers.sh.
"""

import json
import re
import time
from typing import Callable, Dict, List, Optional, Sequence


def parse_statistics(content: str) -> Dict[str, int]:
    """Parse PageSpeed statistics page content into a dictionary.

    The statistics page has lines in format:
        counter_name: value
    or:
        counter_name value

    Equivalent to parsing output of: $WGET_DUMP $STATISTICS_URL

    Args:
        content: Raw text content of statistics page

    Returns:
        Dictionary mapping stat names to integer values
    """
    stats: Dict[str, int] = {}

    # Try JSON format first (mod_pagespeed returns JSON from statistics handler)
    stripped = content.strip()
    if stripped.startswith("{"):
        try:
            data = json.loads(stripped)
            if "variables" in data and isinstance(data["variables"], dict):
                return {k: int(v) for k, v in data["variables"].items()}
        except (json.JSONDecodeError, ValueError):
            pass  # Fall through to line-based parsing

    for line in content.splitlines():
        # Match patterns like:
        #   cache_hits: 42
        #   cache_hits 42
        #   image_rewrites:42
        match = re.match(r"^(\S+):?\s*(\d+)\s*$", line.strip())
        if match:
            name = match.group(1).rstrip(":")
            value = int(match.group(2))
            stats[name] = value

    return stats


def get_stat(stats: Dict[str, int], name: str, default: int = 0) -> int:
    """Get a statistic value by name.

    Equivalent to: get_stat from bash helpers

    Args:
        stats: Dictionary of statistics
        name: Name of statistic to get
        default: Value to return if stat not found

    Returns:
        Statistic value or default
    """
    return stats.get(name, default)


def scrape_header(headers_text: str, header_name: str) -> str:
    """Extract a header value from raw HTTP headers text.

    Equivalent to: scrape_header from bash helpers

    This is useful when working with raw wget output that includes
    headers in the response body.

    Args:
        headers_text: Raw HTTP headers as text
        header_name: Name of header to find (case-insensitive)

    Returns:
        Header value or empty string if not found
    """
    pattern = rf"^{re.escape(header_name)}:\s*(.+?)\r?$"
    match = re.search(pattern, headers_text, re.IGNORECASE | re.MULTILINE)
    return match.group(1).strip() if match else ""


def extract_headers(response_text: str) -> str:
    """Extract the headers section from a wget --save-headers dump.

    Equivalent to: extract_headers from bash helpers

    Headers are separated from body by a blank line (\\r\\n\\r\\n).

    Args:
        response_text: Full response including headers and body

    Returns:
        Just the headers portion
    """
    # Find the blank line separating headers from body
    # Try both \r\n\r\n and \n\n patterns
    for separator in ["\r\n\r\n", "\n\n"]:
        pos = response_text.find(separator)
        if pos != -1:
            return response_text[:pos]

    # No separator found, assume it's all headers
    return response_text


def scrape_content_length(headers_text: str) -> Optional[int]:
    """Extract Content-Length value from headers text.

    Equivalent to: scrape_content_length from bash helpers

    Args:
        headers_text: Raw HTTP headers as text

    Returns:
        Content-Length value as int, or None if not found
    """
    value = scrape_header(headers_text, "Content-Length")
    if value:
        try:
            return int(value)
        except ValueError:
            pass
    return None


def extract_beacon_params(html: str) -> Optional[Dict[str, str]]:
    """Extract beacon initialization parameters from HTML.

    PageSpeed injects a beacon initialization call like:
        pagespeed.criticalCssBeaconInit('/beacon', 'url', 'hash', 'nonce')

    This function extracts those parameters.

    Args:
        html: HTML content containing beacon init

    Returns:
        Dictionary with keys 'path', 'url', 'hash', 'nonce', or None
    """
    # Pattern matches criticalCssBeaconInit('path', 'url', 'hash', 'nonce')
    pattern = (
        r"criticalCssBeaconInit\(\s*"
        r"'([^']*)',\s*"  # path
        r"'([^']*)',\s*"  # url
        r"'([^']*)',\s*"  # hash
        r"'([^']*)'"  # nonce; the live snippet passes a 5th arg (selectors)
    )
    match = re.search(pattern, html)
    if match:
        return {
            "path": match.group(1),
            "url": match.group(2),
            "hash": match.group(3),
            "nonce": match.group(4),
        }
    return None


def count_pattern_matches(content: str, pattern: str) -> int:
    """Count occurrences of a pattern in content.

    Equivalent to: grep -c pattern

    Args:
        content: Text to search
        pattern: Regex pattern to count

    Returns:
        Number of matches
    """
    return len(re.findall(pattern, content))


def count_matching_lines(content: str, pattern: str) -> int:
    """Count the lines of content that match the regex pattern.

    Equivalent to: grep -c pattern   (lines, not occurrences)

    Args:
        content: Text to search
        pattern: Regex pattern to match against each line

    Returns:
        Number of lines with at least one match
    """
    regex = re.compile(pattern)
    return sum(1 for line in content.splitlines() if regex.search(line))


# Up-down statistics that count work still in flight across the whole server.
# While any of them is above zero, a counter that looks quiet can still move:
# a fetch that has not timed out yet (for example a worker's start-up fetch of
# a remote configuration that never answers) lands its cache insert seconds
# after the last visible change.
IN_FLIGHT_GAUGES = ("curl_fetch_active_count",)


def settled_stats(
    capture: Callable[[], Dict[str, int]],
    counters: Sequence[str],
    interval: float = 0.5,
    quiet_samples: int = 5,
    timeout: float = 60.0,
) -> Dict[str, int]:
    """Snapshot statistics once the server has stopped moving the named counters.

    The Apache lane keeps one statistics set for all vhosts and all worker
    processes, so asynchronous work left over from an earlier test (a rewrite
    finishing, a cache write, a freshen, a newly started worker's own start-up
    fetches) can land inside a later test's exact-delta window. Call this
    instead of ``capture()`` for the FIRST sample of such a window.

    The server counts as settled once ``quiet_samples`` consecutive snapshots,
    ``interval`` seconds apart, agree on every name in ``counters`` and every
    one of them reports nothing in flight. A snapshot that lacks any name in
    ``counters`` (an error page, a redirect body, a momentarily empty
    response) is never counted as quiet.

    "Nothing in flight" means every ``IN_FLIGHT_GAUGES`` statistic the server
    exposes reads zero. Which gauges a server exposes is taken from the first
    snapshot that has all of ``counters``: a gauge present there is required
    in every later snapshot; a gauge absent there is treated as not provided
    by this server (a port without that fetcher) and is only checked if it
    shows up later. On a server that exposes none of them the quiet criterion
    is the named counters alone, and a failure says so.

    The quiet criterion covers only the counters named and the exposed
    fetcher gauges. The module exposes no statistic for rewrites in flight, so
    asynchronous work that moves none of the named counters while it runs (a
    rewrite still computing on a worker thread) can still land inside a
    window this function has certified.

    Args:
        capture: Callable returning a fresh statistics snapshot.
        counters: Names of the counters that must stop moving.
        interval: Seconds to wait between snapshots.
        quiet_samples: Consecutive agreeing, idle snapshots required (>= 2).
        timeout: Seconds to wait before failing the test.

    Returns:
        The last snapshot of the quiet run.

    Raises:
        AssertionError: If the server does not settle within ``timeout``.
    """
    if quiet_samples < 2:
        raise ValueError(f"quiet_samples must be >= 2, got {quiet_samples}")

    exposed: Optional[List[str]] = None  # decided by the first complete sample
    deadline = time.monotonic() + timeout
    run: List[Dict[str, int]] = []
    while True:
        current = capture()
        missing = [c for c in counters if c not in current]
        if not missing and exposed is None:
            exposed = [g for g in IN_FLIGHT_GAUGES if g in current]
        if not missing and exposed:
            missing = [g for g in exposed if g not in current]
        busy = {} if missing else {
            g: current[g] for g in IN_FLIGHT_GAUGES
            if g in current and current[g] > 0}
        if missing or busy:
            run = []
        elif run and all(current[c] == run[0][c] for c in counters):
            run.append(current)
        else:
            run = [current]
        if len(run) >= quiet_samples:
            return current
        if time.monotonic() >= deadline:
            if missing:
                state = (f"the last snapshot ({len(current)} statistics) "
                         f"lacks {missing}")
            else:
                state = (f"last {dict((c, current[c]) for c in counters)}, "
                         f"in flight {busy}")
            if exposed is None:
                criterion = "no complete snapshot seen"
            elif exposed:
                criterion = f"with nothing in flight per {exposed}"
            else:
                criterion = ("counters only: this server exposes none of "
                             f"{list(IN_FLIGHT_GAUGES)}")
            raise AssertionError(
                f"statistics never settled within {timeout}s "
                f"({quiet_samples} identical samples {interval}s apart, "
                f"{criterion}): {state}"
            )
        time.sleep(interval)
