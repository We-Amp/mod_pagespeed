#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""In-place optimization on Apache serves and records a resource only under
the URL Apache itself read for the request.

Apache matches its <Location> and <Directory> sections and applies its access
rules on its own normalized reading of the request line; the module keys the
in-place cache on the URL it parsed from that line.  Spellings both read as
the same path are optimized in place as before.  A spelling Apache reads as a
different path from the module gets Apache's own answer for the path Apache
read, without an in-place lookup or recording under the path the module
reads.

Fixture (setup_apache_test.sh, debug_conf_dirs): ipro/reading/ is cacheable
for 200 seconds; ipro/reading/members/ requires the X-Reading-Pass header;
next to the open/ and members/ directories, the files open\\site.css and
members\\site.css (a backslash in their names) hold another stylesheet.
"""

import http.client
import random

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_http_status,
    assert_stat_delta,
)
from pagespeed_test_framework.stats import settled_stats

READING = "/mod_pagespeed_test/ipro/reading"
OPEN = f"{READING}/open/site.css"
MEMBERS = f"{READING}/members/site.css"
# Apache reads these spellings as the files with a backslash in their names;
# the module reads the stylesheets in open/ and members/.
OPEN_SPELLED = f"{READING}/open\\site.css"
MEMBERS_SPELLED = f"{READING}/members\\site.css"
PASS = {"X-Reading-Pass": "granted"}

OPEN_MARKER = b"reading-open"
OTHER_MARKER = b"reading-other"
# The stylesheets as Apache serves them; optimized in place they are shorter.
OPEN_SIZE = 40
OTHER_SIZE = 41

_IN_PLACE_COUNTERS = [
    "ipro_served",
    "ipro_not_in_cache",
    "ipro_not_rewritable",
    "ipro_recorder_resources",
]


def _token() -> int:
    return random.randint(1, 1000000)


def _optimize_open(client: PageSpeedClient, token: int, path: str = OPEN):
    """Polls until the open stylesheet is served optimized in place."""
    response = client.fetch_until(
        f"{path}?r={token}",
        condition=lambda r: (r.status == 200 and OPEN_MARKER in r.body
                             and len(r.body) < OPEN_SIZE),
        timeout=60.0,
    )
    assert_http_status(response, 200)
    assert OPEN_MARKER in response.body
    assert OTHER_MARKER not in response.body
    assert len(response.body) < OPEN_SIZE
    return response


def _assert_apaches_own_answer(response) -> None:
    """The spelled request gets the file Apache read, as Apache serves it."""
    assert_http_status(response, 200)
    assert response.body.startswith(b"." + OTHER_MARKER), response.body
    assert len(response.body) == OTHER_SIZE, response.body


def _assert_no_in_place_activity(snapshot, send) -> None:
    before = settled_stats(snapshot, _IN_PLACE_COUNTERS)
    send()
    after = settled_stats(snapshot, _IN_PLACE_COUNTERS)
    for name in _IN_PLACE_COUNTERS:
        assert_stat_delta(before, after, name, 0)


@pytest.mark.apache_only
@pytest.mark.requires_module
@pytest.mark.requires_stats
@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestIproRequestReading:

    def test_access_rule_applies(self, secondary_client: PageSpeedClient):
        """The members stylesheet is answered only with the header."""
        token = _token()
        assert_http_status(secondary_client.get(f"{MEMBERS}?r={token}"), 403)
        assert_http_status(
            secondary_client.get(f"{MEMBERS}?r={token}", headers=PASS), 200)

    def test_spellings_read_alike_are_optimized_in_place(
        self, secondary_client: PageSpeedClient
    ):
        """The plain path, an escaped letter and a doubled slash name the same
        file to Apache and to the module; each is optimized in place."""
        for path in (
            OPEN,
            f"{READING}/open/%73ite.css",
            f"{READING}/open//site.css",
        ):
            _optimize_open(secondary_client, _token(), path)

    def test_spelling_read_differently_is_not_served_another_entry(
        self, secondary_client: PageSpeedClient, secondary_stats_snapshot
    ):
        """Once the open stylesheet is optimized in place, a spelling Apache
        reads as another file gets that file, unoptimized, without an in-place
        lookup."""
        token = _token()
        _optimize_open(secondary_client, token)
        _assert_no_in_place_activity(
            secondary_stats_snapshot,
            lambda: _assert_apaches_own_answer(
                secondary_client.get(f"{OPEN_SPELLED}?r={token}")))

    def test_spelling_read_differently_records_nothing(
        self, secondary_client: PageSpeedClient, secondary_stats_snapshot
    ):
        """A spelling Apache reads as another file is neither looked up nor
        recorded: the open stylesheet, asked for afterwards under the same
        query, is optimized from its own content."""
        token = _token()

        def send():
            for _ in range(3):
                _assert_apaches_own_answer(
                    secondary_client.get(f"{OPEN_SPELLED}?r={token}"))

        _assert_no_in_place_activity(secondary_stats_snapshot, send)
        _optimize_open(secondary_client, token)

    def test_request_line_naming_another_scheme_is_left_to_apache(
        self, secondary_client: PageSpeedClient, secondary_stats_snapshot
    ):
        """A request line naming https on the plain-http port is routed by
        Apache on its own scheme and the Host header; it gets Apache's answer
        without an in-place lookup or recording under the https URL."""
        token = _token()
        target = (f"https://{secondary_client.host}:{secondary_client.port}"
                  f"{OPEN}?r={token}")

        def send():
            for _ in range(3):
                conn = http.client.HTTPConnection(
                    secondary_client.proxy_host, secondary_client.proxy_port,
                    timeout=30)
                try:
                    conn.putrequest("GET", target, skip_host=True,
                                    skip_accept_encoding=True)
                    conn.putheader(
                        "Host",
                        f"{secondary_client.host}:{secondary_client.port}")
                    conn.endheaders()
                    response = conn.getresponse()
                    body = response.read()
                finally:
                    conn.close()
                assert response.status == 200, (response.status, body)
                assert body.startswith(b"." + OPEN_MARKER), body
                assert len(body) == OPEN_SIZE, body

        _assert_no_in_place_activity(secondary_stats_snapshot, send)

    def test_spelling_beside_an_access_rule_is_left_to_apache(
        self, secondary_client: PageSpeedClient, secondary_stats_snapshot
    ):
        """With the members stylesheet recorded, a spelling Apache reads as a
        file outside the members location gets that file, without an
        in-place lookup under the members path."""
        token = _token()
        assert_http_status(
            secondary_client.get(f"{MEMBERS}?r={token}", headers=PASS), 200)
        _assert_no_in_place_activity(
            secondary_stats_snapshot,
            lambda: _assert_apaches_own_answer(
                secondary_client.get(f"{MEMBERS_SPELLED}?r={token}")))


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
