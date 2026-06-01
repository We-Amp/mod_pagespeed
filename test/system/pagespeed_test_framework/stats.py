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

import re
from typing import Dict, Optional


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
        r"'([^']*)'\s*"   # nonce
        r"\)"
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
