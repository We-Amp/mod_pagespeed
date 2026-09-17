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

"""HTTP client for PageSpeed system tests.

This module provides a client for making HTTP requests to a PageSpeed-enabled
server. It mirrors the wget-based fetching from the bash system tests.
"""

import gzip
import http.client
import itertools
import os
import re
import ssl
import time
import urllib.parse
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple, Union

from pagespeed_test_framework.stats import parse_statistics


def _read_timeout_multiplier() -> float:
    raw = os.environ.get("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "1.0")
    try:
        value = float(raw)
    except ValueError:
        return 1.0
    return value if value > 0 else 1.0


# Scales fetch_until timeouts for slow environments (e.g. Windows AppVerifier
# + page heap, where PageSpeed's async work runs 5-50x slower).
_TIMEOUT_MULTIPLIER = _read_timeout_multiplier()


def _read_fetch_until_retries() -> int:
    """Number of EXTRA full-budget fetch_until attempts after a TimeoutError.

    Defaults to 0 (no retry -- the historical behavior), so Linux/Apache/nginx
    and unit-test runs are unaffected. The IIS CI path sets this to 1 to absorb
    the documented "PageSpeed init slow under heavy runner load" flake, where a
    rewrite that WOULD converge runs out of its first budget purely because the
    shared Windows box was busy. This is safe against masking real regressions:
    a condition that never becomes True still raises TimeoutError after the
    retries are exhausted (a non-converging rewrite cannot be "retried into"
    converging), and every retry is logged. It is strictly "a bit more budget,
    opt-in, loud", not "ignore failures". See the IIS init-load flake note.
    """
    raw = os.environ.get("PAGESPEED_TEST_FETCH_RETRIES", "0")
    try:
        value = int(raw)
    except ValueError:
        return 0
    return value if value >= 0 else 0


_FETCH_UNTIL_RETRIES = _read_fetch_until_retries()


# Sequences the per-failure evidence files so repeated timeouts in one run
# (e.g. the IIS retry budget) never overwrite each other.
_TIMEOUT_EVIDENCE_SEQ = itertools.count(1)

# Cap, in bytes, for the last-response-body excerpt embedded in the
# fetch_until TimeoutError message. 512 bytes covers a page's doctype and
# the start of its head -- enough to tell an unconverged rewrite from an
# error or empty body -- while keeping the pytest failure line readable.
# Longer bodies are cut and marked; the full body still goes to
# PAGESPEED_EVIDENCE_DIR when that is set (see _save_timeout_evidence).
_TIMEOUT_BODY_EXCERPT_BYTES = 512


def _format_last_body_excerpt(body: Optional[bytes]) -> str:
    """One-line excerpt of the last response body, for the TimeoutError.

    A convergence-window miss reports status and pattern, but triage still
    cannot tell a page that served fine yet never converged from a body that
    carried no HTML at all. Returns repr() of the first
    ``_TIMEOUT_BODY_EXCERPT_BYTES`` bytes (repr keeps the message on one line
    whatever the body contains), a "+N more bytes" marker when content was
    cut, a by-length summary when the body is not decodable text, and "" for
    an empty body. Must never raise: diagnostics cannot mask the timeout.
    """
    if not body:
        return ""
    excerpt = body[:_TIMEOUT_BODY_EXCERPT_BYTES]
    cut = len(body) > _TIMEOUT_BODY_EXCERPT_BYTES
    try:
        text = excerpt.decode("utf-8")
    except UnicodeDecodeError as err:
        # A byte-boundary slice can split a multi-byte character, which would
        # misreport a text body as non-text. Only a CUT excerpt whose first
        # bad byte sits within its last 3 bytes (a partial trailing
        # character: UTF-8 sequences are at most 4 bytes) is such an artifact
        # -- everything before err.start then decoded fine. Anything else
        # means the body genuinely is not text.
        if not cut or err.start < len(excerpt) - 3:
            return f"<non-text body, {len(body)} bytes>"
        try:
            text = excerpt[: err.start].decode("utf-8")
        except UnicodeDecodeError:
            return f"<non-text body, {len(body)} bytes>"
    # Bytes of the original body the repr actually represents (the trim above
    # may have dropped a partial character).
    shown = len(text.encode("utf-8"))
    note = repr(text)
    if len(body) > shown:
        note += f" (+{len(body) - shown} more bytes)"
    return note


# Default User-Agent matching the bash tests (Chrome 6).
# This matches the wget user_agent in system_test_helpers.sh.
DEFAULT_USER_AGENT = (
    "Mozilla/5.0 (X11; U; Linux x86_64; en-US) "
    "AppleWebKit/534.0 (KHTML, like Gecko) Chrome/6.0.408.1 Safari/534.0"
)

# WebP-capable User-Agent
WEBP_USER_AGENT = (
    "Mozilla/5.0 (Linux; Android 4.1.2; Nexus 7 Build/JZ054K) "
    "AppleWebKit/535.19 (KHTML, like Gecko) Chrome/80.0.1025.166 Safari/535.19"
)

# WebP with animated WebP support User-Agent (matches bash test webp-animated)
WEBP_ANIMATED_USER_AGENT = "webp-animated"


@dataclass
class Response:
    """HTTP response with parsed headers and body.

    Attributes:
        status: HTTP status code (e.g., 200, 404)
        headers: Dictionary of response headers (case-preserved keys)
        body: Raw response body as bytes
        url: The URL that was requested
    """

    status: int
    headers: Dict[str, str]
    body: bytes
    url: str = ""
    _header_lookup: Dict[str, str] = field(default_factory=dict, repr=False)

    def __post_init__(self):
        # Build case-insensitive header lookup
        self._header_lookup = {k.lower(): v for k, v in self.headers.items()}

    @property
    def text(self) -> str:
        """Return body decoded as UTF-8 text."""
        return self.body.decode("utf-8", errors="replace")

    def header(self, name: str, default: str = "") -> str:
        """Get header value by name (case-insensitive).

        Args:
            name: Header name to look up
            default: Value to return if header not found

        Returns:
            Header value or default
        """
        return self._header_lookup.get(name.lower(), default)

    @property
    def content_length(self) -> Optional[int]:
        """Return Content-Length header value as int, or None."""
        val = self.header("Content-Length")
        return int(val) if val else None

    @property
    def content_type(self) -> str:
        """Return Content-Type header value."""
        return self.header("Content-Type")

    @property
    def cache_control(self) -> str:
        """Return Cache-Control header value."""
        return self.header("Cache-Control")

    def is_ok(self) -> bool:
        """Return True if status is 2xx."""
        return 200 <= self.status < 300


class PageSpeedClient:
    """HTTP client for testing PageSpeed servers.

    This client provides methods for fetching URLs and waiting for
    asynchronous optimizations to complete (fetch_until pattern).

    Example:
        client = PageSpeedClient("localhost", 8080)
        response = client.get("/mod_pagespeed_example/combine_css.html")
        print(response.status, response.text)
    """

    def __init__(
        self,
        host: str,
        port: int = 80,
        timeout: float = 30.0,
        user_agent: str = DEFAULT_USER_AGENT,
        use_https: bool = False,
    ):
        """Initialize the client.

        Args:
            host: Server hostname
            port: Server port
            timeout: Connection timeout in seconds
            user_agent: User-Agent header value
            use_https: Use HTTPS instead of HTTP
        """
        self.host = host
        self.port = port
        # Scale by PAGESPEED_TEST_TIMEOUT_MULTIPLIER like fetch_until -- AppVerifier
        # runs are 3-50x slower. Keep the raw value so derived clients
        # (e.g. with_webp) re-scale exactly once instead of double-scaling.
        self._raw_timeout = timeout
        self.timeout = timeout * _TIMEOUT_MULTIPLIER
        self.user_agent = user_agent
        self.use_https = use_https

    def _make_connection(self) -> Union[http.client.HTTPConnection, http.client.HTTPSConnection]:
        """Create a new HTTP(S) connection."""
        if self.use_https:
            # Don't verify SSL for local testing
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            return http.client.HTTPSConnection(
                self.host, self.port, timeout=self.timeout, context=context
            )
        return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)

    def get(
        self,
        path: str,
        headers: Optional[Dict[str, str]] = None,
        allow_redirects: bool = False,
    ) -> Response:
        """Perform a GET request.

        Equivalent to: $WGET_DUMP $URL

        Args:
            path: URL path (e.g., "/mod_pagespeed_example/index.html")
            headers: Additional request headers
            allow_redirects: Follow redirects (default False)

        Returns:
            Response object with status, headers, and body
        """
        return self._request("GET", path, headers=headers, allow_redirects=allow_redirects)

    def post(
        self,
        path: str,
        data: Union[str, bytes, None] = None,
        headers: Optional[Dict[str, str]] = None,
    ) -> Response:
        """Perform a POST request.

        Equivalent to: $CURL -d "$DATA" "$URL"

        Args:
            path: URL path
            data: Request body
            headers: Additional request headers

        Returns:
            Response object
        """
        body = data.encode("utf-8") if isinstance(data, str) else data
        return self._request("POST", path, body=body, headers=headers)

    def purge(self, path: str, headers: Optional[Dict[str, str]] = None) -> Response:
        """Perform a PURGE request for cache invalidation.

        Equivalent to: $CURL --request PURGE "$URL"

        Args:
            path: URL path to purge
            headers: Additional request headers

        Returns:
            Response object
        """
        return self._request("PURGE", path, headers=headers)

    def get_gzip(
        self,
        path: str,
        headers: Optional[Dict[str, str]] = None,
    ) -> Response:
        """Perform a GET request with gzip decompression.

        Sends Accept-Encoding: gzip and decompresses the response.

        Args:
            path: URL path
            headers: Additional request headers

        Returns:
            Response with decompressed body
        """
        hdrs = dict(headers) if headers else {}
        hdrs["Accept-Encoding"] = "gzip"
        response = self.get(path, headers=hdrs)

        if response.header("Content-Encoding") == "gzip":
            try:
                response.body = gzip.decompress(response.body)
            except gzip.BadGzipFile:
                pass  # Not actually gzipped despite header

        return response

    def _request(
        self,
        method: str,
        path: str,
        body: Optional[bytes] = None,
        headers: Optional[Dict[str, str]] = None,
        allow_redirects: bool = False,
    ) -> Response:
        """Internal method to perform HTTP requests."""
        request_headers = {"User-Agent": self.user_agent}
        if headers:
            request_headers.update(headers)

        conn = self._make_connection()
        try:
            conn.request(method, path, body=body, headers=request_headers)
            resp = conn.getresponse()

            # Join duplicate headers with commas (per HTTP spec)
            raw_headers = resp.getheaders()
            header_dict = {}
            for name, value in raw_headers:
                if name in header_dict:
                    header_dict[name] = f"{header_dict[name]}, {value}"
                else:
                    header_dict[name] = value

            response = Response(
                status=resp.status,
                headers=header_dict,
                body=resp.read(),
                url=path,
            )

            # Handle redirects
            if allow_redirects and resp.status in (301, 302, 303, 307, 308):
                location = response.header("Location")
                if location:
                    # Handle relative URLs
                    if location.startswith("/"):
                        return self.get(location, headers=headers, allow_redirects=True)
                    # For absolute URLs, we'd need to parse and possibly change host

            return response
        finally:
            conn.close()

    def fetch_until(
        self,
        path: str,
        condition: Callable[[Response], bool],
        timeout: float = 100.0,
        interval: float = 0.5,
        headers: Optional[Dict[str, str]] = None,
        use_gzip: bool = False,
        detail_fn: Optional[Callable[[Response], str]] = None,
    ) -> Response:
        """Poll URL until condition is satisfied.

        This is the Python equivalent of the bash fetch_until function.
        It repeatedly fetches the URL and checks the condition until
        either the condition returns True or the timeout is reached.

        Equivalent to:
            fetch_until $URL 'grep -c pattern' expected_count

        When PAGESPEED_TEST_FETCH_RETRIES > 0, a budget exhaustion (TimeoutError)
        triggers up to that many additional full-budget attempts. This is OFF by
        default; the IIS CI path enables it (=1) to absorb the documented
        "PageSpeed init slow under load" flake. It cannot mask a real regression:
        a condition that never converges still raises TimeoutError after the
        retries are spent, and each retry is logged loudly. See
        ``_read_fetch_until_retries``.

        Args:
            path: URL path to fetch
            condition: Function that takes Response and returns True when done
            timeout: Maximum seconds to wait (default 100s like bash)
            interval: Seconds between requests (default 0.5s)
            headers: Optional request headers
            use_gzip: Use gzip-compressed fetching
            detail_fn: Optional callable mapping the last Response to a short
                description of how close the condition was (e.g. "matches=1
                expected=2"); included in the TimeoutError and the on-failure
                evidence so a timeout is diagnosable after the fact.

        Returns:
            Response that satisfied the condition

        Raises:
            TimeoutError: If condition not met within timeout (after any retries)
        """
        attempts = _FETCH_UNTIL_RETRIES + 1
        last_error: Optional[TimeoutError] = None
        for attempt in range(1, attempts + 1):
            try:
                return self._fetch_until_once(
                    path,
                    condition,
                    timeout=timeout,
                    interval=interval,
                    headers=headers,
                    use_gzip=use_gzip,
                    detail_fn=detail_fn,
                )
            except TimeoutError as err:
                last_error = err
                if attempt < attempts:
                    # Loud so triage can see the retry kicked in (and is not a
                    # silent "ignore failure"). The condition is GET-based and
                    # idempotent, so re-polling is safe.
                    print(
                        f"[fetch_until] timeout on attempt {attempt}/{attempts} "
                        f"for {path}; retrying full budget. ({err})",
                        flush=True,
                    )
        # All attempts exhausted -- surface the genuine timeout unchanged.
        assert last_error is not None
        raise last_error

    def _fetch_until_once(
        self,
        path: str,
        condition: Callable[[Response], bool],
        timeout: float = 100.0,
        interval: float = 0.5,
        headers: Optional[Dict[str, str]] = None,
        use_gzip: bool = False,
        detail_fn: Optional[Callable[[Response], str]] = None,
    ) -> Response:
        """Single budget-bounded poll of ``path`` until ``condition`` holds.

        Raises TimeoutError when the (multiplier-scaled) budget is exhausted.
        """
        timeout = timeout * _TIMEOUT_MULTIPLIER
        start = time.time()
        last_response: Optional[Response] = None
        fetch_fn = self.get_gzip if use_gzip else self.get

        while time.time() - start < timeout:
            try:
                last_response = fetch_fn(path, headers=headers)
                if condition(last_response):
                    return last_response
            except (http.client.HTTPException, OSError):
                # Connection errors - keep trying
                pass

            time.sleep(interval)

        elapsed = time.time() - start
        status_info = f"status={last_response.status}" if last_response else "no response"
        body_note = (
            _format_last_body_excerpt(last_response.body) if last_response else ""
        )
        detail = ""
        if detail_fn is not None and last_response is not None:
            try:
                detail = detail_fn(last_response)
            except Exception:
                # Diagnostics must never replace the genuine timeout.
                detail = ""
        evidence_note = self._save_timeout_evidence(
            path, last_response, status_info, detail, elapsed
        )
        raise TimeoutError(
            f"Condition not met after {elapsed:.1f}s for {path}. Last: {status_info}"
            + (f", {detail}" if detail else "")
            + (f", body={body_note}" if body_note else "")
            + evidence_note
        )

    def _save_timeout_evidence(
        self,
        path: str,
        last_response: Optional[Response],
        status_info: str,
        detail: str,
        elapsed: float,
    ) -> str:
        """Capture the failure window the instant a fetch_until times out.

        The server's message buffer rotates within ~a minute under AppVerifier
        churn, so the suite-end capture in run_iis_tests.ps1 arrives blind for
        the window that actually failed. Writes the last response
        body and a moment-of-failure message_history snapshot into
        PAGESPEED_EVIDENCE_DIR (per-iteration, uploaded by CI); no-op when the
        variable is unset. Returns a note for the TimeoutError message, or ""
        -- and never raises: evidence capture must not mask the timeout.
        """
        evidence_dir = os.environ.get("PAGESPEED_EVIDENCE_DIR", "")
        if not evidence_dir:
            return ""
        try:
            os.makedirs(evidence_dir, exist_ok=True)
            slug = re.sub(r"[^A-Za-z0-9._-]+", "-", path).strip("-.")[:80]
            prefix = f"fetch-until-timeout-{next(_TIMEOUT_EVIDENCE_SEQ):03d}-{slug}"

            if last_response is not None:
                with open(os.path.join(evidence_dir, f"{prefix}-body.html"), "wb") as f:
                    f.write(last_response.body)

            try:
                admin_path = os.environ.get("PAGESPEED_ADMIN_PATH", "/pagespeed_admin")
                snapshot = self.get(f"{admin_path}/message_history")
                with open(
                    os.path.join(evidence_dir, f"{prefix}-messages.json"), "wb"
                ) as f:
                    f.write(snapshot.body)
            except Exception:
                # The worker may be too wedged to serve admin pages; the body
                # and info files are still worth keeping.
                pass

            info_lines = [
                f"url: {path}",
                f"elapsed: {elapsed:.1f}s",
                f"last: {status_info}",
            ]
            if detail:
                info_lines.append(f"detail: {detail}")
            if last_response is not None:
                info_lines.append("headers:")
                info_lines.extend(
                    f"  {name}: {value}"
                    for name, value in last_response.headers.items()
                )
            with open(
                os.path.join(evidence_dir, f"{prefix}-info.txt"), "w",
                encoding="utf-8",
            ) as f:
                f.write("\n".join(info_lines) + "\n")

            return f" Evidence: {prefix}-* in {evidence_dir}"
        except Exception:
            return ""

    def fetch_until_count(
        self,
        path: str,
        pattern: str,
        expected_count: int,
        timeout: float = 100.0,
        headers: Optional[Dict[str, str]] = None,
        case_insensitive: bool = False,
    ) -> Response:
        """Poll URL until pattern appears expected number of times.

        Convenience wrapper for the common fetch_until pattern of
        counting regex matches.

        Equivalent to:
            fetch_until $URL 'grep -c pattern' count

        Args:
            path: URL path to fetch
            pattern: Regex pattern to search for
            expected_count: Number of matches required
            timeout: Maximum seconds to wait
            headers: Optional request headers
            case_insensitive: Use case-insensitive matching

        Returns:
            Response where pattern appears expected_count times

        Raises:
            TimeoutError: If count not reached within timeout
        """
        flags = re.IGNORECASE if case_insensitive else 0
        regex = re.compile(pattern, flags)

        def check_count(r: Response) -> bool:
            # Use == not >= because we're waiting for optimization to complete.
            # E.g., combine_css should reduce 4 CSS files to 1, so we wait
            # until count equals 1, not just until count >= 1 (which would
            # return immediately with the unoptimized 4 files).
            return len(regex.findall(r.text)) == expected_count

        def count_detail(r: Response) -> str:
            # matches=1 expected=2 (partial convergence) triages completely
            # differently from matches=0 (rewrite never started).
            return f"matches={len(regex.findall(r.text))} expected={expected_count}"

        return self.fetch_until(
            path, check_count, timeout=timeout, headers=headers,
            detail_fn=count_detail,
        )

    def fetch_until_contains(
        self,
        path: str,
        pattern: str,
        timeout: float = 100.0,
        headers: Optional[Dict[str, str]] = None,
    ) -> Response:
        """Poll URL until pattern is found in response.

        Equivalent to:
            fetch_until $URL 'fgrep -c pattern' 1

        Args:
            path: URL path to fetch
            pattern: String or regex pattern to find
            timeout: Maximum seconds to wait
            headers: Optional request headers

        Returns:
            Response containing the pattern

        Raises:
            TimeoutError: If pattern not found within timeout
        """
        regex = re.compile(pattern)

        def check_contains(r: Response) -> bool:
            return regex.search(r.text) is not None

        def contains_detail(r: Response) -> str:
            return f"pattern {pattern!r} not found"

        return self.fetch_until(
            path, check_contains, timeout=timeout, headers=headers,
            detail_fn=contains_detail,
        )

    def get_statistics(
        self, stats_path: str = "/mod_pagespeed_statistics",
        disable_pagespeed: bool = True
    ) -> Dict[str, int]:
        """Fetch and parse the statistics page.

        Equivalent to:
            $WGET_DUMP $STATISTICS_URL | scrape_pipe_stat

        Args:
            stats_path: Path to statistics endpoint
            disable_pagespeed: If True, add ?PageSpeed=off to prevent rewriting.
                This works for Apache but breaks nginx/Envoy where admin
                endpoints require PageSpeed to be enabled.

        Returns:
            Dictionary of statistic names to values
        """
        # Disable PageSpeed rewriting on the stats page itself
        # Note: This breaks nginx/Envoy admin endpoints which require PageSpeed
        # to be enabled. Those platforms should pass disable_pagespeed=False.
        if disable_pagespeed:
            if "?" in stats_path:
                stats_path += "&PageSpeed=off"
            else:
                stats_path += "?PageSpeed=off"

        response = self.get(stats_path)
        return parse_statistics(response.text)

    def with_webp(self) -> "PageSpeedClient":
        """Return a new client configured for WebP image requests.

        The returned client uses a WebP-capable User-Agent and sends
        the Accept: image/webp header.
        """
        return PageSpeedClient(
            # Pass the RAW (unscaled) timeout: the constructor applies
            # _TIMEOUT_MULTIPLIER itself, so handing it self.timeout (already
            # scaled) would double-scale. See PageSpeedClient.__init__.
            host=self.host,
            port=self.port,
            timeout=self._raw_timeout,
            user_agent=WEBP_USER_AGENT,
            use_https=self.use_https,
        )


class ProxiedPageSpeedClient(PageSpeedClient):
    """HTTP client that routes requests through a proxy.

    Used for testing configurations with SECONDARY_HOSTNAME.
    """

    def __init__(
        self,
        host: str,
        port: int,
        proxy_host: str,
        proxy_port: int,
        timeout: float = 30.0,
        user_agent: str = DEFAULT_USER_AGENT,
    ):
        """Initialize the proxied client.

        Args:
            host: Target server hostname (used in Host header)
            port: Target server port
            proxy_host: Proxy server hostname
            proxy_port: Proxy server port
            timeout: Connection timeout
            user_agent: User-Agent header
        """
        super().__init__(host, port, timeout, user_agent)
        self.proxy_host = proxy_host
        self.proxy_port = proxy_port

    def _make_connection(self) -> http.client.HTTPConnection:
        """Create connection to the proxy."""
        return http.client.HTTPConnection(
            self.proxy_host, self.proxy_port, timeout=self.timeout
        )

    def _request(
        self,
        method: str,
        path: str,
        body: Optional[bytes] = None,
        headers: Optional[Dict[str, str]] = None,
        allow_redirects: bool = False,
    ) -> Response:
        """Perform request through proxy."""
        request_headers = {"User-Agent": self.user_agent, "Host": f"{self.host}:{self.port}"}
        if headers:
            request_headers.update(headers)

        # For proxied requests, use full URL
        full_url = f"http://{self.host}:{self.port}{path}"

        conn = self._make_connection()
        try:
            conn.request(method, full_url, body=body, headers=request_headers)
            resp = conn.getresponse()

            # Join duplicate headers with commas (per HTTP spec)
            raw_headers = resp.getheaders()
            header_dict = {}
            for name, value in raw_headers:
                if name in header_dict:
                    header_dict[name] = f"{header_dict[name]}, {value}"
                else:
                    header_dict[name] = value

            return Response(
                status=resp.status,
                headers=header_dict,
                body=resp.read(),
                url=path,
            )
        finally:
            conn.close()
