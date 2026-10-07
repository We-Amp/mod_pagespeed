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

"""AvoidRenamingIntrospectiveJavascript keeps introspective scripts' URLs.

Ported from: pagespeed/system/system_tests/aris.sh and the aris cases of
pagespeed/system/system_tests/combine_javascript.sh

avoid_renaming_introspective_javascript__on/ turns the option on (its
.htaccess); the __off.html page has it off through a <Location> scoped to
that page, where the bash config turned it off server-wide. With the option on, a script
that inspects its own URL is neither inlined, cache-extended, rewritten
nor combined; with it off, it is.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient
from pagespeed_test_framework.stats import count_matching_lines

ON = "avoid_renaming_introspective_javascript__on/"
OFF = "avoid_renaming_introspective_javascript__off.html"

# (page, filters, poll regex, poll count, then regex, then count) --
# "fetch_until $URL '<poll regex>' <poll count>" followed by
# "check [ $(grep -c '<then regex>' $FETCH_FILE) = <then count> ]".
CASES = [
    pytest.param(ON, "inline_javascript", r"src=", 1, None, None, id="inline-on"),
    pytest.param(OFF, "inline_javascript", r"src=", 0, None, None, id="inline-off"),
    pytest.param(ON, "rewrite_javascript", r'src="../normal.js"', 0,
                 r'src="../introspection.js"', 1, id="cache-extend-on"),
    pytest.param(OFF, "rewrite_javascript", r'src="normal.js"', 0,
                 r'src="introspection.js"', 0, id="cache-extend-off"),
    pytest.param(ON, "testing,core", r'src="../normal.js"', 0,
                 r'src="../introspection.js"', 1, id="url-modification-on"),
    pytest.param(OFF, "testing,core", r'src="normal.js"', 0,
                 r'src="introspection.js"', 0, id="url-modification-off"),
    pytest.param(ON, "combine_javascript", r"src=", 2, None, None, id="combine-on"),
    pytest.param(OFF, "combine_javascript", r"src=", 1, None, None, id="combine-off"),
]


@pytest.mark.requires_fixture("debug_conf_dirs")
class TestAvoidRenamingIntrospectiveJavascript:
    """Bash: aris.sh:15-55, combine_javascript.sh:20-29."""

    @pytest.mark.parametrize("page,filters,poll,poll_count,then,then_count", CASES)
    def test_filter_on_introspective_javascript(
        self, client: PageSpeedClient, test_root: str,
        page, filters, poll, poll_count, then, then_count,
    ):
        url = f"{test_root}/{page}?PageSpeedFilters={filters}"
        response = client.fetch_until(
            url,
            condition=lambda r: count_matching_lines(r.text, poll) == poll_count,
            timeout=100.0,
            detail_fn=lambda r: (
                f"lines matching {poll!r}: {count_matching_lines(r.text, poll)} "
                f"expected={poll_count}"
            ),
        )
        if then is not None:
            assert count_matching_lines(response.text, then) == then_count, (
                f"{url}: lines matching {then!r} = "
                f"{count_matching_lines(response.text, then)}, expected {then_count}"
            )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
