# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Shared worker-readiness helpers for test harness fixtures.

Every harness that depends on optimization/notification behavior must
wait for the worker to actually be up before running any test. A worker
that is still starting manifests as vague timeouts ("expected WebP,
still PNG after 30s") that waste debugging time.

Two entry points, matching the two ways harnesses can reach the worker:

- ``wait_worker_http(url, ...)`` — for harnesses that expose
  ``PAGESPEED_API_PORT`` (E2E, realworld). Polls the HTTP ``/v1/health``
  endpoint until it answers 200 with ``ready`` true (when the field is
  present) and returns the parsed JSON.

- ``wait_worker_socket(exec_fn, ...)`` — for harnesses without an
  exposed HTTP port (stress, http-compliance, production-tests). Polls
  the text-protocol ``/shared/pagespeed.sock.health`` socket inside the
  worker container until its status word is ``OK`` or ``DEGRADED``.

Both raise ``TimeoutError`` when the worker never becomes ready.
"""

from __future__ import annotations

import json
import subprocess
import time
from collections.abc import Callable

import requests

DEFAULT_TIMEOUT = 30.0
DEFAULT_HEALTH_SOCKET = "/shared/pagespeed.sock.health"
READY_STATUS_WORDS = frozenset({"OK", "DEGRADED"})


def wait_worker_http(
    base_url: str,
    timeout: float = DEFAULT_TIMEOUT,
    poll_interval: float = 1.0,
) -> dict:
    """Poll ``<base_url>/v1/health`` until the worker reports ready.

    Ready means HTTP 200 and, when the JSON carries a ``ready`` field,
    that field is true. Returns the parsed health JSON on success and
    raises ``TimeoutError`` if the worker never gets there within
    ``timeout`` seconds.
    """
    deadline = time.time() + timeout
    last_detail = "no response from worker"
    while time.time() < deadline:
        try:
            r = requests.get(base_url + "/v1/health", timeout=2)
            if r.status_code == 200:
                data = r.json()
                if data.get("ready", True):
                    return data
                last_detail = f"ready=false (status={data.get('status')!r})"
            else:
                last_detail = f"HTTP {r.status_code}"
        except (requests.RequestException, ValueError) as exc:
            last_detail = f"{type(exc).__name__}: {exc}"
        time.sleep(poll_interval)
    raise TimeoutError(
        f"Worker not ready at {base_url}/v1/health "
        f"after {timeout}s (last detail: {last_detail})"
    )


def _parse_health_socket_response(response: str) -> dict[str, str]:
    """Parse the text ``HEALTH`` socket response into a tag dict.

    The worker returns a single line like::

        OK 2/16 notifs=0 variants=0 proactive=0 errors=0 \
          cache_entries=0 inflight=0

    The status word is exposed as ``status``; each ``key=value`` tag is
    exposed under its own key. Unrecognized tokens are ignored.
    """
    tokens = response.strip().split()
    parsed: dict[str, str] = {}
    if not tokens:
        return parsed
    parsed["status"] = tokens[0]
    for tok in tokens[1:]:
        if "=" in tok:
            k, _, v = tok.partition("=")
            parsed[k] = v
    return parsed


def wait_worker_socket(
    exec_fn: Callable[[str, list[str]], subprocess.CompletedProcess],
    timeout: float = DEFAULT_TIMEOUT,
    poll_interval: float = 1.0,
    socket_path: str = DEFAULT_HEALTH_SOCKET,
    container: str = "worker",
) -> dict[str, str]:
    """Poll the worker's text-protocol HEALTH socket via ``docker exec``.

    ``exec_fn(container, argv)`` must run ``argv`` inside the given
    compose service container and return a ``CompletedProcess`` with
    ``stdout`` populated (see each harness's ``docker_exec`` helper).

    Returns the parsed tag dict as soon as the status word is ``OK`` or
    ``DEGRADED``. Raises ``TimeoutError`` when the socket never produces
    a ready response in ``timeout`` seconds.
    """
    # Single-line script: avoids indent/quoting surprises when passed via
    # `docker exec` argv. The HEALTH reply is a single line <200 bytes sent
    # in one uv_write, so recv(4096) captures it in one go.
    script = (
        "import socket,sys;"
        "s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);"
        "s.settimeout(5);"
        f"s.connect({socket_path!r});"
        "sys.stdout.write(s.recv(4096).decode());"
        "s.close()"
    )
    deadline = time.time() + timeout
    last_parsed: dict[str, str] = {}
    last_failure = "no attempt recorded"
    while time.time() < deadline:
        result = exec_fn(container, ["python3", "-c", script])
        if result.returncode == 0 and result.stdout.strip():
            parsed = _parse_health_socket_response(result.stdout)
            last_parsed = parsed
            if parsed.get("status") in READY_STATUS_WORDS:
                return parsed
            # Any other status word (e.g. STARTING) — keep polling.
        else:
            # Record the most recent failure so the final TimeoutError names
            # the real cause (missing python3, socket not ready, ECONNREFUSED,
            # …) instead of only the empty parse result.
            stderr_tail = (result.stderr or "").strip()[-200:]
            last_failure = (
                f"rc={result.returncode} "
                f"stdout={result.stdout!r} stderr={stderr_tail!r}"
            )
        time.sleep(poll_interval)
    raise TimeoutError(
        f"Worker HEALTH socket at {socket_path} did not report OK/DEGRADED "
        f"after {timeout}s (last parsed: {json.dumps(last_parsed)}; "
        f"last_failure: {last_failure})"
    )
