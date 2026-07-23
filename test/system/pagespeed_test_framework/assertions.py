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

"""Custom assertions for PageSpeed system tests.

This module provides assertion functions that mirror the check_* functions
from the bash system_test_helpers.sh.
"""

import re
from typing import Dict, Optional, Union

from pagespeed_test_framework.client import Response

# Magic-byte signatures for the image formats the rewriter can emit, longest
# discriminator first. AVIF is ISOBMFF: bytes 4..8 are the "ftyp" box type and
# bytes 8..12 the major brand, which is "avif" for a still image and "avis" for
# an animated one -- so the brand must be matched as a set, never as a single
# 8-byte "ftypavif" compare.
_AVIF_BRANDS = (b"avif", b"avis")


def _detect_image_format(body: bytes) -> Optional[str]:
    """Identify an image format from its leading bytes.

    Args:
        body: Raw image bytes

    Returns:
        Lowercase format name ("avif", "webp", "jpeg", "png", "gif"), or None
        if the bytes match no format this framework knows about.
    """
    if body[0:2] == b"\xff\xd8":
        return "jpeg"
    if body[0:4] == b"RIFF" and body[8:12] == b"WEBP":
        return "webp"
    if body[4:8] == b"ftyp" and body[8:12] in _AVIF_BRANDS:
        return "avif"
    if body[0:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if body[0:6] in (b"GIF87a", b"GIF89a"):
        return "gif"
    return None


def assert_contains(
    content: Union[str, bytes, Response],
    pattern: str,
    msg: str = "",
    flags: int = 0,
) -> None:
    """Assert that content contains the pattern (regex).

    Equivalent to: check_from "$OUT" grep "$PATTERN"

    Args:
        content: String, bytes, or Response to search
        pattern: Regex pattern to find
        msg: Optional message for assertion failure
        flags: Regex flags (e.g., re.DOTALL, re.IGNORECASE)

    Raises:
        AssertionError: If pattern not found
    """
    if isinstance(content, Response):
        text = content.text
    elif isinstance(content, bytes):
        text = content.decode("utf-8", errors="replace")
    else:
        text = content

    match = re.search(pattern, text, flags)
    if not match:
        prefix = f"{msg}: " if msg else ""
        # Show snippet of content for debugging
        snippet = text[:500] + "..." if len(text) > 500 else text
        raise AssertionError(f"{prefix}Pattern '{pattern}' not found in:\n{snippet}")


def assert_not_contains(
    content: Union[str, bytes, Response],
    pattern: str,
    msg: str = "",
) -> None:
    """Assert that content does NOT contain the pattern.

    Equivalent to: check_not_from "$OUT" grep "$PATTERN"

    Args:
        content: String, bytes, or Response to search
        pattern: Regex pattern that should NOT be present
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If pattern is found
    """
    if isinstance(content, Response):
        text = content.text
    elif isinstance(content, bytes):
        text = content.decode("utf-8", errors="replace")
    else:
        text = content

    match = re.search(pattern, text)
    if match:
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Pattern '{pattern}' unexpectedly found at position {match.start()}"
        )


def assert_http_status(
    response: Response,
    expected: int = 200,
    msg: str = "",
) -> None:
    """Assert HTTP response status code.

    Equivalent to: check_200_http_response

    Args:
        response: Response object to check
        expected: Expected status code (default 200)
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If status doesn't match
    """
    if response.status != expected:
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Expected HTTP {expected}, got {response.status} for {response.url}"
        )


def assert_header_equals(
    response: Response,
    header_name: str,
    expected_value: str,
    msg: str = "",
) -> None:
    """Assert that a response header has an exact value.

    Args:
        response: Response object to check
        header_name: Name of header (case-insensitive)
        expected_value: Expected header value
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If header value doesn't match
    """
    actual = response.header(header_name)
    if actual != expected_value:
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Header '{header_name}': expected '{expected_value}', got '{actual}'"
        )


def assert_header_contains(
    response: Response,
    header_name: str,
    pattern: str,
    msg: str = "",
) -> None:
    """Assert that a response header contains a pattern.

    Args:
        response: Response object to check
        header_name: Name of header (case-insensitive)
        pattern: Regex pattern to find in header value
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If pattern not found in header
    """
    actual = response.header(header_name)
    if not re.search(pattern, actual):
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Header '{header_name}': pattern '{pattern}' not found in '{actual}'"
        )


def assert_stat_delta(
    old_stats: Dict[str, int],
    new_stats: Dict[str, int],
    stat_name: str,
    expected_delta: int,
    msg: str = "",
) -> None:
    """Assert that a statistic changed by exactly the expected amount.

    Equivalent to: check_stat $OLDSTATS $NEWSTATS counter_name expected_diff

    Args:
        old_stats: Statistics before operation
        new_stats: Statistics after operation
        stat_name: Name of statistic to check
        expected_delta: Expected change in value
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If delta doesn't match
    """
    old_val = old_stats.get(stat_name, 0)
    new_val = new_stats.get(stat_name, 0)
    actual_delta = new_val - old_val

    if actual_delta != expected_delta:
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Stat '{stat_name}' delta: expected {expected_delta}, "
            f"got {actual_delta} (old={old_val}, new={new_val})"
        )


def assert_stat_increased(
    old_stats: Dict[str, int],
    new_stats: Dict[str, int],
    stat_name: str,
    min_increase: int = 1,
    msg: str = "",
) -> None:
    """Assert that a statistic increased by at least a minimum amount.

    Equivalent to: check_stat_op $OLD $NEW counter ">=" min

    Args:
        old_stats: Statistics before operation
        new_stats: Statistics after operation
        stat_name: Name of statistic to check
        min_increase: Minimum required increase (default 1)
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If increase is less than minimum
    """
    old_val = old_stats.get(stat_name, 0)
    new_val = new_stats.get(stat_name, 0)
    actual_delta = new_val - old_val

    if actual_delta < min_increase:
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Stat '{stat_name}' expected increase >= {min_increase}, "
            f"got {actual_delta} (old={old_val}, new={new_val})"
        )


def assert_stat_unchanged(
    old_stats: Dict[str, int],
    new_stats: Dict[str, int],
    stat_name: str,
    msg: str = "",
) -> None:
    """Assert that a statistic did not change.

    Args:
        old_stats: Statistics before operation
        new_stats: Statistics after operation
        stat_name: Name of statistic to check
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If stat changed
    """
    assert_stat_delta(old_stats, new_stats, stat_name, 0, msg)


def assert_file_size(
    content: Union[bytes, Response],
    operator: str,
    expected_size: int,
    msg: str = "",
) -> None:
    """Assert that content size meets a constraint.

    Equivalent to: check_file_size "$FILE" -le 60000

    Args:
        content: Bytes or Response to check size of
        operator: Comparison operator ("-lt", "-le", "-eq", "-ge", "-gt")
        expected_size: Size to compare against
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If size constraint not met
        ValueError: If operator is invalid
    """
    if isinstance(content, Response):
        size = len(content.body)
    else:
        size = len(content)

    operators = {
        "-lt": (lambda a, b: a < b, "<"),
        "-le": (lambda a, b: a <= b, "<="),
        "-eq": (lambda a, b: a == b, "=="),
        "-ge": (lambda a, b: a >= b, ">="),
        "-gt": (lambda a, b: a > b, ">"),
        "<": (lambda a, b: a < b, "<"),
        "<=": (lambda a, b: a <= b, "<="),
        "==": (lambda a, b: a == b, "=="),
        ">=": (lambda a, b: a >= b, ">="),
        ">": (lambda a, b: a > b, ">"),
    }

    if operator not in operators:
        raise ValueError(f"Invalid operator: {operator}")

    compare_fn, op_str = operators[operator]

    if not compare_fn(size, expected_size):
        prefix = f"{msg}: " if msg else ""
        raise AssertionError(
            f"{prefix}Size {size} not {op_str} {expected_size}"
        )


def assert_image_format(
    response: Union[bytes, Response],
    expected_format: str,
    msg: str = "",
) -> None:
    """Assert that a response body carries an image of the expected format.

    This checks the actual bytes, not the Content-Type header, so it stays
    honest even when the server's mime map is wrong or missing.

    Args:
        response: Response (or raw bytes) whose body should be an image
        expected_format: Expected format name, e.g. "avif", "webp", "jpeg"
        msg: Optional message for assertion failure

    Raises:
        AssertionError: If the body is not in the expected format
        ValueError: If expected_format is not a format we can detect
    """
    known_formats = ("avif", "webp", "jpeg", "png", "gif")
    expected = expected_format.lower()
    if expected not in known_formats:
        raise ValueError(
            f"Unknown expected_format: {expected_format!r} "
            f"(known: {', '.join(known_formats)})"
        )

    if isinstance(response, Response):
        body = response.body
        content_type = response.header("Content-Type") or "<absent>"
    else:
        body = response
        content_type = "<n/a: raw bytes>"

    actual = _detect_image_format(body)
    if actual == expected:
        return

    prefix = f"{msg}: " if msg else ""
    detected = actual if actual else "unrecognized"
    raise AssertionError(
        f"{prefix}Expected {expected} image, got {detected}. "
        f"First 16 bytes: {body[:16].hex(' ')} | "
        f"Content-Type: {content_type} | body length: {len(body)}"
    )
