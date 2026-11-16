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

"""Directives that steer the server's own fetches are refused in .htaccess.

ModPagespeedMapOriginDomain, ModPagespeedMapProxyDomain and
ModPagespeedCustomFetchHeader stay available in the configuration the
operator writes, but an .htaccess line with one of them makes Apache answer
500 for that directory, as for ModPagespeedLoadFromFile.  The same holds for
an ExperimentSpec with alternate_origin_domain=.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient

FIXTURE_DIRS = (
    "map_origin",
    "map_proxy",
    "custom_fetch_header",
    "experiment_alternate_origin",
)


@pytest.mark.apache_only
@pytest.mark.requires_fixture("debug_conf_dirs")
class TestHtaccessFetchDirectives:
    @pytest.mark.parametrize("fixture_dir", FIXTURE_DIRS)
    def test_fetch_directive_in_htaccess_is_refused(
        self, client: PageSpeedClient, test_root: str, fixture_dir: str
    ):
        url = f"{test_root}/htaccess/fetch_directives/{fixture_dir}/plain.txt"
        response = client.get(url)
        assert response.status == 500, (
            f"{url}: expected 500 for a fetch directive in .htaccess, got "
            f"{response.status}: {response.text[:200]!r}"
        )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
