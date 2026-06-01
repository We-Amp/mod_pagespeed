// Copyright (c) 2024-2026 We-Amp B.V.

// Integration tests for AdminLicenseHandler.
// Uses the testkey variant (zero-seed Ed25519 key) so tests can sign
// AND verify tokens without needing the production key.
//
// IMPORTANT: Token expiry uses the SYSTEM CLOCK (VerifyLicenseToken uses
// now_sec=0 by default), while rate limiting and renewal window checks use
// the MockTimer. Tests use far_future = iat + kMaxTokenLifetimeSec
// (1806451200) for non-expired tokens. Expired-token tests use an older iat
// (Jan 2024) with exp shortly after, satisfying exp >= iat while remaining
// in the past per the system clock.

#include "pagespeed/system/admin_license_handler.h"

#include <atomic>
#include <filesystem>
#include <random>
#include <memory>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/license_v2/license_file.h"
#include "pagespeed/kernel/license_v2/license_signer.h"
#include "pagespeed/kernel/license_v2/license_token.h"
#include "pagespeed/kernel/license_v2/license_verifier.h"
#include "pagespeed/kernel/license_v2/tracking_metadata.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"

namespace net_instaweb {

namespace {

// Test seed (32 bytes of zeros) — matches the PAGESPEED_TEST_LICENSE_KEY
// compiled into license_verifier_testkey.
const GoogleString kTestSeed(32, '\0');

// A minimal UrlAsyncFetcher that captures the URL and request body, and
// returns a configurable response.  Calls Done() synchronously so tests
// do not need a scheduler or event loop.
class CapturingUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  CapturingUrlAsyncFetcher() = default;

  void Fetch(const GoogleString& url, MessageHandler* handler,
             AsyncFetch* fetch) override {
    last_url_ = url;
    last_body_ = fetch->request_headers()->message_body();
    fetch_count_++;

    fetch->response_headers()->set_status_code(response_status_);
    fetch->response_headers()->Add(HttpAttributes::kContentType,
                                   kContentTypeJson.mime_type());
    fetch->Write(response_body_, handler);
    fetch->Done(simulate_success_);
  }

  void set_response_body(const GoogleString& body) { response_body_ = body; }
  void set_response_status(int status) { response_status_ = status; }
  void set_simulate_success(bool s) { simulate_success_ = s; }

  const GoogleString& last_url() const { return last_url_; }
  const GoogleString& last_body() const { return last_body_; }
  int fetch_count() const { return fetch_count_; }
  void reset_fetch_count() { fetch_count_ = 0; }

 private:
  GoogleString last_url_;
  GoogleString last_body_;
  GoogleString response_body_ = "{}";
  int response_status_ = 200;
  bool simulate_success_ = true;
  int fetch_count_ = 0;

  CapturingUrlAsyncFetcher(const CapturingUrlAsyncFetcher&) = delete;
  CapturingUrlAsyncFetcher& operator=(const CapturingUrlAsyncFetcher&) = delete;
};

// Generate a unique temp directory name using random suffix.
GoogleString MakeUniqueTempDir() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  return StrCat("/tmp/admin_license_test_", Integer64ToString(dist(gen)));
}

class AdminLicenseHandlerTest : public ::testing::Test {
 protected:
  AdminLicenseHandlerTest()
      : thread_system_(Platform::CreateThreadSystem()),
        mock_timer_(new NullMutex, MockTimer::kApr_5_2010_ms),
        handler_(new NullMutex),
        tmp_dir_(MakeUniqueTempDir()),
        license_handler_(&mock_timer_, thread_system_.get(), &handler_,
                         &fetcher_, tmp_dir_) {
    AdminLicenseHandler::SetLicenseServiceUrlForTesting(
        "http://test.example.com");
    // Generate the test keypair matching PAGESPEED_TEST_LICENSE_KEY.
    CreateKeypair(kTestSeed, &public_key_, &private_key_);
    // Create the temp directory for license file I/O.
    std::filesystem::create_directories(tmp_dir_);
  }

  ~AdminLicenseHandlerTest() override {
    AdminLicenseHandler::SetLicenseServiceUrlForTesting(
        "http://test.example.com");
    // Clean up only our unique temp directory.
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir_, ec);
    // Also remove license file written by LicenseFilePath (derived from
    // parent of cache_path).
    std::filesystem::path license_path = LicenseFilePath(tmp_dir_);
    std::filesystem::remove(license_path, ec);
  }

  StringAsyncFetch* NewFetch(GoogleString* buffer) {
    return new StringAsyncFetch(
        RequestContext::NewTestRequestContext(thread_system_.get()), buffer);
  }

  int DoRequest(StringPiece path, StringPiece request_body,
                bool is_global, GoogleString* out_body) {
    StringAsyncFetch* fetch = NewFetch(out_body);
    // Set valid CSRF headers for mutation endpoints.
    fetch->request_headers()->Add(HttpAttributes::kContentType,
                                  "application/json");
    fetch->request_headers()->Add(HttpAttributes::kXRequestedWith,
                                  "XMLHttpRequest");
    license_handler_.HandleRequest(path, request_body, is_global, fetch);
    int status = fetch->response_headers()->status_code();
    delete fetch;
    return status;
  }

  // Low-level helper with optional Content-Type and X-Requested-With headers.
  // Pass nullptr to omit a header entirely.
  int DoRequestWithHeaders(StringPiece path, StringPiece request_body,
                           bool is_global,
                           const char* content_type,
                           const char* x_requested_with,
                           GoogleString* out_body) {
    StringAsyncFetch* fetch = NewFetch(out_body);
    if (content_type) {
      fetch->request_headers()->Add(HttpAttributes::kContentType, content_type);
    }
    if (x_requested_with) {
      fetch->request_headers()->Add(HttpAttributes::kXRequestedWith,
                                    x_requested_with);
    }
    license_handler_.HandleRequest(path, request_body, is_global, fetch);
    int status = fetch->response_headers()->status_code();
    delete fetch;
    return status;
  }

  // Like DoRequest but without CSRF headers — for testing CSRF rejection.
  int DoRequestNoCsrf(StringPiece path, StringPiece request_body,
                      bool is_global, GoogleString* out_body) {
    return DoRequestWithHeaders(path, request_body, is_global,
                                nullptr, nullptr, out_body);
  }

  // Like DoRequest but with custom Content-Type — for testing CSRF variants.
  int DoRequestWithContentType(StringPiece path, StringPiece request_body,
                               bool is_global, StringPiece content_type,
                               GoogleString* out_body) {
    return DoRequestWithHeaders(path, request_body, is_global,
                                content_type.as_string().c_str(),
                                "XMLHttpRequest", out_body);
  }

  int DoGlobalRequest(StringPiece path, StringPiece request_body,
                      GoogleString* out_body) {
    return DoRequest(path, request_body, true, out_body);
  }

  int DoLocalRequest(StringPiece path, StringPiece request_body,
                     GoogleString* out_body) {
    return DoRequest(path, request_body, false, out_body);
  }

  void AdvancePastRateLimit() { mock_timer_.AdvanceMs(2000); }

  void AdvancePastRenewalRateLimit() {
    mock_timer_.AdvanceMs(3600000 + 1000);
  }

  GoogleString MakeToken(StringPiece sub, StringPiece plan, int64_t exp,
                         StringPiece sid = "", StringPiece kid = "k1",
                         int64_t iat = 1743379200) {
    LicensePayload payload;
    payload.sub = sub.as_string();
    payload.iss = "modpagespeed.com";
    payload.iat = iat;
    payload.plan = plan.as_string();
    payload.exp = exp;
    payload.sid = sid.as_string();
    payload.kid = kid.as_string();
    payload.products = {PAGESPEED_PRODUCT_ID};  // v3: authorize this product
    return SignLicenseToken(payload, public_key_, private_key_);
  }

  bool ApplyValidToken(StringPiece sub, StringPiece plan, int64_t exp,
                       StringPiece sid = "") {
    GoogleString token = MakeToken(sub, plan, exp, sid);
    GoogleString body;
    GoogleString json = StrCat("{\"key\":\"", token, "\"}");
    int status = DoGlobalRequest("/v1/license/apply", json, &body);
    return status == 200 &&
           body.find("\"success\":true") != GoogleString::npos;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer mock_timer_;
  MockMessageHandler handler_;
  CapturingUrlAsyncFetcher fetcher_;
  GoogleString tmp_dir_;
  AdminLicenseHandler license_handler_;
  GoogleString public_key_;
  GoogleString private_key_;
};

// ---------------------------------------------------------------------------
// HandleStatus tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, StatusReturnsUnlicensedByDefault) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"licensed\":false"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"trial_available\":true"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"is_global\":true"));
}

TEST_F(AdminLicenseHandlerTest, StatusReturnsLicensedAfterApply) {
  int64_t far_future = 1806451200;  // iat + 2 years (max lifetime)
  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", far_future,
                              "sub_abc"));
  AdvancePastRateLimit();

  GoogleString body;
  int status = DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"licensed\":true"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"license_type\":\"pro\""));
  EXPECT_THAT(body, ::testing::HasSubstr("\"domain\":\"test@example.com\""));
  EXPECT_THAT(body,
              ::testing::Not(::testing::HasSubstr("\"expired\":true")));
}

TEST_F(AdminLicenseHandlerTest, StatusShowsExpiredToken) {
  // Use iat in the past with exp shortly after — valid structure, but expired.
  int64_t expired_iat = 1706745600;           // Jan 31, 2024
  int64_t past = expired_iat + 86400;         // Feb 1, 2024
  GoogleString token = MakeToken("test@example.com", "pro", past, "", "k1",
                                 expired_iat);
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString apply_body;
  int apply_status = DoGlobalRequest("/v1/license/apply", json, &apply_body);
  EXPECT_EQ(200, apply_status);
  EXPECT_THAT(apply_body, ::testing::HasSubstr("\"success\":true"));
  AdvancePastRateLimit();

  GoogleString body;
  int status = DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"licensed\":true"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"expired\":true"));
}

TEST_F(AdminLicenseHandlerTest, RejectsTokenForWrongProduct) {
  // Create a token authorized for the 2.0 optimizer line (not mps1).
  int64_t far_future = 1806451200;
  LicensePayload payload;
  payload.sub = "wrong-product@example.com";
  payload.iss = "modpagespeed.com";
  payload.iat = 1743379200;  // March 31, 2026
  payload.plan = "pro";
  payload.exp = far_future;
  payload.kid = "k1";
  payload.products = {"the 2.0 optimizer line"};  // This build is mps1 — should reject.
  GoogleString token = SignLicenseToken(payload, public_key_, private_key_);

  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/apply", json, &body);
  EXPECT_EQ(200, status);  // HTTP 200 with success:false
  EXPECT_THAT(body, ::testing::HasSubstr("\"success\":false"));
  EXPECT_THAT(body, ::testing::HasSubstr("not valid for this product"));
  EXPECT_THAT(body, ::testing::HasSubstr("the 2.0 optimizer line"));

  // License should remain invalid.
  EXPECT_FALSE(license_handler_.IsLicenseValid());
}

// ---------------------------------------------------------------------------
// Global-only gating tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, StatusAvailableOnLocalAdmin) {
  GoogleString body;
  int status = DoLocalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"licensed\":false"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"is_global\":false"));
  EXPECT_THAT(body,
              ::testing::Not(::testing::HasSubstr("\"trial_available\":true")));
}

TEST_F(AdminLicenseHandlerTest, ApplyBlockedOnLocalAdmin) {
  GoogleString body;
  int status = DoLocalRequest("/v1/license/apply", "{\"key\":\"x\"}", &body);
  EXPECT_EQ(403, status);
  EXPECT_THAT(body, ::testing::HasSubstr("global admin"));
}

TEST_F(AdminLicenseHandlerTest, ActivateBlockedOnLocalAdmin) {
  GoogleString body;
  int status = DoLocalRequest("/v1/license/activate",
                              "{\"nonce\":\"abc\"}", &body);
  EXPECT_EQ(403, status);
  EXPECT_THAT(body, ::testing::HasSubstr("global admin"));
}

TEST_F(AdminLicenseHandlerTest, TrialBlockedOnLocalAdmin) {
  GoogleString body;
  int status = DoLocalRequest(
      "/v1/license/trial",
      "{\"email\":\"a@b.com\",\"terms_accepted_at\":\"t\","
      "\"terms_version\":\"1\"}",
      &body);
  EXPECT_EQ(403, status);
  EXPECT_THAT(body, ::testing::HasSubstr("global admin"));
}

TEST_F(AdminLicenseHandlerTest, ConsentBlockedOnLocalAdmin) {
  GoogleString body;
  int status = DoLocalRequest("/v1/license/consent",
                              "{\"accepted\":true}", &body);
  EXPECT_EQ(403, status);
  EXPECT_THAT(body, ::testing::HasSubstr("global admin"));
}

TEST_F(AdminLicenseHandlerTest, MutationEndpointsWorkOnGlobalAdmin) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent",
                               "{\"accepted\":true}", &body);
  EXPECT_EQ(200, status);
  EXPECT_EQ(1, fetcher_.fetch_count());
}

// ---------------------------------------------------------------------------
// HandleApply tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, ApplyRejectsEmptyKey) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/apply", "{\"key\":\"\"}", &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("License key must not be empty"));
}

TEST_F(AdminLicenseHandlerTest, ApplyRejectsOversizedBody) {
  GoogleString big_body(5000, 'x');
  GoogleString out;
  int status = DoGlobalRequest("/v1/license/apply", big_body, &out);
  EXPECT_EQ(400, status);
  EXPECT_THAT(out, ::testing::HasSubstr("Request body too large"));
}

TEST_F(AdminLicenseHandlerTest, ApplyRejectsMissingKey) {
  GoogleString body;
  int status =
      DoGlobalRequest("/v1/license/apply", "{\"foo\":\"bar\"}", &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Missing 'key' or 'license_key'"));
}

TEST_F(AdminLicenseHandlerTest, ApplyRejectsInvalidToken) {
  GoogleString body;
  int status =
      DoGlobalRequest("/v1/license/apply", "{\"key\":\"invalid\"}", &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"success\":false"));
}

TEST_F(AdminLicenseHandlerTest, ApplyAcceptsValidToken) {
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("customer@example.com", "enterprise",
                                 far_future, "sub_xyz");
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/apply", json, &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"success\":true"));
  EXPECT_THAT(body, ::testing::HasSubstr("\"plan\":\"enterprise\""));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
}

// the design record P4: the agent_optimize entitlement is parsed from the token and gates
// IsAgentOptimizeEntitled() (= license-valid AND the claim). A token WITHOUT the
// entitlement is still a valid license (soft check, never a reject); a token
// WITH it flips the gate on — and a renewed/re-applied token that drops it
// flips the gate back off.
TEST_F(AdminLicenseHandlerTest, AgentOptimizeEntitlementGate) {
  const int64_t kIat = 1743379200;
  const int64_t kExp = 1806451200;  // iat + 2 years (max lifetime).

  // Apply a signed token (with/without the entitlement) via the public
  // /v1/license/apply path (ApplyToken itself is private).
  auto apply = [&](bool with_entitlement) {
    LicensePayload payload;
    payload.sub = "customer@example.com";
    payload.iss = "modpagespeed.com";
    payload.iat = kIat;
    payload.plan = "enterprise";
    payload.exp = kExp;
    payload.products = {PAGESPEED_PRODUCT_ID};
    if (with_entitlement) {
      payload.entitlements = {"agent_optimize"};
    }
    GoogleString token = SignLicenseToken(payload, public_key_, private_key_);
    GoogleString json = StrCat("{\"key\":\"", token, "\"}");
    GoogleString body;
    int status = DoGlobalRequest("/v1/license/apply", json, &body);
    return status == 200 &&
           body.find("\"success\":true") != GoogleString::npos;
  };

  // No entitlement: valid license, but agent_optimize gate is off.
  ASSERT_TRUE(apply(false));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
  EXPECT_FALSE(license_handler_.IsAgentOptimizeEntitled());

  // With entitlement: gate flips on.
  ASSERT_TRUE(apply(true));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
  EXPECT_TRUE(license_handler_.IsAgentOptimizeEntitled());

  // Re-apply (renewal) a token that drops the entitlement: gate flips off,
  // license still valid.
  ASSERT_TRUE(apply(false));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
  EXPECT_FALSE(license_handler_.IsAgentOptimizeEntitled());
}

TEST_F(AdminLicenseHandlerTest, ApplyAcceptsLicenseKeyField) {
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("customer@example.com", "pro", far_future);
  GoogleString json = StrCat("{\"license_key\":\"", token, "\"}");
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/apply", json, &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"success\":true"));
}

TEST_F(AdminLicenseHandlerTest, ApplyPersistsTokenToDisk) {
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("customer@example.com", "pro", far_future);
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString body;
  DoGlobalRequest("/v1/license/apply", json, &body);

  std::filesystem::path license_path = LicenseFilePath(tmp_dir_);
  GoogleString disk_token;
  EXPECT_TRUE(ReadLicenseFile(license_path, &disk_token));
  EXPECT_EQ(token, disk_token);
}

// ---------------------------------------------------------------------------
// HandleConsent tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, ConsentProxiesAcceptedTrue) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent",
                               "{\"accepted\":true}", &body);
  EXPECT_EQ(200, status);
  EXPECT_EQ("http://test.example.com/api/consent", fetcher_.last_url());
  EXPECT_EQ("{\"accepted\":true}", fetcher_.last_body());
}

TEST_F(AdminLicenseHandlerTest, ConsentProxiesAcceptedFalse) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent",
                               "{\"accepted\":false}", &body);
  EXPECT_EQ(200, status);
  EXPECT_EQ("http://test.example.com/api/consent", fetcher_.last_url());
  EXPECT_EQ("{\"accepted\":false}", fetcher_.last_body());
}

TEST_F(AdminLicenseHandlerTest, ConsentRejectsMissingField) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent", "{}", &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body,
              ::testing::HasSubstr("Missing or invalid 'accepted' field"));
}

TEST_F(AdminLicenseHandlerTest, ConsentRejectsOversizedBody) {
  GoogleString big_body(5000, 'x');
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent", big_body, &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Request body too large"));
}

TEST_F(AdminLicenseHandlerTest, ConsentDoesNotAutoApplyToken) {
  // Consent passes auto_apply_token=false.  Verify that even if the
  // response contains a "token" field, it is NOT applied.
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("sneaky@example.com", "pro", far_future);
  fetcher_.set_response_body(
      StrCat("{\"token\":\"", token, "\",\"ok\":true}"));
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent",
                               "{\"accepted\":true}", &body);
  EXPECT_EQ(200, status);
  EXPECT_FALSE(license_handler_.IsLicenseValid());
}

// ---------------------------------------------------------------------------
// HandleActivate tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, ActivateProxiesNonceAndOrderRef) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/activate",
                               "{\"nonce\":\"abc\",\"order_ref\":\"def\"}",
                               &body);
  EXPECT_EQ(200, status);
  EXPECT_EQ("http://test.example.com/api/activate", fetcher_.last_url());
  EXPECT_THAT(fetcher_.last_body(), ::testing::HasSubstr("\"nonce\":\"abc\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"order_ref\":\"def\""));
  // Tracking metadata.
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"server\":\"" PAGESPEED_SERVER "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"os\":\"" PAGESPEED_OS "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"arch\":\"" PAGESPEED_ARCH "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"distribution\":\"" PAGESPEED_DISTRIBUTION "\""));
}

TEST_F(AdminLicenseHandlerTest, ActivateRejectsMissingNonce) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/activate",
                               "{\"order_ref\":\"def\"}", &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Missing 'nonce' field"));
}

TEST_F(AdminLicenseHandlerTest, ActivateWithNonceOnlySucceeds) {
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/activate",
                               "{\"nonce\":\"abc\"}", &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(fetcher_.last_body(), ::testing::HasSubstr("\"nonce\":\"abc\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::Not(::testing::HasSubstr("order_ref")));
}

TEST_F(AdminLicenseHandlerTest, ActivateRejectsOversizedBody) {
  GoogleString big_body(5000, 'x');
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/activate", big_body, &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Request body too large"));
}

TEST_F(AdminLicenseHandlerTest, ActivateAutoAppliesValidToken) {
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("activated@example.com", "pro", far_future);
  fetcher_.set_response_body(
      StrCat("{\"token\":\"", token, "\",\"message\":\"activated\"}"));
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/activate",
                               "{\"nonce\":\"abc\",\"order_ref\":\"def\"}",
                               &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"message\":\"activated\""));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
}

TEST_F(AdminLicenseHandlerTest, ActivateAutoApplyFailsForInvalidToken) {
  fetcher_.set_response_body(
      "{\"token\":\"fake.token.value\",\"message\":\"activated\"}");
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/activate",
                               "{\"nonce\":\"abc\",\"order_ref\":\"def\"}",
                               &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"message\":\"activated\""));
  EXPECT_FALSE(license_handler_.IsLicenseValid());
}

// ---------------------------------------------------------------------------
// HandleTrial tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, TrialProxiesEmailAndTerms) {
  GoogleString body;
  int status = DoGlobalRequest(
      "/v1/license/trial",
      "{\"email\":\"test@example.com\","
      "\"terms_accepted_at\":\"2025-01-01T00:00:00Z\","
      "\"terms_version\":\"1.0\"}",
      &body);
  EXPECT_EQ(200, status);
  EXPECT_EQ("http://test.example.com/api/trial", fetcher_.last_url());
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"email\":\"test@example.com\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"terms_accepted_at\":\"2025-01-01"));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"terms_version\":\"1.0\""));
  // Tracking metadata.
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"server\":\"" PAGESPEED_SERVER "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"os\":\"" PAGESPEED_OS "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"arch\":\"" PAGESPEED_ARCH "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"distribution\":\"" PAGESPEED_DISTRIBUTION "\""));
}

TEST_F(AdminLicenseHandlerTest, TrialRejectsMissingEmail) {
  GoogleString body;
  int status = DoGlobalRequest(
      "/v1/license/trial",
      "{\"terms_accepted_at\":\"2025-01-01T00:00:00Z\","
      "\"terms_version\":\"1.0\"}",
      &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Missing 'email' field"));
}

TEST_F(AdminLicenseHandlerTest, TrialRejectsMissingTermsAcceptedAt) {
  GoogleString body;
  int status = DoGlobalRequest(
      "/v1/license/trial",
      "{\"email\":\"test@example.com\","
      "\"terms_version\":\"1.0\"}",
      &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Missing 'terms_accepted_at' field"));
}

TEST_F(AdminLicenseHandlerTest, TrialRejectsMissingTermsVersion) {
  GoogleString body;
  int status = DoGlobalRequest(
      "/v1/license/trial",
      "{\"email\":\"test@example.com\","
      "\"terms_accepted_at\":\"2025-01-01T00:00:00Z\"}",
      &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Missing 'terms_version' field"));
}

TEST_F(AdminLicenseHandlerTest, TrialRejectsOversizedBody) {
  GoogleString big_body(5000, 'x');
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/trial", big_body, &body);
  EXPECT_EQ(400, status);
  EXPECT_THAT(body, ::testing::HasSubstr("Request body too large"));
}

// ---------------------------------------------------------------------------
// Rate limiting tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, RateLimitRejectsSecondRequestTooFast) {
  GoogleString body1;
  int status1 = DoGlobalRequest("/v1/license/consent",
                                "{\"accepted\":true}", &body1);
  EXPECT_EQ(200, status1);
  EXPECT_EQ(1, fetcher_.fetch_count());

  GoogleString body2;
  int status2 = DoGlobalRequest("/v1/license/consent",
                                "{\"accepted\":true}", &body2);
  EXPECT_EQ(429, status2);
  EXPECT_THAT(body2, ::testing::HasSubstr("Rate limited"));
  EXPECT_EQ(1, fetcher_.fetch_count());
}

TEST_F(AdminLicenseHandlerTest, RateLimitAllowsAfterInterval) {
  GoogleString body1;
  int status1 = DoGlobalRequest("/v1/license/consent",
                                "{\"accepted\":true}", &body1);
  EXPECT_EQ(200, status1);
  EXPECT_EQ(1, fetcher_.fetch_count());

  AdvancePastRateLimit();

  GoogleString body2;
  int status2 = DoGlobalRequest("/v1/license/consent",
                                "{\"accepted\":true}", &body2);
  EXPECT_EQ(200, status2);
  EXPECT_EQ(2, fetcher_.fetch_count());
}

// ---------------------------------------------------------------------------
// HandleRequest routing tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, UnknownPathReturnsFalse) {
  GoogleString body;
  StringAsyncFetch* fetch = NewFetch(&body);
  // Must set valid CSRF headers to get past the CSRF check.
  fetch->request_headers()->Add(HttpAttributes::kContentType,
                                "application/json");
  fetch->request_headers()->Add(HttpAttributes::kXRequestedWith,
                                "XMLHttpRequest");
  bool handled = license_handler_.HandleRequest(
      "/v1/license/unknown", "", true, fetch);
  EXPECT_FALSE(handled);
  delete fetch;
}

TEST_F(AdminLicenseHandlerTest, UpstreamFetchFailureReturns502) {
  fetcher_.set_simulate_success(false);
  GoogleString body;
  int status = DoGlobalRequest("/v1/license/consent",
                               "{\"accepted\":true}", &body);
  EXPECT_EQ(502, status);
  EXPECT_THAT(body, ::testing::HasSubstr("License service unavailable"));
}

// ---------------------------------------------------------------------------
// License state callback tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, CallbackInvokedOnApply) {
  bool callback_value = false;
  bool callback_invoked = false;
  license_handler_.set_license_state_callback(
      [&](bool valid, bool /*agent_optimize*/) {
        callback_value = valid;
        callback_invoked = true;
      });

  int64_t far_future = 1806451200;
  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", far_future));
  EXPECT_TRUE(callback_invoked);
  EXPECT_TRUE(callback_value);
}

TEST_F(AdminLicenseHandlerTest, CallbackNotInvokedForBadToken) {
  bool callback_invoked = false;
  license_handler_.set_license_state_callback(
      [&](bool valid, bool /*agent_optimize*/) { callback_invoked = true; });

  GoogleString body;
  DoGlobalRequest("/v1/license/apply", "{\"key\":\"invalid\"}", &body);
  EXPECT_FALSE(callback_invoked);
}

TEST_F(AdminLicenseHandlerTest, CallbackReportsExpiredToken) {
  bool callback_value = true;
  bool callback_invoked = false;
  license_handler_.set_license_state_callback(
      [&](bool valid, bool /*agent_optimize*/) {
        callback_value = valid;
        callback_invoked = true;
      });

  int64_t expired_iat = 1706745600;           // Jan 31, 2024
  int64_t past = expired_iat + 86400;         // Feb 1, 2024
  GoogleString token = MakeToken("test@example.com", "pro", past, "", "k1",
                                 expired_iat);
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString body;
  DoGlobalRequest("/v1/license/apply", json, &body);
  EXPECT_TRUE(callback_invoked);
  // Expired token → IsLicenseValid() returns false.
  EXPECT_FALSE(callback_value);
}

// ---------------------------------------------------------------------------
// IsLicenseValid tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, IsLicenseValidDefaultFalse) {
  EXPECT_FALSE(license_handler_.IsLicenseValid());
}

TEST_F(AdminLicenseHandlerTest, IsLicenseValidAfterValidToken) {
  int64_t far_future = 1806451200;
  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", far_future));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
}

TEST_F(AdminLicenseHandlerTest, IsLicenseValidFalseForExpired) {
  // Use iat in the past with exp shortly after — valid structure, but expired.
  int64_t expired_iat = 1706745600;           // Jan 31, 2024
  int64_t past = expired_iat + 86400;         // Feb 1, 2024
  GoogleString token = MakeToken("test@example.com", "pro", past, "", "k1",
                                 expired_iat);
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString body;
  DoGlobalRequest("/v1/license/apply", json, &body);
  EXPECT_FALSE(license_handler_.IsLicenseValid());
}

// ---------------------------------------------------------------------------
// MaybeRenew / Auto-renewal tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, MaybeRenewSkipsWithoutLicense) {
  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(0, fetcher_.fetch_count());
}

TEST_F(AdminLicenseHandlerTest, MaybeRenewSkipsWithoutSid) {
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("test@example.com", "pro", far_future, "");
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString apply_body;
  DoGlobalRequest("/v1/license/apply", json, &apply_body);
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(0, fetcher_.fetch_count());
}

TEST_F(AdminLicenseHandlerTest, MaybeRenewSkipsWhenNotInWindow) {
  int64_t far_future = 1806451200;  // iat + 2 years (max lifetime)
  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", far_future,
                              "sub_active"));
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(0, fetcher_.fetch_count());
}

TEST_F(AdminLicenseHandlerTest, MaybeRenewTriggersInWindow) {
  // Set mock timer to 3 days before a far-future expiry so the renewal
  // window (7 days) is triggered.  exp must be in the real future so the
  // system clock doesn't mark it expired.
  int64_t exp = 1806451200;  // iat + 2 years (max lifetime)
  int64_t three_days_before_ms =
      static_cast<int64_t>(exp - 3 * 24 * 3600) * 1000;
  mock_timer_.SetTimeUs(three_days_before_ms * 1000);

  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", exp,
                              "sub_active"));
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  fetcher_.set_response_body("{}");
  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(1, fetcher_.fetch_count());
  EXPECT_THAT(fetcher_.last_url(),
              ::testing::HasSubstr("/api/renew"));
  // Tracking metadata in renewal request.
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"server\":\"" PAGESPEED_SERVER "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"os\":\"" PAGESPEED_OS "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"arch\":\"" PAGESPEED_ARCH "\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"distribution\":\"" PAGESPEED_DISTRIBUTION "\""));
}

TEST_F(AdminLicenseHandlerTest, MaybeRenewTriggersForExpired) {
  int64_t expired_iat = 1706745600;           // Jan 31, 2024
  int64_t past = expired_iat + 86400;         // Feb 1, 2024
  GoogleString token = MakeToken("test@example.com", "pro", past, "sub_exp",
                                 "k1", expired_iat);
  GoogleString json = StrCat("{\"key\":\"", token, "\"}");
  GoogleString apply_body;
  DoGlobalRequest("/v1/license/apply", json, &apply_body);
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  fetcher_.set_response_body("{}");
  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(1, fetcher_.fetch_count());
  EXPECT_THAT(fetcher_.last_url(), ::testing::HasSubstr("/api/renew"));
}

TEST_F(AdminLicenseHandlerTest, MaybeRenewRateLimitsAtOneHour) {
  int64_t exp = 1806451200;
  int64_t three_days_before_ms =
      static_cast<int64_t>(exp - 3 * 24 * 3600) * 1000;
  mock_timer_.SetTimeUs(three_days_before_ms * 1000);

  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", exp,
                              "sub_active"));
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  fetcher_.set_response_body("{}");
  GoogleString body1;
  DoGlobalRequest("/v1/license/status", "", &body1);
  EXPECT_EQ(1, fetcher_.fetch_count());

  AdvancePastRateLimit();
  GoogleString body2;
  DoGlobalRequest("/v1/license/status", "", &body2);
  EXPECT_EQ(1, fetcher_.fetch_count());  // Rate limited.

  AdvancePastRenewalRateLimit();
  GoogleString body3;
  DoGlobalRequest("/v1/license/status", "", &body3);
  EXPECT_EQ(2, fetcher_.fetch_count());
}

TEST_F(AdminLicenseHandlerTest, MaybeRenewAppliesNewToken) {
  int64_t exp = 1806451200;
  int64_t three_days_before_ms =
      static_cast<int64_t>(exp - 3 * 24 * 3600) * 1000;
  mock_timer_.SetTimeUs(three_days_before_ms * 1000);

  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", exp,
                              "sub_active"));
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  int64_t new_expiry = 1806451200;  // iat + 2 years (max lifetime)
  GoogleString renewed_token = MakeToken("test@example.com", "pro",
                                         new_expiry, "sub_active");
  fetcher_.set_response_body(
      StrCat("{\"token\":\"", renewed_token, "\"}"));

  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(1, fetcher_.fetch_count());
  EXPECT_TRUE(license_handler_.IsLicenseValid());
  EXPECT_EQ(renewed_token, license_handler_.license_token());
}

// ---------------------------------------------------------------------------
// SID guard tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, SidGuardRejectsMismatchedSid) {
  int64_t exp = 1806451200;
  int64_t three_days_before_ms =
      static_cast<int64_t>(exp - 3 * 24 * 3600) * 1000;
  mock_timer_.SetTimeUs(three_days_before_ms * 1000);

  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", exp,
                              "sub_original"));
  GoogleString original_token = license_handler_.license_token();
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  int64_t new_expiry = 1806451200;
  GoogleString wrong_sid_token = MakeToken("test@example.com", "pro",
                                           new_expiry, "sub_different");
  fetcher_.set_response_body(
      StrCat("{\"token\":\"", wrong_sid_token, "\"}"));

  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(1, fetcher_.fetch_count());
  EXPECT_EQ(original_token, license_handler_.license_token());
}

TEST_F(AdminLicenseHandlerTest, SidGuardAcceptsMatchingSid) {
  int64_t exp = 1806451200;
  int64_t three_days_before_ms =
      static_cast<int64_t>(exp - 3 * 24 * 3600) * 1000;
  mock_timer_.SetTimeUs(three_days_before_ms * 1000);

  ASSERT_TRUE(ApplyValidToken("test@example.com", "pro", exp,
                              "sub_same"));
  AdvancePastRateLimit();
  fetcher_.reset_fetch_count();

  int64_t new_expiry = 1806451200;
  GoogleString matching_token = MakeToken("test@example.com", "pro",
                                          new_expiry, "sub_same");
  fetcher_.set_response_body(
      StrCat("{\"token\":\"", matching_token, "\"}"));

  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(1, fetcher_.fetch_count());
  EXPECT_EQ(matching_token, license_handler_.license_token());
}

// ---------------------------------------------------------------------------
// Init / license file tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, InitLoadsLicenseFromDisk) {
  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("disk@example.com", "pro", far_future);
  std::filesystem::path license_path = LicenseFilePath(tmp_dir_);
  ASSERT_TRUE(WriteLicenseFile(license_path, token));

  AdminLicenseHandler handler2(&mock_timer_, thread_system_.get(), &handler_,
                               &fetcher_, tmp_dir_);
  handler2.Init();
  EXPECT_TRUE(handler2.IsLicenseValid());
  EXPECT_EQ(token, handler2.license_token());
}

TEST_F(AdminLicenseHandlerTest, InitIgnoresInvalidFileToken) {
  std::filesystem::path license_path = LicenseFilePath(tmp_dir_);
  ASSERT_TRUE(WriteLicenseFile(license_path, "not-a-valid-token"));

  AdminLicenseHandler handler2(&mock_timer_, thread_system_.get(), &handler_,
                               &fetcher_, tmp_dir_);
  handler2.Init();
  EXPECT_FALSE(handler2.IsLicenseValid());
}

TEST_F(AdminLicenseHandlerTest, InitHandlesMissingFile) {
  AdminLicenseHandler handler2(&mock_timer_, thread_system_.get(), &handler_,
                               &fetcher_, tmp_dir_);
  handler2.Init();
  EXPECT_FALSE(handler2.IsLicenseValid());
}

TEST_F(AdminLicenseHandlerTest, StatusReloadsFromDiskWhenInvalid) {
  EXPECT_FALSE(license_handler_.IsLicenseValid());

  int64_t far_future = 1806451200;
  GoogleString token = MakeToken("reloaded@example.com", "pro", far_future);
  std::filesystem::path license_path = LicenseFilePath(tmp_dir_);
  ASSERT_TRUE(WriteLicenseFile(license_path, token));

  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_THAT(body, ::testing::HasSubstr("\"licensed\":true"));
  EXPECT_TRUE(license_handler_.IsLicenseValid());
}

TEST_F(AdminLicenseHandlerTest, StatusDoesNotReloadWhenAlreadyValid) {
  // Apply a valid token in memory.
  int64_t far_future = 1806451200;
  ASSERT_TRUE(ApplyValidToken("original@example.com", "pro", far_future));
  GoogleString original_token = license_handler_.license_token();
  AdvancePastRateLimit();

  // Write a DIFFERENT token to disk (simulating another worker).
  GoogleString other_token = MakeToken("other@example.com", "enterprise",
                                       far_future);
  std::filesystem::path license_path = LicenseFilePath(tmp_dir_);
  ASSERT_TRUE(WriteLicenseFile(license_path, other_token));

  // Status poll should NOT reload from disk since license is already valid.
  GoogleString body;
  DoGlobalRequest("/v1/license/status", "", &body);
  EXPECT_EQ(original_token, license_handler_.license_token());
  EXPECT_THAT(body, ::testing::HasSubstr("\"domain\":\"original@example.com\""));
}

// ---------------------------------------------------------------------------
// Input sanitization tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, ActivateSanitizesInjectionAttempt) {
  GoogleString body;
  int status = DoGlobalRequest(
      "/v1/license/activate",
      "{\"nonce\":\"abc\\\",\\\"evil\\\":\\\"true\",\"order_ref\":\"def\"}",
      &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(fetcher_.last_body(), ::testing::HasSubstr("\"nonce\":\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::Not(::testing::HasSubstr("\"evil\":")));
}

TEST_F(AdminLicenseHandlerTest, TrialSanitizesFields) {
  GoogleString body;
  int status = DoGlobalRequest(
      "/v1/license/trial",
      "{\"email\":\"a@b.com\",\"terms_accepted_at\":\"t\","
      "\"terms_version\":\"1\",\"extra\":\"ignored\"}",
      &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(fetcher_.last_body(), ::testing::HasSubstr("\"email\":"));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"terms_accepted_at\":"));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"terms_version\":"));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::Not(::testing::HasSubstr("\"extra\":")));
  // Tracking metadata is always included.
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"server\":\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"os\":\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"arch\":\""));
  EXPECT_THAT(fetcher_.last_body(),
              ::testing::HasSubstr("\"distribution\":\""));
}

// ---------------------------------------------------------------------------
// CSRF protection tests
// ---------------------------------------------------------------------------

TEST_F(AdminLicenseHandlerTest, CsrfBlocksMissingHeaders) {
  GoogleString body;
  int status = DoRequestNoCsrf("/v1/license/apply", "{\"key\":\"x\"}", true,
                               &body);
  EXPECT_EQ(403, status);
  EXPECT_THAT(body, ::testing::HasSubstr("CSRF"));
}

TEST_F(AdminLicenseHandlerTest, CsrfBlocksTextPlainContentType) {
  GoogleString body;
  int status = DoRequestWithContentType(
      "/v1/license/apply", "{\"key\":\"x\"}", true, "text/plain", &body);
  EXPECT_EQ(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfBlocksFormUrlEncoded) {
  GoogleString body;
  int status = DoRequestWithContentType(
      "/v1/license/apply", "key=x", true,
      "application/x-www-form-urlencoded", &body);
  EXPECT_EQ(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfBlocksMultipartFormData) {
  GoogleString body;
  int status = DoRequestWithContentType(
      "/v1/license/apply", "key=x", true,
      "multipart/form-data; boundary=----", &body);
  EXPECT_EQ(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfBlocksMissingXRequestedWith) {
  GoogleString body;
  // Content-Type is valid but X-Requested-With is intentionally omitted.
  int status = DoRequestWithHeaders("/v1/license/apply", "{\"key\":\"x\"}",
                                    true, "application/json", nullptr, &body);
  EXPECT_EQ(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfBlocksWrongXRequestedWith) {
  GoogleString body;
  // X-Requested-With has wrong case — must be exactly "XMLHttpRequest".
  int status = DoRequestWithHeaders("/v1/license/apply", "{\"key\":\"x\"}",
                                    true, "application/json",
                                    "xmlhttprequest", &body);
  EXPECT_EQ(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfAcceptsJsonWithCharset) {
  GoogleString body;
  int status = DoRequestWithContentType(
      "/v1/license/apply", "{\"key\":\"x\"}", true,
      "application/json; charset=utf-8", &body);
  // Should pass CSRF (400 = body validation, not 403 = CSRF rejection)
  EXPECT_NE(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfAcceptsCaseInsensitiveContentType) {
  GoogleString body;
  int status = DoRequestWithContentType(
      "/v1/license/apply", "{\"key\":\"x\"}", true,
      "APPLICATION/JSON", &body);
  EXPECT_NE(403, status);
}

TEST_F(AdminLicenseHandlerTest, CsrfSkippedForStatusEndpoint) {
  // /v1/license/status should work without any CSRF headers.
  GoogleString body;
  int status = DoRequestNoCsrf("/v1/license/status", "", true, &body);
  EXPECT_EQ(200, status);
  EXPECT_THAT(body, ::testing::HasSubstr("\"licensed\":"));
}

// ---------------------------------------------------------------------------
// IsLicenseTimeValid: expiry must be re-derived from "now" on every check.
// Regression for the audit finding (2026-05-29) where IsLicenseValid trusted a
// cached apply-time flag and so kept a once-valid token valid forever.
// ---------------------------------------------------------------------------

static const int64_t kGrace = 72 * 3600;  // mirrors kGracePeriodSec

TEST(IsLicenseTimeValidTest, NoExpiryTokenIsAlwaysValid) {
  // expires_at <= 0 means a v1 token with no expiry: valid at any time.
  EXPECT_TRUE(AdminLicenseHandler::IsLicenseTimeValid(0, 0, kGrace));
  EXPECT_TRUE(AdminLicenseHandler::IsLicenseTimeValid(0, 9999999999LL, kGrace));
}

TEST(IsLicenseTimeValidTest, ValidBeforeAndAtExpiry) {
  const int64_t exp = 2000000000;  // some fixed future instant
  EXPECT_TRUE(
      AdminLicenseHandler::IsLicenseTimeValid(exp, exp - 86400, kGrace));
  EXPECT_TRUE(AdminLicenseHandler::IsLicenseTimeValid(exp, exp, kGrace));
}

TEST(IsLicenseTimeValidTest, ValidWithinGraceWindowAfterExpiry) {
  const int64_t exp = 2000000000;
  EXPECT_TRUE(AdminLicenseHandler::IsLicenseTimeValid(exp, exp + 1, kGrace));
  EXPECT_TRUE(
      AdminLicenseHandler::IsLicenseTimeValid(exp, exp + kGrace - 1, kGrace));
}

TEST(IsLicenseTimeValidTest, InvalidPastGraceWindow) {
  // This is the case the old code got wrong: a token whose exp (and grace)
  // are now in the past must be rejected, regardless of how it looked at
  // apply time.
  const int64_t exp = 2000000000;
  EXPECT_FALSE(
      AdminLicenseHandler::IsLicenseTimeValid(exp, exp + kGrace, kGrace));
  EXPECT_FALSE(
      AdminLicenseHandler::IsLicenseTimeValid(exp, exp + kGrace + 1, kGrace));
  EXPECT_FALSE(AdminLicenseHandler::IsLicenseTimeValid(
      exp, exp + 365LL * 24 * 3600, kGrace));
}

}  // namespace

}  // namespace net_instaweb
