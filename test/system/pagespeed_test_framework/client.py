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
        self.timeout = timeout
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
        raise TimeoutError(
            f"Condition not met after {elapsed:.1f}s for {path}. Last: {status_info}"
        )

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

        return self.fetch_until(path, check_count, timeout=timeout, headers=headers)

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

        return self.fetch_until(path, check_contains, timeout=timeout, headers=headers)

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
            host=self.host,
            port=self.port,
            timeout=self.timeout,
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
