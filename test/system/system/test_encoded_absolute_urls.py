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

"""A .pagespeed. URL that encodes another host's absolute URL is refused.

Ported from: pagespeed/apache/system_tests/encoded_absolute_urls.sh

Resource reconstruction must never fetch an absolute URL for a host that
is not the request's own; the refusal is logged once. The log is read from
PAGESPEED_APACHE_ERROR_LOG by offset, where the bash tailed it.
"""

import os
import subprocess
import time

import pytest

REJECTED = b"Rejected absolute url reference"


def _log_path() -> str:
    return os.environ.get("PAGESPEED_APACHE_ERROR_LOG", "/var/log/apache2/error.log")


def _log_size() -> int:
    path = _log_path()
    try:
        return os.path.getsize(path)
    except OSError:
        out = subprocess.run(["sudo", "-n", "stat", "-c", "%s", path],
                             capture_output=True, text=True)
        if out.returncode != 0:
            pytest.fail(f"cannot stat {path}: {out.stderr.strip()}")
        return int(out.stdout.strip())


def _read_log_from(offset: int) -> bytes:
    path = _log_path()
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read()
    except OSError:
        out = subprocess.run(["sudo", "-n", "tail", "-c", f"+{offset + 1}", path],
                             capture_output=True)
        if out.returncode != 0:
            pytest.fail(f"cannot read {path}: {out.stderr!r}")
        return out.stdout


@pytest.mark.apache_only  # reads the Apache error log (bash: apache/, tail -F $APACHE_LOG)
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestEncodedAbsoluteUrls:
    """Bash: Encoded absolute urls are not respected."""

    def test_encoded_absolute_url_is_rejected(self, vhost_client):
        start = _log_size()
        response = vhost_client("absolute-urls.example.com").get(
            "/,hexample.com.pagespeed.jm.0.js"
        )
        assert response.status >= 400, (
            f"the encoded absolute URL was served: HTTP {response.status}"
        )
        deadline = time.monotonic() + 10.0
        rejections = _read_log_from(start).count(REJECTED)
        while rejections < 1 and time.monotonic() < deadline:
            time.sleep(0.1)
            rejections = _read_log_from(start).count(REJECTED)
        assert rejections == 1, f"'{REJECTED.decode()}' logged {rejections} times"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
