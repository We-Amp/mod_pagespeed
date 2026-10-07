#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Self-test of the lane fixtures (test/system/setup_apache_test.sh).

Each class proves one lane fixture is live -- configuration the bash
suite's install/debug.conf.template carried and the ported tests rely on --
so a fixture regression fails here, by name, instead of as a confusing
failure inside a ported test. Every class is gated on its fixture name
(conftest.KNOWN_LANE_FIXTURES); lanes that do not provision it skip.
"""

import os
import pathlib
import socket
import time

import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    fetch_chunks,
    require_status_ok,
)


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestDebugConfDirs:
    """debug.conf.template <Directory>/<Location> test blocks are live."""

    def test_ipro_cache_control_directory(self, client: PageSpeedClient, test_root: str):
        # <Directory .../ipro/cc200p/> Header set Cache-control "private, max-age=200"
        response = client.get(f"{test_root}/ipro/cc200p/example.css?PageSpeed=off")
        require_status_ok(response, "ipro/cc200p/example.css")
        assert response.header("Cache-Control") == "private, max-age=200"

    def test_ipro_cache_control_value_is_copied_verbatim(
        self, client: PageSpeedClient, test_root: str
    ):
        # <Directory .../ipro/cc200sma50cc9nsp/>
        #   Header set Cache-control "max-age=200,s-maxage=50,max-age=9"
        response = client.get(f"{test_root}/ipro/cc200sma50cc9nsp/example.css?PageSpeed=off")
        require_status_ok(response, "ipro/cc200sma50cc9nsp/example.css")
        assert response.header("Cache-Control") == "max-age=200,s-maxage=50,max-age=9"

    def test_no_transform_directory(self, client: PageSpeedClient, test_root: str):
        # <Directory .../no_transform> Header append 'Cache-Control' 'no-transform'
        response = client.get(f"{test_root}/no_transform/BikeCrashIcn.png?PageSpeed=off")
        require_status_ok(response, "no_transform/BikeCrashIcn.png")
        assert "no-transform" in response.header("Cache-Control")

    def test_auth_directory_requires_basic_auth(self, client: PageSpeedClient, test_root: str):
        url = f"{test_root}/auth/medium_purple.css"
        assert client.get(url).status == 401
        authorized = client.get(url, headers={"Authorization": "Basic dXNlcjE6cGFzc3dvcmQ="})
        require_status_ok(authorized, "auth/medium_purple.css with user1's credentials")

    def test_add_resource_header_location(self, client: PageSpeedClient, example_root: str):
        # <Location ~ "\.pagespeed\.(...)"> ModPagespeedAddResourceHeader "X-Foo" "Bar"
        response = client.get(f"{example_root}/images/Puzzle.jpg.pagespeed.ce.0123456789.jpg")
        require_status_ok(response, "cache-extended Puzzle.jpg")
        assert response.header("X-Foo") == "Bar"


@pytest.mark.requires_fixture("admin_handlers")
class TestAdminHandlers:
    """Extra handler paths, and the MessagesDomains list does not lock anyone out."""

    def test_alternate_admin_path(self, client: PageSpeedClient):
        response = client.get("/alt/admin/path/statistics")
        require_status_ok(response, "/alt/admin/path/statistics")
        assert "application/json" in response.header("Content-Type")

    def test_console_handler(self, client: PageSpeedClient):
        response = client.get("/pagespeed_console")
        require_status_ok(response, "/pagespeed_console")
        assert "text/html" in response.header("Content-Type")

    def test_global_statistics_handler(self, client: PageSpeedClient):
        require_status_ok(
            client.get("/mod_pagespeed_global_statistics"), "/mod_pagespeed_global_statistics"
        )

    def test_messages_handler_still_served_on_primary(self, client: PageSpeedClient):
        require_status_ok(client.get("/mod_pagespeed_message"), "primary /mod_pagespeed_message")

    @pytest.mark.requires_https
    def test_messages_handler_still_served_on_https(self, https_client: PageSpeedClient):
        require_status_ok(
            https_client.get("/mod_pagespeed_message"), "HTTPS /mod_pagespeed_message"
        )

    @pytest.mark.requires_secondary
    def test_messages_handler_still_served_on_default_secondary(self, secondary_client):
        require_status_ok(
            secondary_client.get("/mod_pagespeed_message"),
            "default secondary vhost /mod_pagespeed_message",
        )


@pytest.mark.requires_fixture("stats_log")
class TestStatsLogFixture:
    """The statistics log of this run exists and receives dumps."""

    def test_statistics_log_is_written_during_this_run(self, client: PageSpeedClient):
        raw = os.environ.get("PAGESPEED_STATS_LOG", "")
        start_ms = os.environ.get("PAGESPEED_LANE_START_MS", "")
        assert raw, "stats_log is provided but PAGESPEED_STATS_LOG is unset"
        assert start_ms.isdigit(), f"PAGESPEED_LANE_START_MS={start_ms!r} is not a millisecond time"
        path = pathlib.Path(raw)
        deadline = time.monotonic() + 30.0
        while not (path.is_file() and "timestamp: " in path.read_text(errors="replace")):
            if time.monotonic() > deadline:
                listing = sorted(os.listdir(path.parent)) if path.parent.is_dir() else "missing"
                pytest.fail(f"no 'timestamp: ' dump in {path} after 30s; {path.parent}: {listing}")
            # The logger dumps from request processing; keep traffic flowing.
            client.get("/mod_pagespeed_example/index.html")
            time.sleep(0.5)


@pytest.mark.requires_fixture("cache_flush")
class TestCacheFlushFixture:
    """The runner can touch cache.flush in the primary cache root."""

    def test_cache_dir_is_writable_by_the_runner(self, server_config):
        assert server_config.cache_dir, "cache_flush is provided but PAGESPEED_CACHE_DIR is unset"
        assert os.access(server_config.cache_dir, os.W_OK), (
            f"{server_config.cache_dir} is not writable by uid {os.getuid()}"
        )


@pytest.mark.requires_fixture("doc_root_scratch")
class TestDocRootScratch:
    """purge/ and cache_flush/ are served from the document root and runner-owned."""

    @pytest.mark.parametrize(
        "tree,page",
        [("purge", "/purge/combine_css.html"), ("cache_flush", "/cache_flush/cache_flush_test.html")],
    )
    def test_tree_is_served_and_writable(self, client: PageSpeedClient, doc_root, tree, page):
        assert os.access(doc_root / tree, os.W_OK), f"{doc_root / tree} is not runner-writable"
        require_status_ok(client.get(page), page)


# ServerName of every vhost the secondary_vhosts fixture defines.
SECONDARY_VHOSTS = [
    "secondary.example.com",
    "forbidden.example.com",
    "unauthorizedresources.example.com",
    "pagespeed-off.example.com",
    "pagespeed-on.example.com",
    "pagespeed-unplugged.example.com",
    "pagespeed-standby.example.com",
    "url-attribute.example.com",
    # selfsigned.modpagespeed.com's vhost proxies to itself
    # (ProxyPass / http://selfsigned.modpagespeed.com/ in setup_apache_test.sh),
    # which only resolves locally through the external_origin fixture's
    # /etc/hosts pin -- without that pin, the proxy's own upstream lookup for
    # the name fails, so this one case needs both fixtures, while every other
    # vhost in this table is gated on secondary_vhosts alone.
    pytest.param(
        "selfsigned.modpagespeed.com",
        marks=pytest.mark.requires_fixture("external_origin"),
    ),
    "cdn.pm.example.com",
    "proxy.pm.example.com",
    "origin.pm.example.com",
    "options-by-cookies-enabled.example.com",
    "options-by-cookies-disabled.example.com",
    "request-option-override.example.com",
    "signed-urls.example.com",
    "signed-urls-transition.example.com",
    "unsigned-urls-transition.example.com",
    "compressedcache.example.com",
    "uncompressedcache.example.com",
    "ipro-for-browser.example.com",
    "ipro-for-browser-vary-on-auto.example.com",
    "ipro-for-browser-vary-on-none.example.com",
    "mpsunplugged.example.com",
    "mpsoff.example.com",
    "rproxy.rmcomments.example.com",
    "origin.rmcomments.example.com",
    "purge.example.com",
    "psoff-dir-on.example.com",
    "psoff-htaccess-on.example.com",
    "messages-allowed.example.com",
    "messages-still-not-allowed.example.com",
    "cleared-inherited.example.com",
    "cleared-inherited-unlisted.example.com",
    "nothing-allowed.example.com",
    "nothing-explicitly-allowed.example.com",
    "everything-explicitly-allowed.example.com",
    "debug-filters.example.com",
    "flush.example.com",
    "broken-fetch.example.com",
    "image-rewrite-with-flush.example.com",
    "respectvary.example.com",
    "redirect.example.com",
    "issue809.example.com",
    "content-encoding.example.com",
    "absolute-urls.example.com",
    "max-cacheable-content-length.example.com",
    "lff-large-files.example.com",
    "lff-large-files-no-fallback.example.com",
    "domain-hyperlinks-on.example.com",
    "domain-hyperlinks-off.example.com",
    "client-domain-rewrite.example.com",
    "map-static-domain.example.com",
    "downstreamcacherebeacon.example.com",
    "downstreamcacheresource.example.com",
    "experiment.devicematch.example.com",
    "experiment.ajax.example.com",
    "ajax.example.com",
    "embed-config-html.example.org",
    "embed-config-resources.example.com",
    "www.example.com",
    "origin.example.com",
    "cdn.example.com",
    "customhostheader.example.com",
    "optimizeforbandwidth.example.com",
    "uses-sendfile.example.com",
    "uses-xaccelredirect.example.com",
    "doesnt-sendfile.example.com",
    "shmcache.example.com",
    "ipro-proxy.example.com",
    "stats-local-only.example.com",
]

# (alias, ServerName) pairs handler_access_messages.sh addresses by alias.
SECONDARY_VHOST_ALIASES = [
    ("more-messages-allowed.example.com", "messages-allowed.example.com"),
    ("anything-b-wildcard.example.com", "messages-allowed.example.com"),
    ("but-this-message-allowed.example.com", "messages-still-not-allowed.example.com"),
    ("anything-c-wildcard.example.com", "cleared-inherited.example.com"),
    ("everything-explicitly-allowed-but-aliased.example.com",
     "everything-explicitly-allowed.example.com"),
]


def _vhost_values(response) -> list:
    return [v.strip() for v in response.header("X-Test-Vhost").split(",") if v.strip()]


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestSecondaryVhosts:
    """Every named vhost answers on the secondary port, by name and by alias."""

    @pytest.mark.parametrize("vhost", SECONDARY_VHOSTS)
    def test_vhost_is_routed_by_name(self, vhost_client, vhost):
        response = vhost_client(vhost).get("/")
        assert vhost in _vhost_values(response), (
            f"http://{vhost}/ was not served by the {vhost} VirtualHost "
            f"(status {response.status}, X-Test-Vhost={response.header('X-Test-Vhost')!r})"
        )

    @pytest.mark.parametrize("alias,server_name", SECONDARY_VHOST_ALIASES)
    def test_vhost_is_routed_by_alias(self, vhost_client, alias, server_name):
        response = vhost_client(alias).get("/")
        assert server_name in _vhost_values(response), (
            f"http://{alias}/ was not served by {server_name} "
            f"(X-Test-Vhost={response.header('X-Test-Vhost')!r})"
        )

    @pytest.mark.requires_secondary
    def test_unknown_host_still_reaches_the_default_ipro_vhost(self, secondary_client):
        # system/test_ipro.py relies on ipro.example.com being the default vhost
        # of the secondary port; the fixture vhosts must not displace it.
        # pagespeed-secondary.conf carries its own X-Test-Vhost header, so this
        # asserts the positive: an unmatched Host still lands on ipro.example.com,
        # not merely that it avoids one of the named fixture vhosts.
        response = secondary_client.get("/mod_pagespeed_example/index.html")
        require_status_ok(response, "secondary default vhost")
        assert response.header("X-Test-Vhost") == "ipro.example.com", (
            f"default vhost of the secondary port changed to "
            f"{response.header('X-Test-Vhost')!r}"
        )


@pytest.mark.requires_fixture("external_origin")
class TestExternalOrigin:
    """Local stand-ins for the internet origins the bash suite fetched."""

    @pytest.mark.parametrize(
        "name", ["selfsigned.modpagespeed.com", "www.gstatic.com", "www.modpagespeed.com"]
    )
    def test_origin_name_resolves_to_this_host(self, name):
        assert socket.gethostbyname(name) == "127.0.0.1"

    def test_stand_in_serves_do_not_modify_with_a_cookie(self, client: PageSpeedClient):
        response = client.get(
            "/do_not_modify/evil.html", headers={"Host": "selfsigned.modpagespeed.com"}
        )
        require_status_ok(response, "http://selfsigned.modpagespeed.com/do_not_modify/evil.html")
        assert response.header("X-Test-Vhost") == "selfsigned.modpagespeed.com"
        assert "test-cookie" in response.header("Set-Cookie")

    def test_stand_in_serves_the_gstatic_image(self, client: PageSpeedClient):
        response = client.get("/psa/static/1.gif", headers={"Host": "www.gstatic.com"})
        require_status_ok(response, "http://www.gstatic.com/psa/static/1.gif")
        assert response.header("Content-Type").startswith("image/gif")

    def test_https_stand_in_serves_the_css(self):
        # No lane fixture currently exports the certificate path to pytest
        # (setup_apache_test.sh keeps TLS_CERT_FILE to itself, and conftest.py
        # has no PAGESPEED_TLS_CERT_FILE), so this connects the same way every
        # other https_client use in this suite does: without certificate
        # verification. The module's own fetcher does verify the hostname and
        # certificate for a real request (generate_tls_certs), and
        # start_apache's own health check against this same stand-in already
        # covers that the certificate is valid for this host.
        stand_in = PageSpeedClient("www.modpagespeed.com", port=443, use_https=True)
        response = stand_in.get("/testfiles/google-cse-default.css")
        require_status_ok(
            response, "https://www.modpagespeed.com/testfiles/google-cse-default.css"
        )
        assert_contains(response, r"\.gsc-completion-selected")

    def test_primary_default_vhost_unchanged(self, client: PageSpeedClient, example_root: str):
        # Negative check, not a stand-in we chose not to fix: 000-default.conf
        # is the stock Debian default site, not a file this lane's setup
        # script writes, so it carries no X-Test-Vhost header to assert
        # positively. The external_origin and flush-origin site files are
        # written to sort after it (see configure_external_origin's comment
        # in setup_apache_test.sh), so this only proves they did not become
        # :80's default -- it can't name what the default vhost actually is.
        response = client.get(f"{example_root}/index.html")
        require_status_ok(response, "primary index.html")
        assert response.header("X-Test-Vhost") == "", (
            f"the primary's default :80 vhost changed to {response.header('X-Test-Vhost')!r}"
        )


@pytest.mark.requires_fixture("flush_origin", "secondary_vhosts")
class TestFlushOrigin:
    """The flushing origin is reachable through Apache and its flushes stream."""

    ROUTE = "/mod_pagespeed_test/flush_origin"

    def test_withflush_page_is_rewritten_and_keeps_its_header(
        self, client: PageSpeedClient
    ):
        response = client.get(f"{self.ROUTE}/withflush")
        require_status_ok(response, "flush_origin/withflush")
        assert response.header("X-My-PHP-Header") == "with_flush"
        assert (response.header("X-Mod-Pagespeed") or response.header("X-Page-Speed")), (
            "the proxied HTML did not pass through PageSpeed"
        )

    def test_slow_response_streams_through_flush_example_com(self, server_config):
        # check_flushing flush ... reads this through --proxy $SECONDARY_HOSTNAME
        response = fetch_chunks(
            server_config.secondary_host,
            server_config.secondary_port,
            f"http://flush.example.com{self.ROUTE}/slow_flushing_html_response",
            "flush.example.com",
            timeout=10.0,
        )
        assert response.status == 200 and response.chunked
        body = response.body.decode("utf-8", "replace")
        for i in range(1, 6):
            assert f"bar:{i}" in body
        # Streamed, not buffered: the first chunk arrives well before the
        # origin's ~5 s of pauses are over, the last one after them.
        assert response.chunks[0].elapsed < 2.2, (
            f"first chunk after {response.chunks[0].elapsed:.2f}s -- response was buffered"
        )
        assert response.chunks[-1].elapsed >= 4.0


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
