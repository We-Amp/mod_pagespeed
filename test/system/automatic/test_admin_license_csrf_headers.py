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

"""AdminLicenseHandler CSRF-header forwarding regression test.

The AdminLicenseHandler CSRF gate (pagespeed/system/admin_license_handler.cc)
requires Content-Type=application/json AND X-Requested-With=XMLHttpRequest on
every mutation endpoint (apply, activate, consent).

On IIS, the AsyncFetch handed to AdminLicenseHandler is the IisModuleBaseFetch
created in iis_http_module.cpp. Its request_headers() is populated by
IisModuleBaseFetch::PopulateRequestHeaders, which forwards a fixed allowlist
of headers from the native IHttpRequest. Until this fix, that allowlist did
not include Content-Type or X-Requested-With -- the CSRF gate therefore
rejected every license POST regardless of what the browser actually sent.

This test goes through the real IIS handler chain (PageSpeedClient -> HTTP
-> IIS -> PageSpeedModule -> IisModuleBaseFetch -> AdminSite::AdminPage ->
AdminLicenseHandler::HandleRequest). Lives in test/system/automatic/ so the
existing run_iis_tests.ps1 pytest invocation collects it.
"""

import pytest

from pagespeed_test_framework import PageSpeedClient, require_no_auth_gate


GLOBAL_ADMIN_PATH = "/pagespeed_global_admin"
LICENSE_CONSENT_PATH = f"{GLOBAL_ADMIN_PATH}/v1/license/consent"
CSRF_HEADERS = {
    "Content-Type": "application/json",
    "X-Requested-With": "XMLHttpRequest",
}
CSRF_REJECT_MESSAGE = "Missing or invalid CSRF headers"


class TestAdminLicenseCsrfHeaderForwarding:
    """Regression tests for IIS PopulateRequestHeaders forwarding the
    Content-Type and X-Requested-With headers AdminLicenseHandler needs.
    """

    @pytest.mark.iis_only
    def test_csrf_gate_passes_when_headers_present(
        self, client: PageSpeedClient
    ):
        """Mutation endpoint must accept correctly-formed CSRF headers.

        We do not assert success status -- trial provisioning may be
        unreachable in CI or fail validation on the synthetic email --
        only that the request gets past the CSRF gate.
        """
        response = client.post(
            LICENSE_CONSENT_PATH,
            data='{"email":"ci-regression@example.com"}',
            headers=CSRF_HEADERS,
        )

        # A 403 without the CSRF gate's own message means an upstream
        # auth gate rejected the request before it reached the license
        # handler. The lanes run the admin endpoints without auth, so
        # that is a plausible regression, not an environment condition
        #.
        require_no_auth_gate(
            response, "License consent endpoint",
            allow_marker=CSRF_REJECT_MESSAGE,
        )

        assert CSRF_REJECT_MESSAGE not in response.text, (
            "Regression: IIS PopulateRequestHeaders did not forward CSRF "
            "headers to AdminLicenseHandler. "
            f"Status={response.status} body={response.text!r}"
        )

    @pytest.mark.iis_only
    def test_csrf_gate_rejects_when_headers_missing(
        self, client: PageSpeedClient
    ):
        """Negative path: CSRF protection itself must still work.

        Without Content-Type=application/json AND X-Requested-With, the
        gate must return 403 with the specific error message. This
        ensures the header-forwarding addition didn't accidentally short-
        circuit CSRF enforcement.
        """
        response = client.post(
            LICENSE_CONSENT_PATH,
            data='{"email":"ci-regression@example.com"}',
            headers=None,
        )

        # A 403 without the CSRF gate's own message means an upstream
        # auth gate rejected the request before it reached the license
        # handler. The lanes run the admin endpoints without auth, so
        # that is a plausible regression, not an environment condition
        #. The gate's own rejection is the behavior under
        # test here and is asserted below.
        require_no_auth_gate(
            response, "License consent endpoint",
            allow_marker=CSRF_REJECT_MESSAGE,
        )

        assert response.status == 403, (
            f"CSRF gate must return 403 without headers, got {response.status} "
            f"body={response.text!r}"
        )
        assert CSRF_REJECT_MESSAGE in response.text, (
            f"Expected CSRF rejection message, got body={response.text!r}"
        )


if __name__ == "__main__":
    # Route through SystemExit: a bare pytest.main(...) only returns its
    # status, and a test main that drops it exits 0 on a red suite -- vacuously green, the gate cannot report failure.
    raise SystemExit(pytest.main([__file__, "-v"]))
