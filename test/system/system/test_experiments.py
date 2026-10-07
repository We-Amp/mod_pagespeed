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

"""Experiments: device-type matching, Ajax requests are left alone, and the
core experiment-assignment framework (cookies, force-enrollment, the beacon,
per-experiment rewriting and options).

Ported from: pagespeed/system/system_tests/experiment_device_types.sh,
ajax_overrides_experiments.sh, and install/apache_experiment_test.sh (+
apache_experiment_ga_test.sh, apache_experiment_no_ga_test.sh)

"Never rewritten" polls for 5 s and requires the timeout, where the bash's
fetch_until -expect_time_out polled for 1 s (5 s under valgrind).

The bash ran the shared apache_experiment_test.sh assertions twice, once
with ModPagespeedAnalyticsID set and once without; this port folds that
into a single run against the AnalyticsID (#EXPERIMENT_GA) vhost (see
TestExperimentFramework).
"""

import re

import pytest

from pagespeed_test_framework import assert_contains, assert_not_contains, require_status_ok
from pagespeed_test_framework.stats import count_matching_lines

DESKTOP_UA = ("Mozilla/5.0 (Windows; U; Windows NT 6.1; en-US) AppleWebKit/534.13 "
              "(KHTML, like Gecko) Chrome/18.0.597.19 Safari/534.13")
MOBILE_UA = ("Mozilla/5.0 (Linux; Android 4.1.4; Galaxy Nexus Build/IMM76B) "
             "AppleWebKit/535.19 (KHTML, like Gecko) Chrome/21.0.1025.133 Mobile Safari/535.19")
AJAX_PAGE = "/mod_pagespeed_test/ajax/ajax.html"
AJAX = {"X-Requested-With": "XmlHttpRequest"}
REWRITTEN = re.escape(".pagespeed.")

# install/apache_experiment_test.sh's fixtures.
EXPERIMENT_VHOST = "experiment.example.com"
EXTEND_CACHE = "/mod_pagespeed_example/extend_cache.html"
ARIS_OFF = "/mod_pagespeed_test/avoid_renaming_introspective_javascript__off.html"
IMG_A = "/mod_pagespeed_example/images/xPuzzle.jpg.pagespeed.a.ic.fakehash.jpg"
IMG_B = "/mod_pagespeed_example/images/xPuzzle.jpg.pagespeed.b.ic.fakehash.jpg"


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestExperimentDeviceTypes:
    """Bash: Mobile experiment does (not) match / Can force-enroll."""

    @pytest.mark.parametrize(
        "user_agent,query,cookie",
        [
            pytest.param(DESKTOP_UA, "", "PageSpeedExperiment=0;", id="desktop-not-enrolled"),
            pytest.param(MOBILE_UA, "", "PageSpeedExperiment=1;", id="mobile-enrolled"),
            pytest.param(DESKTOP_UA, "?PageSpeedEnrollExperiment=1", "PageSpeedExperiment=1;",
                         id="desktop-force-enrolled"),
        ],
    )
    def test_experiment_enrollment(self, vhost_client, user_agent, query, cookie):
        client = vhost_client("experiment.devicematch.example.com").with_user_agent(user_agent)
        response = client.get(f"/mod_pagespeed_example/extend_cache.html{query}")
        require_status_ok(response, "extend_cache.html")
        cookies = response.header_values("Set-Cookie")
        assert any(value.startswith(cookie) for value in cookies), cookies


def _never_rewritten(vhost):
    """fetch_until -expect_time_out ... 'fgrep -c \\.pagespeed\\.' 3 with the Ajax header."""
    with pytest.raises(TimeoutError):
        vhost.fetch_until(
            AJAX_PAGE,
            condition=lambda r: count_matching_lines(r.text, REWRITTEN) == 3,
            headers=AJAX,
            timeout=5.0,
        )
    last = vhost.get(AJAX_PAGE, headers=AJAX)
    require_status_ok(last, "ajax.html with the Ajax header")
    assert "<head" not in last.text, last.text[:300]
    assert ".pagespeed." not in last.text, last.text[:300]
    return last


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts")
class TestAjaxRequests:
    """Bash: Ajax overrides experiments / Experiments not injected on ajax.html
    with an Ajax header / Ajax disables any filters that add head."""

    def test_ajax_overrides_experiments(self, vhost_client):
        vhost = vhost_client("experiment.ajax.example.com")
        page = vhost.fetch_until(
            AJAX_PAGE,
            condition=lambda r: count_matching_lines(r.text, REWRITTEN) == 3,
            timeout=100.0,
        )
        assert "<head" in page.text and "Two spaces." in page.text, page.text[:300]
        last = _never_rewritten(vhost)
        assert "Two  spaces." in last.text, last.text[:300]

    def test_ajax_disables_filters_that_add_head(self, vhost_client):
        vhost = vhost_client("ajax.example.com")
        page = vhost.fetch_until(
            AJAX_PAGE,
            condition=lambda r: count_matching_lines(r.text, REWRITTEN) == 3,
            timeout=100.0,
        )
        assert "<head" in page.text, page.text[:300]
        _never_rewritten(vhost)


def _wget_dump(response) -> str:
    """Status line, headers and body, as `wget --save-headers` saves them.

    The bash checks (check_from/check_not_from on $WGET_DUMP output) grep
    this whole dump, headers included -- e.g. the Set-Cookie header for the
    "cookie is set" test.
    """
    lines = [f"HTTP/1.1 {response.status}"]
    lines += [f"{name}: {value}" for name, value in response.raw_headers]
    return "\n".join(lines) + "\n\n" + response.text


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "experiment_framework")
class TestExperimentFramework:
    """Bash: install/apache_experiment_test.sh (+ apache_experiment_ga_test.sh,
    apache_experiment_no_ga_test.sh).

    The bash ran the shared apache_experiment_test.sh assertions twice, once
    per vhost variant (with/without ModPagespeedAnalyticsID), sourced by
    apache_experiment_ga_test.sh and apache_experiment_no_ga_test.sh. The two
    variants differ only in whether AnalyticsID is set; every one of their own
    assertions checks that no analytics javascript is emitted either way, so
    this class runs the shared checks once against the experiment.example.com
    vhost (which carries an AnalyticsID, the #EXPERIMENT_GA config) and folds
    the two variants' identical "no analytics javascript" checks into
    test_no_analytics_javascript_for_any_group.
    """

    def test_pagespeed_experiment_cookie_is_set(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(EXTEND_CACHE)
        assert_contains(_wget_dump(response), r"PageSpeedExperiment=")

    def test_pagespeed_filters_query_param_disables_experiments(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(f"{EXTEND_CACHE}?PageSpeed=on&PageSpeedFilters=rewrite_css")
        require_status_ok(response, "extend_cache.html?PageSpeed=on&PageSpeedFilters=rewrite_css")
        assert response.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
            f"served by {response.header('X-Test-Vhost')!r}"
        )
        assert_not_contains(_wget_dump(response), r"PageSpeedExperiment=")

    def test_modpagespeed_filters_query_param_also_disables_experiments(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(
            f"{EXTEND_CACHE}?ModPagespeed=on&ModPagespeedFilters=rewrite_css"
        )
        require_status_ok(
            response, "extend_cache.html?ModPagespeed=on&ModPagespeedFilters=rewrite_css"
        )
        assert response.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
            f"served by {response.header('X-Test-Vhost')!r}"
        )
        assert_not_contains(_wget_dump(response), r"PageSpeedExperiment=")

    def test_experiment_assignment_can_be_forced(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(f"{EXTEND_CACHE}?PageSpeedEnrollExperiment=2")
        assert_contains(_wget_dump(response), r"PageSpeedExperiment=2")

    def test_experiment_assignment_can_be_forced_to_a_0_percent_experiment(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(f"{EXTEND_CACHE}?PageSpeedEnrollExperiment=3")
        assert_contains(_wget_dump(response), r"PageSpeedExperiment=3")

    def test_experiment_assignment_can_be_forced_even_if_already_assigned(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(
            f"{EXTEND_CACHE}?PageSpeedEnrollExperiment=2",
            headers={"Cookie": "PageSpeedExperiment=7"},
        )
        assert_contains(_wget_dump(response), r"PageSpeedExperiment=2")

    def test_already_assigned_user_is_not_reassigned(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(EXTEND_CACHE, headers={"Cookie": "PageSpeedExperiment=2"})
        require_status_ok(response, "extend_cache.html")
        assert response.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
            f"served by {response.header('X-Test-Vhost')!r}"
        )
        assert_not_contains(_wget_dump(response), r"PageSpeedExperiment=")

    def test_beacon_includes_the_experiment_id(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        host_re = re.escape(EXPERIMENT_VHOST)
        for cookie, exptid in (("7", "7"), ("2", "2")):
            response = vhost.get(EXTEND_CACHE, headers={"Cookie": f"PageSpeedExperiment={cookie}"})
            assert_contains(
                _wget_dump(response),
                r"pagespeed\.addInstrumentationInit\('/mod_pagespeed_beacon', "
                rf"'&exptid={exptid}', 'http://{host_re}(?::[0-9]+)?"
                rf"{re.escape(EXTEND_CACHE)}'\);",
            )

    def test_no_experiment_group_beacon_has_no_experiment_id(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        response = vhost.get(EXTEND_CACHE, headers={"Cookie": "PageSpeedExperiment=0"})
        require_status_ok(response, "extend_cache.html")
        assert response.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
            f"served by {response.header('X-Test-Vhost')!r}"
        )
        assert_not_contains(_wget_dump(response), r"mod_pagespeed_beacon.*exptid")

    def test_resource_urls_are_rewritten_to_include_experiment_indexes(self, vhost_client):
        # id=7 is index a and id=2 is index b -- the order they're defined in
        # the config file.
        vhost = vhost_client(EXPERIMENT_VHOST)
        vhost.fetch_until_count(
            EXTEND_CACHE, r"\.pagespeed\.a\.ic\.", 1,
            headers={"Cookie": "PageSpeedExperiment=7"},
        )
        # apache_experiment_test.sh:91-92: a fresh, independent fetch still
        # carries the index -- not just the one response that satisfied the
        # poll above.
        refetch_a = vhost.get(EXTEND_CACHE, headers={"Cookie": "PageSpeedExperiment=7"})
        assert_contains(_wget_dump(refetch_a), r"\.pagespeed\.a\.ic\.")
        vhost.fetch_until_count(
            EXTEND_CACHE, r"\.pagespeed\.b\.ic\.", 1,
            headers={"Cookie": "PageSpeedExperiment=2"},
        )
        # apache_experiment_test.sh:93-94: same re-check for cookie 2 / index b.
        refetch_b = vhost.get(EXTEND_CACHE, headers={"Cookie": "PageSpeedExperiment=2"})
        assert_contains(_wget_dump(refetch_b), r"\.pagespeed\.b\.ic\.")

    def test_options_are_respected(self, vhost_client):
        vhost = vhost_client(EXPERIMENT_VHOST)
        normal_src = r'src="normal\.js"'
        introspection_src = r'src="introspection\.js"'
        # id=2: AvoidRenamingIntrospectiveJavascript=on. First poll until
        # normal.js has been renamed, after which introspection.js would have
        # been renamed too if it were going to be.
        response = vhost.fetch_until(
            ARIS_OFF,
            condition=lambda r: count_matching_lines(r.text, normal_src) == 0,
            headers={"Cookie": "PageSpeedExperiment=2"},
            detail_fn=lambda r: f"normal.js src lines={count_matching_lines(r.text, normal_src)}",
        )
        assert count_matching_lines(response.text, introspection_src) == 1, (
            response.text[:500]
        )

        # id=7: AvoidRenamingIntrospectiveJavascript=off. Repeat, expecting
        # introspection.js to get renamed too.
        vhost.fetch_until(
            ARIS_OFF,
            condition=lambda r: count_matching_lines(r.text, introspection_src) == 0,
            headers={"Cookie": "PageSpeedExperiment=7"},
        )

    def test_images_are_different_when_the_url_specifies_different_experiments(
        self, vhost_client
    ):
        # Image B is smaller because id=2 (side B) keeps convert_jpeg_to_progressive
        # enabled while id=7 (side A) disables it.
        vhost = vhost_client(EXPERIMENT_VHOST)
        ceiling_a = int(102902 * 1.05)
        ceiling_b = int(98276 * 1.05)
        response_a = vhost.fetch_until(
            IMG_A,
            condition=lambda r: len(r.body) <= ceiling_a,
            detail_fn=lambda r: f"len={len(r.body)} ceiling={ceiling_a}",
        )
        require_status_ok(response_a, "xPuzzle.jpg.pagespeed.a.ic.fakehash.jpg")
        assert response_a.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
            f"served by {response_a.header('X-Test-Vhost')!r}"
        )
        response_b = vhost.fetch_until(
            IMG_B,
            condition=lambda r: len(r.body) <= ceiling_b,
            detail_fn=lambda r: f"len={len(r.body)} ceiling={ceiling_b}",
        )
        require_status_ok(response_b, "xPuzzle.jpg.pagespeed.b.ic.fakehash.jpg")
        assert response_b.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
            f"served by {response_b.header('X-Test-Vhost')!r}"
        )

    def test_no_analytics_javascript_for_any_group(self, vhost_client):
        """Folds apache_experiment_ga_test.sh:32 and
        apache_experiment_no_ga_test.sh:30 -- both check the same three
        cookies for the same absent 'Experiment:' string."""
        vhost = vhost_client(EXPERIMENT_VHOST)
        for cookie in ("2", "7", "0"):
            response = vhost.get(EXTEND_CACHE, headers={"Cookie": f"PageSpeedExperiment={cookie}"})
            require_status_ok(response, f"extend_cache.html cookie={cookie}")
            assert response.header("X-Test-Vhost") == EXPERIMENT_VHOST, (
                f"served by {response.header('X-Test-Vhost')!r} cookie={cookie}"
            )
            assert_not_contains(
                _wget_dump(response), r"Experiment:", msg=f"cookie={cookie}"
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
