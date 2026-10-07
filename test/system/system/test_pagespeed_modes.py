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

"""PageSpeed on / standby / unplugged / off.

Ported from: pagespeed/system/system_tests/pagespeed_on_off_unplugged_standby.sh
             pagespeed/apache/system_tests/unplugged.sh

pagespeed-<mode>.example.com: `ModPagespeed <mode>` + collapse_whitespace.
Queries try to turn on debug; collapse_whitespace shows as a line starting
with </table> on the example index.
"""

import pytest

from pagespeed_test_framework import (
    assert_contains,
    assert_http_status,
    assert_not_contains,
    require_status_ok,
)
from pagespeed_test_framework.client import Response

_INDEX = "/mod_pagespeed_example/"
_WITH_QUERY = "/mod_pagespeed_example/?PageSpeed=on&PageSpeedFilters=+debug"
_RESOURCE = "/mod_pagespeed_example/rewrite_javascript.js.pagespeed.jm.0.js"


def _wget_dump(response: Response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks grep this whole dump, headers included.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.headers.items()]
    return "\n".join(lines) + "\n\n" + response.text


def _page(client, path: str) -> str:
    """Bash: OUT=$($WGET_DUMP "$URL")"""
    return _wget_dump(require_status_ok(client.get(path), path))


def _assert_debug_on(out: str) -> None:
    assert_contains(out, r"(?m)^mod_pagespeed on$")


def _assert_debug_off(out: str) -> None:
    assert_not_contains(out, r"(?m)^mod_pagespeed on$")


def _assert_collapse_on(out: str) -> None:
    assert_contains(out, r"(?m)^</table>")


def _assert_collapse_off(out: str) -> None:
    assert_not_contains(out, r"(?m)^</table>")


def _assert_resource_served(client) -> None:
    """Bash: check $WGET_DUMP "$URL" -O /dev/null"""
    assert_http_status(client.get(_RESOURCE), 200)


def _assert_resource_refused(client) -> None:
    """Bash: check_not $WGET_DUMP "$URL" -O /dev/null"""
    response = client.get(_RESOURCE)
    assert response.status >= 400, (
        f"Expected {_RESOURCE} to be refused, got {response.status}"
    )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestPageSpeedOn:
    HOST = "pagespeed-on.example.com"

    def test_no_query_params(self, vhost_client):
        """start_test pagespeed on, no query params"""
        out = _page(vhost_client(self.HOST), _INDEX)
        _assert_collapse_on(out)
        _assert_debug_off(out)

    def test_with_query_params(self, vhost_client):
        """start_test pagespeed on, with query params"""
        out = _page(vhost_client(self.HOST), _WITH_QUERY)
        _assert_collapse_on(out)
        _assert_debug_on(out)

    def test_resource_url(self, vhost_client):
        """start_test pagespeed on, resource url"""
        _assert_resource_served(vhost_client(self.HOST))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestPageSpeedStandby:
    HOST = "pagespeed-standby.example.com"

    def test_no_query_params(self, vhost_client):
        """start_test pagespeed standby, no query params"""
        out = _page(vhost_client(self.HOST), _INDEX)
        _assert_collapse_off(out)
        _assert_debug_off(out)

    def test_with_query_params(self, vhost_client):
        """start_test pagespeed standby, with query params"""
        out = _page(vhost_client(self.HOST), _WITH_QUERY)
        _assert_collapse_on(out)
        _assert_debug_on(out)

    def test_resource_url(self, vhost_client):
        """start_test pagespeed standby, resource url"""
        _assert_resource_served(vhost_client(self.HOST))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestPageSpeedUnplugged:
    HOST = "pagespeed-unplugged.example.com"

    def test_no_query_params(self, vhost_client):
        """start_test pagespeed unplugged, no query params"""
        out = _page(vhost_client(self.HOST), _INDEX)
        _assert_collapse_off(out)
        _assert_debug_off(out)

    def test_with_query_params(self, vhost_client):
        """start_test pagespeed unplugged, with query params"""
        out = _page(vhost_client(self.HOST), _WITH_QUERY)
        _assert_collapse_off(out)
        _assert_debug_off(out)

    def test_resource_url(self, vhost_client):
        """start_test pagespeed unplugged, resource url"""
        _assert_resource_refused(vhost_client(self.HOST))


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestPageSpeedOff:
    HOST = "pagespeed-off.example.com"

    def test_no_query_params(self, vhost_client):
        """start_test pagespeed off, no query params"""
        out = _page(vhost_client(self.HOST), _INDEX)
        _assert_collapse_off(out)
        _assert_debug_off(out)

    # Bash: the else branch of `if [ "$SERVER_NAME" = "nginx" ]` -- on every
    # port but nginx, "off" behaves as standby (the two nginx_only tests below
    # carry ngx_pagespeed's off=unplugged expectation). A port difference, not
    # a lane-fixture gap, so the marker is not_nginx rather than requires_fixture.
    @pytest.mark.not_nginx
    def test_with_query_params_is_standby(self, vhost_client):
        """start_test pagespeed off, with query params (everywhere else off=standby)"""
        out = _page(vhost_client(self.HOST), _WITH_QUERY)
        _assert_collapse_on(out)
        _assert_debug_on(out)

    @pytest.mark.not_nginx
    def test_resource_url_standby_behavior(self, vhost_client):
        """start_test pagespeed off, resource url, expect standby behavior"""
        _assert_resource_served(vhost_client(self.HOST))

    # This class needs the secondary_vhosts fixture, which the nginx lane
    # does not provide yet, so these two are dormant until the nginx lane
    # gains named vhosts (tracked in the medium-value triage).
    @pytest.mark.nginx_only
    def test_with_query_params_is_unplugged_on_nginx(self, vhost_client):
        """start_test pagespeed off, with query params (ngx_pagespeed off=unplugged)"""
        out = _page(vhost_client(self.HOST), _WITH_QUERY)
        _assert_collapse_off(out)
        _assert_debug_off(out)

    @pytest.mark.nginx_only
    def test_resource_url_unplugged_behavior_on_nginx(self, vhost_client):
        """start_test pagespeed off, resource url, expect unplugged behavior"""
        _assert_resource_refused(vhost_client(self.HOST))


_FILTERED = "/mod_pagespeed_example/styles/A.yellow.css.pagespeed.cf.KM5K8SbHQL.css"


# The bash suite (unplugged.sh, Apache-only) required X-Mod-Pagespeed
# specifically; this port accepts either port's header name
# (X-Mod-Pagespeed / X-Page-Speed) because the property under test is
# "the module answered", not the header spelling.
def _module_header(response: Response) -> str:
    return response.header("X-Mod-Pagespeed") or response.header("X-Page-Speed")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestUnpluggedAndOffVhosts:

    def test_unplugged_and_off(self, vhost_client):
        """start_test PageSpeed Unplugged and Off (apache/system_tests/unplugged.sh)"""
        unplugged = vhost_client("mpsunplugged.example.com")
        off = vhost_client("mpsoff.example.com")

        # PageSpeed unplugged does not serve .pagespeed. resources.
        refused = unplugged.get(_FILTERED)
        assert refused.status >= 400, (
            f"unplugged must not serve {_FILTERED}, got {refused.status}")
        # PageSpeed off does serve .pagespeed. resources.
        assert_http_status(off.get(_FILTERED), 200)

        # PageSpeed unplugged doesn't rewrite HTML, even when asked via query.
        html = require_status_ok(unplugged.get(f"{_INDEX}?PageSpeed=on"),
                                 "unplugged index")
        assert not _module_header(html), (
            f"unplugged answered with a module header: {_module_header(html)!r}")
        # PageSpeed off does rewrite HTML if asked.
        html = require_status_ok(off.get(f"{_INDEX}?PageSpeed=on"), "off index")
        assert _module_header(html), "off + ?PageSpeed=on must answer with the module header"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
