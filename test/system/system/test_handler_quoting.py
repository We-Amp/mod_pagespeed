#!/usr/bin/env python3
# Copyright (c) 2026 We-Amp B.V.
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

"""Request paths are never reflected raw by the module's handlers.

Ported from: pagespeed/apache/system_tests/handler_quoting.sh

The bash expected the 404 pages to echo an HTML-escaped path; Apache's
default error page no longer echoes the path, so the port asserts that
the raw path never appears and that any echo is escaped. The message
handler answers JSON with nosniff, which is its guard against reflected
markup, and (#1056) its JSON encoder now escapes '<', '>' and '&' as
defence in depth, so a logged message can never be mistaken for markup
even under another content type. The logged path reaches the message
history and the error log keeps the exact URL.

Deviation from the bash: asserts that the message handler's JSON body
carries the escaped form of the logged markup ("<evil>"), not
just that raw markup never appears -- the JSON+nosniff contract alone
used to be the only guard; now the encoder itself never emits unescaped
markup.
"""

import os
import subprocess

import pytest

from pagespeed_test_framework import PageSpeedClient

EVIL_404 = "404<evil>.js.pagespeed.jm.0.js"
EVIL_STATIC = "/pagespeed_static/<evil>.js"


def _read_error_log() -> str:
    path = os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        out = subprocess.run(["sudo", "-n", "cat", path], capture_output=True)
        if out.returncode != 0:
            pytest.fail(f"cannot read {path}: {out.stderr!r}")
        return out.stdout.decode("utf-8", "replace")


def _assert_not_reflected_raw(response, raw: str) -> None:
    assert raw not in response.text, f"raw {raw!r} reflected: {response.text[:400]}"
    assert "<evil>" not in response.text, f"raw markup reflected: {response.text[:400]}"
    assert "evil" not in response.text or "&lt;evil&gt;" in response.text, (
        f"the path was echoed without HTML escaping: {response.text[:400]}"
    )


def _assert_json_with_nosniff(response) -> None:
    assert response.header("Content-Type").lower().startswith("application/json"), (
        f"Content-Type {response.header('Content-Type')!r}"
    )
    assert response.header("X-Content-Type-Options").lower().startswith("nosniff"), (
        response.raw_headers
    )


@pytest.mark.apache_only  # only Apache echoes/logs these paths (bash comment; Apache error log)
class TestHandlerQuoting:
    """Bash: Proper quoting in our 404 handler / in mod_pagespeed_message /
    in the static-asset prefix."""

    def test_resource_404_never_reflects_the_raw_path(self, client: PageSpeedClient):
        response = client.get(f"/{EVIL_404}")
        assert response.status >= 400, f"HTTP {response.status}"
        _assert_not_reflected_raw(response, EVIL_404)

    def test_message_handler_answers_only_json(self, client: PageSpeedClient):
        client.get(f"/{EVIL_404}")
        messages = client.fetch_until(
            "/mod_pagespeed_message",
            condition=lambda r: "evil" in r.text and ".js.pagespeed.jm.0.js" in r.text,
            timeout=30.0,
            detail_fn=lambda r: f"status={r.status} tail={r.text[-300:]!r}",
        )
        _assert_json_with_nosniff(messages)
        # The JSON encoder escapes markup characters (#1056): the logged
        # message carries the escaped form, and the raw markup never
        # appears in the body even though it's a JSON response with
        # nosniff and would be safe either way.
        assert "<evil>" not in messages.text, (
            f"raw markup reflected in the JSON body: {messages.text[-300:]!r}"
        )
        assert "\\u003cevil\\u003e" in messages.text, (
            f"the logged path's markup was not escaped: {messages.text[-300:]!r}"
        )
        assert EVIL_404 in _read_error_log(), "the error log lost the exact request path"

    def test_static_handler_never_reflects_the_raw_path(self, client: PageSpeedClient):
        response = client.get(EVIL_STATIC)
        assert response.status >= 400, f"HTTP {response.status}"
        _assert_not_reflected_raw(response, EVIL_STATIC)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
