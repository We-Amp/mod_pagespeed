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

"""A same-domain redirect keeps the PageSpeed query parameters.

Ported from: pagespeed/automatic/system_tests/redirect_with_ps_params.sh

redirect.example.com enables add_instrumentation and collapse_whitespace;
"?PageSpeedFilters=-add_instrumentation" must survive the redirect. The
test follows the one redirect itself, where wget followed it.
"""

from urllib.parse import urlsplit

import pytest

from pagespeed_test_framework import require_status_ok

PAGE = "/mod_pagespeed_test/forbidden.html"
OPTS = "?PageSpeedFilters=-add_instrumentation"
INSTRUMENTATION = "pagespeed.addInstrumentationInit"


def _assert_not_redirected(response, what):
    require_status_ok(response, what)
    assert response.status not in (301, 302), f"{what}: redirected ({response.status})"
    assert not response.header_values("Location"), f"{what}: Location {response.header_values('Location')}"


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestRedirectWithPageSpeedParams:
    """Bash: Redirecting to the same domain retains PageSpeed query parameters."""

    def test_same_domain_redirect_keeps_pagespeed_query_params(self, vhost_client):
        vhost = vhost_client("redirect.example.com")

        default = vhost.get(PAGE)
        _assert_not_redirected(default, "default fetch")
        assert INSTRUMENTATION in default.text, default.text[:300]
        assert "  " not in default.text, "whitespace was not collapsed"

        without = vhost.get(PAGE + OPTS)
        _assert_not_redirected(without, "fetch with -add_instrumentation")
        assert INSTRUMENTATION not in without.text, without.text[:300]
        assert "  " not in without.text, "whitespace was not collapsed"

        redirect = vhost.get("/redirect" + PAGE + OPTS)
        assert redirect.status in (301, 302), f"expected a redirect, got {redirect.status}"
        location = redirect.header("Location")
        assert "=-add_instrumentati" in location, f"Location {location!r} lost the options"
        parts = urlsplit(location)
        target = vhost.get(parts.path + (f"?{parts.query}" if parts.query else ""))
        require_status_ok(target, f"redirect target {location}")
        assert INSTRUMENTATION not in target.text, target.text[:300]
        assert "  " not in target.text, "whitespace was not collapsed"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
