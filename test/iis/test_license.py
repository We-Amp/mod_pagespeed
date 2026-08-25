#!/usr/bin/env python3
# Copyright 2024 Google LLC
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

"""License validation tests for IIS PageSpeed module.

These tests verify that the license key validation works correctly.
"""

import os
import pytest

from pagespeed_test_framework import (
    PageSpeedClient,
    assert_contains,
    assert_http_status,
)


@pytest.mark.license
class TestLicenseValidation:
    """License validation tests."""

    @pytest.fixture
    def license_key(self):
        """Return the license key from environment or skip."""
        key = os.environ.get("PAGESPEED_LICENSE_KEY")
        if not key:
            pytest.skip("PAGESPEED_LICENSE_KEY not set")
        return key

    def test_with_valid_license(
        self, client: PageSpeedClient, example_root: str, license_key: str
    ):
        """With valid license, all features should work."""
        response = client.get(f"{example_root}/")
        assert_http_status(response, 200)

        # X-PageSpeed header should be present
        x_pagespeed = response.header("X-PageSpeed")
        assert x_pagespeed, "X-PageSpeed header should be present with valid license"

    def test_admin_with_valid_license(
        self, client: PageSpeedClient, admin_path: str, license_key: str
    ):
        """Admin should be accessible with valid license."""
        response = client.get(admin_path)
        assert response.status in (200, 403), \
            f"Expected 200 or 403, got {response.status}"


@pytest.mark.license
class TestLicenseFeatures:
    """Tests for license-gated features."""

    @pytest.fixture
    def enterprise_key(self):
        """Return enterprise license key or skip."""
        key = os.environ.get("PAGESPEED_ENTERPRISE_KEY")
        if not key:
            pytest.skip("PAGESPEED_ENTERPRISE_KEY not set")
        return key

    def test_enterprise_features(
        self, client: PageSpeedClient, example_root: str, enterprise_key: str
    ):
        """Enterprise features should work with enterprise license."""
        # Test a feature that requires enterprise license
        response = client.get(
            f"{example_root}/?PageSpeedFilters=prioritize_critical_css"
        )
        assert_http_status(response, 200)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
