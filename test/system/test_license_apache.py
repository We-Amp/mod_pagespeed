# Copyright (c) 2024-2026 We-Amp B.V.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0

"""Apache mod_pagespeed license endpoint integration tests.

Targeted tests that verify the Apache-specific integration wiring for
the 5 admin license endpoints. These tests exercise code paths that
are only reachable through real Apache HTTP request handling:

  - POST body reading via apr_brigade (instaweb_handler.cc)
  - Handler routing through pagespeed_admin / pagespeed_global_admin
  - CSRF dual-header enforcement through real HTTP
  - ShouldOptimize() enforcement (licensed = optimization active)
  - License lifecycle end-to-end via real Apache

The underlying license handler logic is exhaustively tested by 67 C++
unit tests in admin_license_handler_test.cc. These integration tests
focus on the narrow but important gap: proving the Apache module
correctly wires everything together.

Prerequisites:
  - Apache running with mod_pagespeed
  - pagespeed_global_admin handler configured
  - Test content at /mod_pagespeed_example/
  - LICENSE_TOKEN env var set, or signing key at ~/.weamp/license-signing-key

Run:
  ./test/system/run_system_tests.sh -- test_license_apache.py -v
"""

import json
import os
import subprocess
import time

import pytest
import requests

# Admin endpoints — global admin required for mutations.
GLOBAL_ADMIN = "/pagespeed_global_admin"
LOCAL_ADMIN = "/pagespeed_admin"

# CSRF headers required by all mutation endpoints.
CSRF_HEADERS = {
    "Content-Type": "application/json",
    "X-Requested-With": "XMLHttpRequest",
}


@pytest.fixture(scope="module")
def base_url():
    """Base URL for Apache server."""
    host = os.environ.get("PAGESPEED_HOST", "localhost")
    port = os.environ.get("PAGESPEED_PORT", "80")
    return f"http://{host}:{port}"


@pytest.fixture(scope="module")
def session():
    """Shared HTTP session."""
    return requests.Session()


@pytest.fixture(scope="module")
def test_token():
    """Generate a valid test license token using the C++ tool.

    Requires generate_license_token to be built:
      bazel build //pagespeed/kernel/license_v2:generate_license_token

    The binary needs --key (signing key path) and --sub (subscriber id).
    Set PAGESPEED_SIGNING_KEY to override the default key location
    (~/.weamp/license-signing-key).
    """
    token = os.environ.get("LICENSE_TOKEN")
    if token:
        return token

    # Try to find the built binary and a signing key.
    for path in [
        "bazel-bin/pagespeed/kernel/license_v2/generate_license_token",
        "/src/bazel-bin/pagespeed/kernel/license_v2/generate_license_token",
    ]:
        if os.path.isfile(path):
            key_path = os.environ.get("PAGESPEED_SIGNING_KEY",
                os.path.expanduser("~/.weamp/license-signing-key"))
            if not os.path.isfile(key_path):
                pytest.skip("No LICENSE_TOKEN set and no signing key at " + key_path)
            result = subprocess.run(
                [path, "--key", key_path, "--sub", "test@system-test.local",
                 "--exp-duration", "3600"],
                capture_output=True, text=True)
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()

    pytest.skip("No test token available (set LICENSE_TOKEN or build generate_license_token)")


@pytest.fixture(scope="module")
def expired_token():
    """Generate an expired test token."""
    token = os.environ.get("LICENSE_TOKEN_EXPIRED")
    if token:
        return token

    for path in [
        "bazel-bin/pagespeed/kernel/license_v2/generate_license_token",
        "/src/bazel-bin/pagespeed/kernel/license_v2/generate_license_token",
    ]:
        if os.path.isfile(path):
            key_path = os.environ.get("PAGESPEED_SIGNING_KEY",
                os.path.expanduser("~/.weamp/license-signing-key"))
            if not os.path.isfile(key_path):
                pytest.skip("No LICENSE_TOKEN_EXPIRED set and no signing key at " + key_path)
            result = subprocess.run(
                [path, "--key", key_path, "--sub", "test@system-test.local",
                 "--exp-duration", "1"],  # expires 1 second after iat
                capture_output=True, text=True,
            )
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip()

    pytest.skip("No expired test token available")


# =============================================================================
# 1. Status endpoint — GET /pagespeed_global_admin/v1/license/status
# =============================================================================


@pytest.mark.license
class TestLicenseStatus:
    """GET /v1/license/status — works on both local and global admin."""

    def test_status_returns_json(self, base_url, session):
        """Status endpoint returns valid JSON with expected fields."""
        r = session.get(f"{base_url}{GLOBAL_ADMIN}/v1/license/status")
        assert r.status_code == 200
        data = r.json()
        assert "licensed" in data
        assert "is_global" in data

    def test_status_available_on_local_admin(self, base_url, session):
        """Status is also available on non-global admin path."""
        r = session.get(f"{base_url}{LOCAL_ADMIN}/v1/license/status")
        assert r.status_code == 200
        data = r.json()
        assert "licensed" in data
        assert data["is_global"] == False


# =============================================================================
# 2. Apply endpoint — POST /pagespeed_global_admin/v1/license/apply
# =============================================================================


@pytest.mark.license
class TestLicenseApply:
    """POST /v1/license/apply — direct license key application."""

    def test_apply_requires_csrf_headers(self, base_url, session, test_token):
        """Apply without CSRF headers returns 403."""
        r = session.post(
            f"{base_url}{GLOBAL_ADMIN}/v1/license/apply",
            data=json.dumps({"key": test_token}),
            headers={"Content-Type": "text/plain"},
        )
        assert r.status_code == 403

    def test_apply_blocked_on_local_admin(self, base_url, session, test_token):
        """Apply on local admin (non-global) returns 403."""
        r = session.post(
            f"{base_url}{LOCAL_ADMIN}/v1/license/apply",
            json={"key": test_token},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 403

    def test_apply_valid_token(self, base_url, session, test_token):
        """Apply a valid token and verify status changes to licensed."""
        r = session.post(
            f"{base_url}{GLOBAL_ADMIN}/v1/license/apply",
            json={"key": test_token},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 200
        data = r.json()
        assert data.get("success") is True

        # Verify status reflects the change.
        status = session.get(f"{base_url}{GLOBAL_ADMIN}/v1/license/status").json()
        assert status["licensed"] is True

    def test_apply_invalid_token_rejected(self, base_url, session):
        """Invalid token string is rejected (200 with success=false)."""
        r = session.post(
            f"{base_url}{GLOBAL_ADMIN}/v1/license/apply",
            json={"key": "not-a-valid-token"},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 200
        data = r.json()
        assert data.get("success") is not True, f"Expected rejection, got: {data}"

    def test_apply_tampered_token_rejected(self, base_url, session, test_token):
        """Token with flipped byte in signature is rejected."""
        import base64
        raw = base64.urlsafe_b64decode(test_token + "==")
        tampered = bytes([raw[0] ^ 0xFF]) + raw[1:]
        tampered_token = base64.urlsafe_b64encode(tampered).rstrip(b"=").decode()
        r = session.post(
            f"{base_url}{GLOBAL_ADMIN}/v1/license/apply",
            json={"key": tampered_token},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 200
        data = r.json()
        assert data.get("success") is not True, f"Expected rejection, got: {data}"

    def test_apply_expired_token_accepted_with_expiry(self, base_url, session, expired_token):
        """Expired token is accepted (valid signature) but reports past expiry.

        The token signature is valid so apply succeeds. Expiry is reported
        via expires_at in the past — enforcement is handled separately by
        the grace period and ShouldOptimize logic.
        """
        r = session.post(
            f"{base_url}{GLOBAL_ADMIN}/v1/license/apply",
            json={"key": expired_token},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 200
        data = r.json()
        assert data.get("success") is True
        # Token was generated with --exp-duration 1, so expires_at is ~1 second
        # after generation time — already in the past by the time we check.
        assert data.get("expires_at") is not None
        assert data["expires_at"] < time.time(), "expired token should have past expiry"


# =============================================================================
# 3. ShouldOptimize enforcement — the most important integration test
# =============================================================================


@pytest.mark.license
class TestShouldOptimize:
    """Verify licensed Apache produces optimization headers."""

    def test_licensed_response_has_pagespeed_header(self, base_url, session, test_token):
        """When licensed, X-Mod-Pagespeed header should be present."""
        # Ensure licensed.
        session.post(
            f"{base_url}{GLOBAL_ADMIN}/v1/license/apply",
            json={"key": test_token},
            headers=CSRF_HEADERS,
        )
        time.sleep(1)

        r = session.get(f"{base_url}/mod_pagespeed_example/")
        x_header = r.headers.get("X-Mod-Pagespeed") or r.headers.get("X-Page-Speed")
        assert x_header is not None, "Expected optimization header when licensed"


# =============================================================================
# 5. Activate endpoint — POST /pagespeed_global_admin/v1/license/activate
# =============================================================================


@pytest.mark.license
class TestLicenseActivate:
    """POST /v1/license/activate — proxy to license service."""

    def test_activate_blocked_on_local_admin(self, base_url, session):
        """Activate on local admin returns 403."""
        r = session.post(
            f"{base_url}{LOCAL_ADMIN}/v1/license/activate",
            json={"nonce": "00000000-0000-0000-0000-000000000000"},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 403


# =============================================================================
# 6. Consent endpoint — POST /pagespeed_global_admin/v1/license/consent
# =============================================================================


@pytest.mark.license
class TestLicenseConsent:
    """POST /v1/license/consent — proxy to license service."""

    def test_consent_blocked_on_local_admin(self, base_url, session):
        """Consent on local admin returns 403."""
        r = session.post(
            f"{base_url}{LOCAL_ADMIN}/v1/license/consent",
            json={"accepted": True},
            headers=CSRF_HEADERS,
        )
        assert r.status_code == 403


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
