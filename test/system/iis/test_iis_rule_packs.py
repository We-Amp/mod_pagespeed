#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""RulePackFile on IIS: accepted at site scope, refused in a match block.

RulePackFile and RulePackMode are server-scoped options. On IIS the site's
pagespeed.config sets them at its top level (site scope); a "host:" match
block applies per request and cannot, so the parser refuses the directive
there, as it does for the daemon options.

The lane (setup_iis_full.ps1, lane fixture "rule_packs") sets in the site's
pagespeed.config

    pagespeed EnableFilters rule_packs
    pagespeed RulePackFile  <web root>\\mod_pagespeed_test\\rule_packs\\pack.json
    host:rulepacks-refused\\.example\\.com
    pagespeed RulePackFile  <web root>\\mod_pagespeed_test\\rule_packs\\refused-pack.json

pack.json lists rulepacks.example.com; refused-pack.json lists only
rulepacks-refused.example.com. The first host is rewritten, so the
site-level setting took. The second host is not: had the match-block setting
been accepted, refused-pack.json would have inserted its canonical there.

Where IIS lets a pack be set, and the full set of cases the other ports run
against the same pack, is in system/test_rule_packs.py.
"""

import os
import re

import pytest

from pagespeed_test_framework import VhostClient, require_status_ok

PAGE = "/mod_pagespeed_test/rule_packs/missing.html"
CANONICAL = re.compile(r'<link rel="canonical" href="([^"]*)">')


@pytest.fixture
def iis_site(server_config):
    port = int(os.environ.get("PAGESPEED_RULE_PACKS_PORT", server_config.port))

    def _make(host: str) -> VhostClient:
        return VhostClient(host, proxy_host=server_config.host, proxy_port=port)

    return _make


@pytest.mark.iis_only
@pytest.mark.requires_fixture("rule_packs")
class TestRulePackScope:
    def test_the_pack_set_at_site_scope_is_used(self, iis_site):
        response = iis_site("rulepacks.example.com").get(PAGE)
        require_status_ok(response, "rulepacks.example.com " + PAGE)
        assert CANONICAL.findall(response.text) == [
            f"https://rulepacks.example.com{PAGE}"
        ], response.text

    def test_a_pack_file_in_a_match_block_is_refused(self, iis_site):
        response = iis_site("rulepacks-refused.example.com").get(PAGE)
        require_status_ok(response, "rulepacks-refused.example.com " + PAGE)
        assert "refused.example.com" not in response.text, (
            "the match-block RulePackFile was used: " + response.text
        )
        assert CANONICAL.findall(response.text) == [], response.text


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
