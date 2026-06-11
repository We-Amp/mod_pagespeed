// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/admin_license_handler.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/license_v2/license_file.h"
#include "pagespeed/kernel/license_v2/license_token.h"
#include "pagespeed/kernel/license_v2/license_verifier.h"
#include "pagespeed/kernel/license_v2/tracking_metadata.h"
#include "pagespeed/system/json_utils.h"

namespace net_instaweb {

namespace {

static const int kMaxRequestBodySize = 4096;

// The default license service URL.  May be overridden for testing.
// Test-only override; intentionally leaked at process exit.
GoogleString* license_service_url_override = nullptr;

const char kDefaultLicenseServiceUrl[] = "https://api.modpagespeed.com";
const char kLicenseServiceUrlEnvVar[] = "PAGESPEED_LICENSE_SERVICE_URL";

}  // namespace

// LicenseProxyFetch captures the upstream response from the license service
// and forwards it to the original client fetch.  Self-deletes on completion.
class LicenseProxyFetch : public StringAsyncFetch {
 public:
  LicenseProxyFetch(AsyncFetch* client_fetch, AdminLicenseHandler* handler,
                    bool auto_apply_token, Timer* timer,
                    MessageHandler* message_handler)
      : StringAsyncFetch(client_fetch->request_context()),
        client_fetch_(client_fetch),
        handler_(handler),
        auto_apply_token_(auto_apply_token),
        timer_(timer),
        message_handler_(message_handler) {}

  void HandleDone(bool success) override {
    // Mark ourselves done/success for StringAsyncFetch bookkeeping.
    set_success(success);
    set_done(true);

    if (!success) {
      // Upstream fetch failed -- return 502 to client.
      ResponseHeaders* headers = client_fetch_->response_headers();
      headers->set_status_code(HttpStatus::kBadGateway);
      headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
      client_fetch_->Write("{\"error\":\"License service unavailable\"}",
                           message_handler_);
      client_fetch_->Done(true);
    } else {
      // Forward the upstream status code and body.
      int status = response_headers()->status_code();
      ResponseHeaders* client_headers = client_fetch_->response_headers();
      client_headers->set_status_code(status);
      client_headers->Add(HttpAttributes::kContentType,
                          kContentTypeJson.mime_type());

      const GoogleString& body = buffer();

      // If auto-apply is requested and the response looks successful,
      // try to extract and apply the token.
      if (auto_apply_token_ && status == HttpStatus::kOK) {
        GoogleString token;
        if (ExtractJsonStringField(body, "token", &token) && !token.empty()) {
          GoogleString error;
          if (handler_->ApplyToken(token, &error)) {
            // Persist to disk (best-effort).
            std::filesystem::path path = LicenseFilePath(handler_->cache_path_);
            if (WriteLicenseFile(path, token)) {
              message_handler_->Message(kInfo, "License saved to %s",
                                        path.string().c_str());
            }
          } else {
            message_handler_->Message(kWarning,
                                      "Auto-apply of license token failed: %s",
                                      error.c_str());
          }
        }
      }

      client_fetch_->Write(body, message_handler_);
      client_fetch_->Done(true);
    }

    // Clear in-flight guard as the LAST operation before self-delete.
    // This ensures the destructor's wait loop won't exit while we're
    // still accessing handler_ state above.
    handler_->request_in_flight_.store(false, std::memory_order_release);
    delete this;
  }

 private:
  AsyncFetch* client_fetch_;
  AdminLicenseHandler* handler_;
  bool auto_apply_token_;
  Timer* timer_;
  MessageHandler* message_handler_;

  LicenseProxyFetch(const LicenseProxyFetch&) = delete;
  LicenseProxyFetch& operator=(const LicenseProxyFetch&) = delete;
};

AdminLicenseHandler::AdminLicenseHandler(Timer* timer,
                                         ThreadSystem* thread_system,
                                         MessageHandler* handler,
                                         UrlAsyncFetcher* fetcher,
                                         const GoogleString& cache_path)
    : timer_(timer),
      thread_system_(thread_system),
      handler_(handler),
      fetcher_(fetcher),
      cache_path_(cache_path) {}

AdminLicenseHandler::~AdminLicenseHandler() {
  // Wait for any in-flight proxy fetch to complete before destroying.
  // Hard timeout prevents shutdown hangs if a fetch never completes.
  static constexpr int kDestructorTimeoutMs = 5000;
  static constexpr int kPollIntervalMs = 10;
  int waited_ms = 0;
  while (request_in_flight_.load(std::memory_order_acquire)) {
    if (waited_ms >= kDestructorTimeoutMs) {
      // Use fprintf instead of LOG() — the logging sink may already be torn
      // down during process shutdown, causing a use-after-free crash.
      fprintf(stderr,
              "AdminLicenseHandler: timed out waiting for in-flight "
              "fetch to complete during shutdown\n");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    waited_ms += kPollIntervalMs;
  }
}

void AdminLicenseHandler::Init() {
  // Try to read license from the default file path.
  GoogleString token;
  std::filesystem::path path = LicenseFilePath(cache_path_);
  if (ReadLicenseFile(path, &token)) {
    GoogleString error;
    if (ApplyToken(token, &error)) {
      handler_->Message(kInfo, "License loaded from %s", path.string().c_str());
    } else {
      handler_->Message(kWarning, "Invalid license in %s: %s",
                        path.string().c_str(), error.c_str());
    }
  }
}

bool AdminLicenseHandler::IsLicenseTimeValid(int64_t expires_at,
                                             int64_t now_sec,
                                             int64_t grace_sec) {
  // A v1 token with no expiry is always valid.
  if (expires_at <= 0) return true;
  // Not yet expired.
  if (now_sec <= expires_at) return true;
  // Expired: allow optimization to continue during the grace window.
  // Overflow-safe form of now_sec < expires_at + grace_sec (now_sec is past
  // expires_at here, so the subtraction is non-negative).
  return (now_sec - expires_at) < grace_sec;
}

bool AdminLicenseHandler::IsLicenseValidLocked() const {
  if (!license_valid_) return false;

  // Always re-derive expiry from the current wall clock rather than trusting
  // the apply-time license_expired_ flag. Otherwise a token that was valid
  // when applied would keep optimizing forever (until restart / disk reload /
  // successful renewal), defeating the time-bounded licensing model — even
  // long after the token's exp has passed.
  int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  return IsLicenseTimeValid(license_expires_at_, now_sec, kGracePeriodSec);
}

bool AdminLicenseHandler::IsLicenseValid() const {
  std::lock_guard<std::mutex> lock(license_mu_);
  return IsLicenseValidLocked();
}

bool AdminLicenseHandler::IsAgentOptimizeEntitled() const {
  // the design record: the served entitlement = license valid AND the agent_optimize
  // claim. An expired license (license gate dominates) or a valid license
  // without the claim both yield false.
  std::lock_guard<std::mutex> lock(license_mu_);
  return IsLicenseValidLocked() && license_agent_optimize_;
}

void AdminLicenseHandler::NotifyLicenseStateChange() {
  if (license_state_callback_) {
    license_state_callback_(IsLicenseValid(), IsAgentOptimizeEntitled());
  }
}

bool AdminLicenseHandler::ApplyToken(StringPiece token, GoogleString* error) {
  LicenseResult result = VerifyLicenseToken(token);
  if (!result.valid) {
    *error = result.error;
    return false;
  }

  // Verify the token authorizes this product.
  if (!CheckProductAuthorization(result.payload, PAGESPEED_PRODUCT_ID)) {
    GoogleString licensed_for;
    for (size_t i = 0; i < result.payload.products.size(); ++i) {
      if (i > 0) licensed_for += ", ";
      licensed_for += result.payload.products[i];
    }
    *error = StrCat("license not valid for this product (licensed for: ",
                    licensed_for.empty() ? "none" : licensed_for,
                    ", running: ", PAGESPEED_PRODUCT_ID, ")");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(license_mu_);
    token.CopyToString(&license_token_);
    license_valid_ = true;
    license_expired_ = result.expired;
    license_expires_at_ = result.expires_at;
    license_plan_ = result.payload.plan;
    license_sub_ = result.payload.sub;
    license_sid_ = result.payload.sid;
    license_iat_ = result.payload.iat;
    // the design record: display-only; empty on legacy scopeless tokens (R9/R10).
    license_scope_ = result.payload.scope;
    license_domain_ = result.payload.domain;
    // the design record: re-evaluated on every apply/renew (same posture as the product
    // check). Absent entitlement = false = feature off; never a token reject.
    license_agent_optimize_ =
        CheckEntitlement(result.payload, "agent_optimize");
  }
  NotifyLicenseStateChange();
  return true;
}

bool AdminLicenseHandler::HandleRequest(StringPiece path,
                                        StringPiece request_body,
                                        bool is_global, AsyncFetch* fetch) {
  if (path == "/v1/license/status") {
    HandleStatus(is_global, fetch);
    return true;
  }

  // CSRF protection: mutation endpoints require correct headers.
  // This blocks cross-origin form submissions and simple CORS requests.
  const RequestHeaders* req_headers = fetch->request_headers();
  const char* content_type = req_headers->Lookup1(HttpAttributes::kContentType);
  const char* x_requested_with =
      req_headers->Lookup1(HttpAttributes::kXRequestedWith);
  if (content_type == nullptr ||
      !StringCaseStartsWith(StringPiece(content_type), "application/json") ||
      x_requested_with == nullptr ||
      StringPiece(x_requested_with) != "XMLHttpRequest") {
    WriteJsonError(fetch, HttpStatus::kForbidden,
                   "Missing or invalid CSRF headers");
    return true;
  }

  // Mutation endpoints require the global admin endpoint.
  if (!is_global) {
    if (path == "/v1/license/apply" || path == "/v1/license/activate" ||
        path == "/v1/license/consent") {
      WriteJsonError(fetch, HttpStatus::kForbidden,
                     "License management is only available on the global "
                     "admin endpoint (/pagespeed_global_admin/)");
      return true;
    }
    return false;
  }

  if (path == "/v1/license/apply") {
    HandleApply(request_body, fetch);
    return true;
  }
  if (path == "/v1/license/activate") {
    HandleActivate(request_body, fetch);
    return true;
  }
  if (path == "/v1/license/consent") {
    HandleConsent(request_body, fetch);
    return true;
  }
  return false;
}

void AdminLicenseHandler::HandleStatus(bool is_global, AsyncFetch* fetch) {
  // If we don't have a license in memory, try reloading from disk.
  // This handles the case where another nginx worker wrote the license file
  // after activation, but this worker hasn't seen it yet.
  if (!IsLicenseValid()) {
    GoogleString token;
    std::filesystem::path path = LicenseFilePath(cache_path_);
    if (ReadLicenseFile(path, &token)) {
      GoogleString error;
      if (ApplyToken(token, &error)) {
        handler_->Message(kInfo, "License reloaded from %s",
                          path.string().c_str());
      }
    }
  }

  // Attempt auto-renewal if needed (piggybacked on status polls).
  MaybeRenew();

  // Copy license state under the lock, then build JSON without holding it.
  bool valid;
  bool expired;
  int64_t expires_at;
  GoogleString plan;
  GoogleString sub;
  GoogleString scope;
  GoogleString site_domain;
  {
    std::lock_guard<std::mutex> lock(license_mu_);
    valid = license_valid_;
    expired = license_expired_;
    expires_at = license_expires_at_;
    plan = license_plan_;
    sub = license_sub_;
    scope = license_scope_;
    site_domain = license_domain_;
  }

  GoogleString json = "{";
  StrAppend(&json, "\"licensed\":", valid ? "true" : "false");
  StrAppend(&json, ",\"is_global\":", is_global ? "true" : "false");
  if (valid) {
    if (!plan.empty()) {
      StrAppend(&json, ",\"license_type\":\"", JsonEscape(plan), "\"");
    }
    if (expires_at > 0) {
      StrAppend(&json, ",\"expires\":", Integer64ToString(expires_at));
    }
    if (expired) {
      StrAppend(&json, ",\"expired\":true");
    }
    if (!sub.empty()) {
      // NOTE: "domain" is the subscriber EMAIL (legacy key name, kept for
      // console compatibility). the design record site domain is "site_domain".
      StrAppend(&json, ",\"domain\":\"", JsonEscape(sub), "\"");
    }
    if (!scope.empty()) {
      StrAppend(&json, ",\"scope\":\"", JsonEscape(scope), "\"");
    }
    if (!site_domain.empty()) {
      StrAppend(&json, ",\"site_domain\":\"", JsonEscape(site_domain), "\"");
    }
  }
  StrAppend(&json, "}");
  WriteJsonResponse(fetch, json);
}

void AdminLicenseHandler::HandleApply(StringPiece request_body,
                                      AsyncFetch* fetch) {
  if (request_body.size() > kMaxRequestBodySize) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Request body too large (max 4KB)");
    return;
  }

  // Extract the license key from the JSON body.
  GoogleString key;
  if (!ExtractJsonStringField(request_body, "key", &key) &&
      !ExtractJsonStringField(request_body, "license_key", &key)) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Missing 'key' or 'license_key' field");
    return;
  }

  if (key.empty()) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "License key must not be empty");
    return;
  }

  GoogleString error;
  if (!ApplyToken(key, &error)) {
    GoogleString json =
        StrCat("{\"success\":false,\"error\":\"", JsonEscape(error), "\"}");
    WriteJsonResponse(fetch, json);
    return;
  }

  // Persist to disk (best-effort).
  std::filesystem::path path = LicenseFilePath(cache_path_);
  if (WriteLicenseFile(path, key)) {
    handler_->Message(kInfo, "License saved to %s", path.string().c_str());
  } else {
    handler_->Message(kWarning, "Failed to save license to %s",
                      path.string().c_str());
  }

  GoogleString plan;
  int64_t expires_at;
  GoogleString scope;
  GoogleString site_domain;
  {
    std::lock_guard<std::mutex> lock(license_mu_);
    plan = license_plan_;
    expires_at = license_expires_at_;
    scope = license_scope_;
    site_domain = license_domain_;
  }

  GoogleString json = "{\"success\":true,\"message\":\"License applied\"";
  if (!plan.empty()) {
    StrAppend(&json, ",\"plan\":\"", JsonEscape(plan), "\"");
  }
  if (expires_at > 0) {
    StrAppend(&json, ",\"expires_at\":", Integer64ToString(expires_at));
  }
  if (!scope.empty()) {
    StrAppend(&json, ",\"scope\":\"", JsonEscape(scope), "\"");
  }
  if (!site_domain.empty()) {
    StrAppend(&json, ",\"site_domain\":\"", JsonEscape(site_domain), "\"");
  }
  StrAppend(&json, "}");
  WriteJsonResponse(fetch, json);
}

void AdminLicenseHandler::WriteJsonResponse(AsyncFetch* fetch,
                                            StringPiece json) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->SetStatusAndReason(HttpStatus::kOK);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  headers->Add(HttpAttributes::kContentLength,
               Integer64ToString(static_cast<int64>(json.size())));
  int64 now_ms = timer_->NowMs();
  headers->SetLastModified(now_ms);
  fetch->Write(json, handler_);
  fetch->Done(true);
}

void AdminLicenseHandler::WriteJsonError(AsyncFetch* fetch, int status_code,
                                         StringPiece error) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->set_status_code(status_code);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  GoogleString json = StrCat("{\"error\":\"", JsonEscape(error), "\"}");
  headers->Add(HttpAttributes::kContentLength,
               Integer64ToString(static_cast<int64>(json.size())));
  fetch->Write(json, handler_);
  fetch->Done(true);
}

const GoogleString& AdminLicenseHandler::LicenseServiceUrl() {
  if (license_service_url_override != nullptr) {
    return *license_service_url_override;
  }
  // Allow overriding via environment variable for development/testing.
  static const GoogleString kUrl = []() -> GoogleString {
    const char* env = getenv(kLicenseServiceUrlEnvVar);
    if (env != nullptr && env[0] != '\0') {
      return GoogleString(env);
    }
    return GoogleString(kDefaultLicenseServiceUrl);
  }();
  return kUrl;
}

void AdminLicenseHandler::SetLicenseServiceUrlForTesting(StringPiece url) {
  delete license_service_url_override;
  license_service_url_override = new GoogleString(url.as_string());
}

bool AdminLicenseHandler::AcquireRequestSlot() {
  bool expected = false;
  if (request_in_flight_.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
    // Acquired cleanly.  Record when we took the slot.
    last_request_start_ms_.store(timer_->NowMs(), std::memory_order_release);
    return true;
  }

  // Slot is held.  Check if it has been stuck longer than the timeout.
  int64_t start_ms = last_request_start_ms_.load(std::memory_order_acquire);
  int64_t now_ms = timer_->NowMs();
  if (start_ms > 0 && (now_ms - start_ms) > kRequestTimeoutMs) {
    handler_->Message(
        kWarning,
        "AdminLicenseHandler: request_in_flight_ stuck for %" PRId64
        " ms (>%" PRId64 " ms); force-resetting",
        now_ms - start_ms, kRequestTimeoutMs);
    request_in_flight_.store(false, std::memory_order_release);

    // Retry the CAS after the force-reset.
    expected = false;
    if (request_in_flight_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
      last_request_start_ms_.store(now_ms, std::memory_order_release);
      return true;
    }
  }

  return false;
}

void AdminLicenseHandler::ProxyToLicenseService(
    StringPiece api_path, const GoogleString& sanitized_body,
    bool auto_apply_token, AsyncFetch* fetch) {
  // Rate limiting: atomically check and set in-flight flag to avoid TOCTOU.
  if (!AcquireRequestSlot()) {
    WriteJsonError(fetch, 429, "Rate limited: request already in flight");
    return;
  }

  // Rate limiting: reject if less than 1 second since last request.
  int64_t now_ms = timer_->NowMs();
  int64_t last_ms = last_request_time_ms_.load(std::memory_order_acquire);
  if (last_ms > 0 && (now_ms - last_ms) < kMinRequestIntervalMs) {
    request_in_flight_.store(false, std::memory_order_release);
    WriteJsonError(fetch, 429, "Rate limited: too many requests");
    return;
  }

  if (fetcher_ == nullptr) {
    request_in_flight_.store(false, std::memory_order_release);
    WriteJsonError(fetch, HttpStatus::kBadGateway,
                   "No fetcher available for proxy requests");
    return;
  }

  last_request_time_ms_.store(now_ms, std::memory_order_release);

  GoogleString url = StrCat(LicenseServiceUrl(), api_path);

  LicenseProxyFetch* proxy_fetch =
      new LicenseProxyFetch(fetch, this, auto_apply_token, timer_, handler_);
  proxy_fetch->request_headers()->set_method(RequestHeaders::kPost);
  proxy_fetch->request_headers()->Add(HttpAttributes::kContentType,
                                      kContentTypeJson.mime_type());
  proxy_fetch->request_headers()->set_message_body(sanitized_body);

  fetcher_->Fetch(url, handler_, proxy_fetch);
}

void AdminLicenseHandler::HandleActivate(StringPiece request_body,
                                         AsyncFetch* fetch) {
  if (request_body.size() > kMaxRequestBodySize) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Request body too large (max 4KB)");
    return;
  }

  // Extract and sanitize: "nonce" (required) and "order_ref" (optional).
  GoogleString nonce, order_ref;
  if (!ExtractJsonStringField(request_body, "nonce", &nonce)) {
    WriteJsonError(fetch, HttpStatus::kBadRequest, "Missing 'nonce' field");
    return;
  }
  // order_ref is optional — nonce-only polling doesn't include it.
  ExtractJsonStringField(request_body, "order_ref", &order_ref);

  // Reconstruct sanitized JSON from scratch (including tracking metadata).
  GoogleString sanitized = StrCat("{\"nonce\":\"", JsonEscape(nonce), "\"");
  if (!order_ref.empty()) {
    StrAppend(&sanitized, ",\"order_ref\":\"", JsonEscape(order_ref), "\"");
  }
  StrAppend(&sanitized, ",\"server\":\"", PAGESPEED_SERVER, "\",\"os\":\"",
            PAGESPEED_OS, "\",\"arch\":\"", PAGESPEED_ARCH,
            "\",\"distribution\":\"", PAGESPEED_DISTRIBUTION, "\"}");

  ProxyToLicenseService("/api/activate", sanitized, true /* auto_apply_token */,
                        fetch);
}

void AdminLicenseHandler::MaybeRenew() {
  // Check if renewal is needed: license must be valid with a subscription ID
  // and either expired or within the renewal window.
  GoogleString token;
  GoogleString sid;
  int64_t expires_at;
  bool expired;
  {
    std::lock_guard<std::mutex> lock(license_mu_);
    if (!license_valid_ || license_sid_.empty() || license_expires_at_ == 0) {
      return;  // No valid license with subscription, or no expiry — skip.
    }
    token = license_token_;
    sid = license_sid_;
    expires_at = license_expires_at_;
    expired = license_expired_;
  }

  // Check if within renewal window (7 days before expiry, or already expired).
  int64_t now_sec = timer_->NowMs() / 1000;
  if (!expired && (expires_at - now_sec) > kRenewalWindowSec) {
    return;  // Not within renewal window yet.
  }

  // Rate limit: minimum 1 hour between renewal attempts.
  int64_t now_ms = timer_->NowMs();
  int64_t last_ms = last_renewal_attempt_ms_.load(std::memory_order_acquire);
  if (last_ms > 0 && (now_ms - last_ms) < kMinRenewalIntervalMs) {
    handler_->Message(kInfo, "License renewal skipped: rate limited");
    return;  // Too soon since last attempt.
  }

  // Try to claim the renewal slot (don't block if a proxy request is in flight).
  if (!AcquireRequestSlot()) {
    return;  // Another request in flight — skip this cycle.
  }

  last_renewal_attempt_ms_.store(now_ms, std::memory_order_release);

  if (fetcher_ == nullptr) {
    request_in_flight_.store(false, std::memory_order_release);
    return;
  }

  handler_->Message(kInfo, "Attempting license auto-renewal (sid=%s...)",
                    sid.substr(0, 8).c_str());

  // Build renewal request: POST current token to /api/renew.
  GoogleString sanitized = StrCat(
      "{\"token\":\"", JsonEscape(token), "\",\"server\":\"", PAGESPEED_SERVER,
      "\",\"os\":\"", PAGESPEED_OS, "\",\"arch\":\"", PAGESPEED_ARCH,
      "\",\"distribution\":\"", PAGESPEED_DISTRIBUTION, "\"}");
  GoogleString url = StrCat(LicenseServiceUrl(), "/api/renew");

  // Create a "fire and forget" fetch — no client_fetch to forward to.
  // We use a special subclass that just applies the token on success.
  // Reuse LicenseProxyFetch with a NullAsyncFetch as client.
  // Actually, we need a simpler approach: a self-deleting fetch that
  // applies the renewed token.
  class RenewalFetch : public StringAsyncFetch {
   public:
    RenewalFetch(AdminLicenseHandler* handler, const GoogleString& current_sid,
                 ThreadSystem* thread_system, MessageHandler* message_handler)
        : StringAsyncFetch(
              RequestContext::NewTestRequestContext(thread_system)),
          handler_(handler),
          current_sid_(current_sid),
          message_handler_(message_handler) {}

    void HandleDone(bool success) override {
      set_success(success);
      set_done(true);

      if (success && response_headers()->status_code() == HttpStatus::kOK) {
        const GoogleString& body = buffer();
        GoogleString token;
        if (ExtractJsonStringField(body, "token", &token) && !token.empty()) {
          // SID guard: verify the renewed token has the same subscription ID.
          LicenseResult result = VerifyLicenseToken(token);
          if (result.valid) {
            if (!result.payload.sid.empty() &&
                result.payload.sid != current_sid_) {
              message_handler_->Message(
                  kWarning,
                  "Renewal returned token with different sid (%s... vs %s...), "
                  "skipping auto-apply (manual key may have been applied)",
                  result.payload.sid.substr(0, 8).c_str(),
                  current_sid_.substr(0, 8).c_str());
            } else {
              GoogleString error;
              if (handler_->ApplyToken(token, &error)) {
                std::filesystem::path path =
                    LicenseFilePath(handler_->cache_path_);
                if (WriteLicenseFile(path, token)) {
                  message_handler_->Message(
                      kInfo, "License auto-renewed and saved to %s",
                      path.string().c_str());
                }
              } else {
                message_handler_->Message(
                    kWarning, "Auto-renewal token failed verification: %s",
                    error.c_str());
              }
            }
          }
        }
      } else {
        message_handler_->Message(kWarning, "License auto-renewal failed");
      }

      handler_->request_in_flight_.store(false, std::memory_order_release);
      delete this;
    }

   private:
    AdminLicenseHandler* handler_;
    GoogleString current_sid_;
    MessageHandler* message_handler_;
  };

  last_request_time_ms_.store(now_ms, std::memory_order_release);

  RenewalFetch* renewal_fetch =
      new RenewalFetch(this, sid, thread_system_, handler_);
  renewal_fetch->request_headers()->set_method(RequestHeaders::kPost);
  renewal_fetch->request_headers()->Add(HttpAttributes::kContentType,
                                        kContentTypeJson.mime_type());
  renewal_fetch->request_headers()->set_message_body(sanitized);

  fetcher_->Fetch(url, handler_, renewal_fetch);
}

void AdminLicenseHandler::HandleConsent(StringPiece request_body,
                                        AsyncFetch* fetch) {
  if (request_body.size() > kMaxRequestBodySize) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Request body too large (max 4KB)");
    return;
  }

  // Extract and sanitize: only "accepted" (boolean) field.
  bool accepted = false;
  if (!ExtractJsonBoolField(request_body, "accepted", &accepted)) {
    WriteJsonError(fetch, HttpStatus::kBadRequest,
                   "Missing or invalid 'accepted' field");
    return;
  }

  GoogleString sanitized =
      StrCat("{\"accepted\":", accepted ? "true" : "false", "}");

  ProxyToLicenseService("/api/consent", sanitized, false /* auto_apply_token */,
                        fetch);
}

}  // namespace net_instaweb
