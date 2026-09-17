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

"""Assertions that FAIL with context where a skip would mask a regression.

A skip should assert something about the *environment*, never
about the *result*. When a test cannot find the artifact the rewriter was
supposed to produce, that absence is the failure -- and it should carry
the response body so the failure is diagnosable from CI output alone.
These helpers make the failing-with-context form cheaper to write than
the masking form::

    # Masking form (green run hiding a broken rewriter):
    match = re.search(r'src="([^"]*\\.pagespeed\\.ic[^"]*)"', response.text)
    if not match:
        pytest.skip("Could not find rewritten image URL")

    # Equivalent honest form (pass the Response so the failure names
    # the URL too):
    match = require_match(
        r'src="([^"]*\\.pagespeed\\.ic[^"]*)"', response,
        "rewritten image URL")

For artifacts that appear asynchronously, poll first with
``client.fetch_until(...)``/``fetch_until_contains`` using the extraction
pattern as the condition, then extract with ``require_match`` -- that way
slow-but-healthy lanes converge instead of going falsely red.
"""

import re
from typing import Optional, Union

import pytest

from pagespeed_test_framework.client import Response

# Failure messages attach the response body for post-hoc diagnosis; ~2KB
# shows whether the rewriter produced anything at all without flooding
# the CI log.
_MAX_BODY_CHARS = 2048


def _body_excerpt(text: str, limit: int = _MAX_BODY_CHARS) -> str:
    """Return text, truncated to limit chars with an explicit marker."""
    if len(text) <= limit:
        return text
    return f"{text[:limit]}\n... <truncated {len(text) - limit} chars>"


def _text_of(content: Union[str, bytes, Response]) -> str:
    if isinstance(content, Response):
        return content.text
    if isinstance(content, bytes):
        return content.decode("utf-8", errors="replace")
    return content


def require_match(
    pattern: str,
    content: Union[str, bytes, Response],
    what: str,
    flags: int = 0,
) -> re.Match:
    """Return the first regex match, or fail with the body attached.

    Use instead of ``if not match: pytest.skip(...)`` whenever the missing
    artifact is the product's job in every lane the test runs in: a
    rewrite that did not happen is a regression, not an environment
    condition.

    Pass the Response itself rather than response.text when you have it:
    the failure message then names the URL that lacked the artifact.

    Args:
        pattern: Regex pattern to find
        content: String, bytes, or Response to search
        what: Human name of the expected artifact, used in the failure
            message (e.g. "rewritten JPEG URL")
        flags: Regex flags (e.g., re.IGNORECASE)

    Returns:
        The re.Match object

    Raises:
        pytest.fail.Exception: If pattern is not found, with the URL
            (when content is a Response) and the (truncated) body attached
    """
    text = _text_of(content)
    match = re.search(pattern, text, flags)
    if match is None:
        url_note = ""
        if isinstance(content, Response) and content.url:
            url_note = f" for {content.url}"
        pytest.fail(
            f"Could not find {what} (pattern: {pattern}){url_note}.\n"
            f"Response body ({len(text)} chars):\n{_body_excerpt(text)}"
        )
    return match


def require_status_ok(response: Response, what: str = "request") -> Response:
    """Fail with status and body unless the response is HTTP 200.

    For endpoints that are unambiguously configured on the lane (admin
    paths, shipped test fixtures): a non-200 there is a regression signal,
    not a reason to skip.

    Args:
        response: Response object to check
        what: Human name of the endpoint, used in the failure message
            (e.g. "Admin statistics page")

    Returns:
        The response, for chaining

    Raises:
        pytest.fail.Exception: If status is not 200
    """
    if response.status != 200:
        pytest.fail(
            f"{what} returned HTTP {response.status} (expected 200) for "
            f"{response.url}.\n"
            f"Response body ({len(response.text)} chars):\n"
            f"{_body_excerpt(response.text)}"
        )
    return response


def require_no_auth_gate(
    response: Response,
    what: str = "Admin endpoint",
    allow_marker: Optional[str] = None,
) -> None:
    """Fail when the response is an auth rejection (401/403).

    The lanes run the admin endpoints without authentication, so an auth
    rejection from the admin handler is a plausible regression, not an
    environment condition.

    Args:
        response: Response object to check
        what: Human name of the endpoint, used in the failure message
        allow_marker: When the rejection body carries this marker, the
            401/403 is the behavior under test (e.g. the CSRF gate's own
            rejection message), not an upstream auth gate, and is allowed
            through

    Raises:
        pytest.fail.Exception: On 401/403 (without allow_marker in body)
    """
    if response.status in (401, 403):
        if allow_marker is not None and allow_marker in response.text:
            return
        pytest.fail(
            f"{what} rejected the request with HTTP {response.status} "
            f"(auth gate) for {response.url}. The test lanes run this "
            "endpoint without authentication; an auth rejection here is a "
            "plausible regression, not an environment condition.\n"
            f"Response body ({len(response.text)} chars):\n"
            f"{_body_excerpt(response.text)}"
        )
