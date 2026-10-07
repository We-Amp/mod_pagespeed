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

"""A virtual host inherits the server-wide options.

Ported from: pagespeed/apache/system_tests/vhost_inheritance.sh

The bash read the HTML configuration page of the secondary port's default
vhost; the admin now answers JSON, and on this lane the vhost that sets
nothing of its own is secondary.example.com.
"""

import json
import re

import pytest

from pagespeed_test_framework import require_status_ok


@pytest.mark.requires_secondary
@pytest.mark.requires_fixture("secondary_vhosts", "debug_conf_dirs")
class TestVhostInheritance:
    """Bash: vhost inheritance works."""

    def test_vhost_inherits_the_global_options(self, vhost_client, server_config):
        response = vhost_client("secondary.example.com").get(f"{server_config.admin_path}/config")
        require_status_ok(response, "secondary.example.com config")
        config = json.loads(response.text)["config"]
        assert isinstance(config, str) and config, f"empty config: {response.text[:200]}"
        assert "http://nonspdy.example.com/" in config, config
        assert re.search(r"\(blrw\)\s+psatest", config), config


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
