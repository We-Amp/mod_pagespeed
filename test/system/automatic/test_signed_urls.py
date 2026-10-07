#!/usr/bin/env python3
# Copyright 2026 We-Amp B.V.
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

"""Signed resource URLs: enforcing, transition and unsigned hosts.

Ported from: pagespeed/automatic/system_tests/signed_urls.sh

Name-based vhosts on the secondary port (setup_apache_test.sh,
configure_fixture_vhosts; blocks from install/debug.conf.template):
  signed-urls.example.com               UrlSigningKey helloworld
  signed-urls-transition.example.com    same + AcceptInvalidSignatures true
  unsigned-urls-transition.example.com  no key, AcceptInvalidSignatures true
"""

import re
import urllib.parse

import pytest

from pagespeed_test_framework import PageSpeedClient, require_match

_PAGE = (
    "/mod_pagespeed_test/unauthorized/inline_css.html"
    "?PageSpeedFilters=rewrite_images,rewrite_css"
)
_COMBINED_CSS = ".yellow{background-color:#ff0}"
_CSS_HREF = r'href="([^"]*all_styles\.css\.pagespeed\.cf\.[^"]+\.css)"'
_BAD_SIGNATURE = "AAAAAAAAAA"
# The bash `sed 's/.\{14\}$//'`: a 10-char signature plus ".css".
_SIGNATURE_AND_EXTENSION = 14

SIGNED = "signed-urls.example.com"
TRANSITION = "signed-urls-transition.example.com"
UNSIGNED_TRANSITION = "unsigned-urls-transition.example.com"


def _rewritten_css_path(client: PageSpeedClient) -> str:
    """Path of the rewritten all_styles.css on the page `client` serves.

    Bash:
        http_proxy=$SECONDARY_HOSTNAME fetch_until -save "$URL" \\
            'fgrep -c all_styles.css.pagespeed.cf' 1
        URL="$(grep -Eo "$URL_REGEX" $FETCH_FILE)"
        check test -n "$URL"
    """
    page = client.fetch_until_count(_PAGE, r"all_styles\.css\.pagespeed\.cf", 1)
    href = require_match(_CSS_HREF, page, "rewritten all_styles.css URL").group(1)
    absolute = urllib.parse.urljoin(f"http://{client.host}{_PAGE}", href)
    return urllib.parse.urlsplit(absolute).path


def _signed_css_path(client: PageSpeedClient) -> str:
    path = _rewritten_css_path(client)
    require_match(
        r"\.cf\.[^./]{20}\.css$", path,
        "a signed .cf. URL (10-char hash followed by a 10-char signature)")
    return path


def _assert_served(client: PageSpeedClient, path: str) -> None:
    """Bash: fetch_until $URL "fgrep -c $COMBINED_CSS" 1"""
    client.fetch_until_contains(path, re.escape(_COMBINED_CSS))


def _assert_refused(client: PageSpeedClient, path: str) -> None:
    """Bash: check_not $WGET ...; check_from "$OUT" egrep -q "403 Forbidden|404 Not Found" """
    response = client.get(path)
    assert response.status in (403, 404), (
        f"Expected 403 or 404 for {path}, got {response.status}:\n"
        f"{response.text[:500]}"
    )


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestSignedUrls:
    """signed-urls.example.com enforces the signature."""

    def test_correct_signature_is_served(self, vhost_client):
        """start_test Signed Urls : Correct URL signature is passed"""
        client = vhost_client(SIGNED)
        _assert_served(client, _signed_css_path(client))

    def test_incorrect_signature_is_refused(self, vhost_client):
        """start_test Signed Urls : Incorrect URL signature is passed"""
        client = vhost_client(SIGNED)
        unsigned = _signed_css_path(client)[:-_SIGNATURE_AND_EXTENSION]
        _assert_refused(client, f"{unsigned}{_BAD_SIGNATURE}.css")

    def test_missing_signature_is_refused(self, vhost_client):
        """start_test Signed Urls : No signature is passed"""
        client = vhost_client(SIGNED)
        unsigned = _signed_css_path(client)[:-_SIGNATURE_AND_EXTENSION]
        _assert_refused(client, f"{unsigned}.css")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestSignedUrlsIgnoredSignatures:
    """signed-urls-transition.example.com signs but accepts invalid signatures."""

    def test_correct_signature_is_served(self, vhost_client):
        """start_test Signed Urls, ignored signature : Correct URL signature is passed"""
        client = vhost_client(TRANSITION)
        _assert_served(client, _signed_css_path(client))

    def test_incorrect_signature_is_served(self, vhost_client):
        """start_test Signed Urls, ignored signatures : Incorrect URL signature is passed"""
        client = vhost_client(TRANSITION)
        unsigned = _signed_css_path(client)[:-_SIGNATURE_AND_EXTENSION]
        _assert_served(client, f"{unsigned}{_BAD_SIGNATURE}.css")

    def test_missing_signature_is_served(self, vhost_client):
        """start_test Signed Urls, ignored signatures : No signature is passed"""
        client = vhost_client(TRANSITION)
        unsigned = _signed_css_path(client)[:-_SIGNATURE_AND_EXTENSION]
        _assert_served(client, f"{unsigned}.css")


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestUnsignedUrlsIgnoredSignatures:
    """unsigned-urls-transition.example.com does not sign, accepts any signature.

    The bash spliced this host's hash (UH8L-zY4b4) in by hand; the port
    reads it from the page this host serves.
    """

    def test_bad_signature_is_served(self, vhost_client):
        """start_test Unsigned Urls, ignored signature : URL with bad signature is passed"""
        client = vhost_client(UNSIGNED_TRANSITION)
        path = _rewritten_css_path(client)
        require_match(r"\.cf\.[^./]{10}\.css$", path,
                      "an unsigned .cf. URL (10-char hash)")
        _assert_served(client, f"{path[:-len('.css')]}{_BAD_SIGNATURE}.css")

    def test_no_signature_is_served(self, vhost_client):
        """start_test Unsigned Urls, ignored signatures : no signature is passed"""
        client = vhost_client(UNSIGNED_TRANSITION)
        path = _rewritten_css_path(client)
        require_match(r"\.cf\.[^./]{10}\.css$", path,
                      "an unsigned .cf. URL (10-char hash)")
        _assert_served(client, path)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
