// Copyright (c) 2024-2026 We-Amp B.V.

// License HTTP handlers for the admin console.
// Provides /v1/license/status, /v1/license/apply, /v1/license/activate,
// /v1/license/trial, and /v1/license/consent endpoints.

#ifndef PAGESPEED_SYSTEM_ADMIN_LICENSE_HANDLER_H_
#define PAGESPEED_SYSTEM_ADMIN_LICENSE_HANDLER_H_

#include <atomic>
#include <functional>
#include <mutex>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class AsyncFetch;
class MessageHandler;
class ThreadSystem;
class Timer;
class UrlAsyncFetcher;

// Manages license state and handles license-related HTTP requests
// from the admin console SPA.
class AdminLicenseHandler {
 public:
  AdminLicenseHandler(Timer* timer, ThreadSystem* thread_system,
                      MessageHandler* handler, UrlAsyncFetcher* fetcher,
                      const GoogleString& cache_path);
  ~AdminLicenseHandler();

  // Initialize: attempt to read license from disk.
  void Init();

  // Route a license API request.  Returns true if the path was handled.
  // Paths: /v1/license/status, /v1/license/apply, /v1/license/activate,
  //        /v1/license/trial, /v1/license/consent
  // Mutation endpoints (apply, activate, trial, consent) require is_global.
  bool HandleRequest(StringPiece path, StringPiece request_body, bool is_global,
                     AsyncFetch* fetch);

  // Get the current license token (may be empty).
  GoogleString license_token() const {
    std::lock_guard<std::mutex> lock(license_mu_);
    return license_token_;
  }

  // Check if the current license is valid and not expired.
  bool IsLicenseValid() const;

  // Set a callback invoked whenever license validity changes.
  // Used by SystemServerContext to update its atomic license_active_ flag.
  void set_license_state_callback(std::function<void(bool)> cb) {
    license_state_callback_ = std::move(cb);
  }

  // Override the license service URL for testing.
  static void SetLicenseServiceUrlForTesting(StringPiece url);

 private:
  void HandleStatus(bool is_global, AsyncFetch* fetch);
  void HandleApply(StringPiece request_body, AsyncFetch* fetch);
  void HandleActivate(StringPiece request_body, AsyncFetch* fetch);
  void HandleTrial(StringPiece request_body, AsyncFetch* fetch);
  void HandleConsent(StringPiece request_body, AsyncFetch* fetch);

  // Attempt license renewal if within renewal window.
  // Called from HandleStatus on each status poll.
  void MaybeRenew();

  // Proxy a POST request to the license service.  |api_path| is the path
  // portion after the base URL (e.g. "/v1/license/activate").
  // |sanitized_body| is the JSON body with only expected fields.
  // If |auto_apply_token| is true, a "token" field in the response will
  // be automatically applied and persisted.
  void ProxyToLicenseService(StringPiece api_path,
                             const GoogleString& sanitized_body,
                             bool auto_apply_token, AsyncFetch* fetch);

  void WriteJsonResponse(AsyncFetch* fetch, StringPiece json);
  void WriteJsonError(AsyncFetch* fetch, int status_code, StringPiece error);

  // Verify and store a license token.
  bool ApplyToken(StringPiece token, GoogleString* error);

  // Returns the license service base URL.
  static const GoogleString& LicenseServiceUrl();

  Timer* timer_;
  ThreadSystem* thread_system_;
  MessageHandler* handler_;
  UrlAsyncFetcher* fetcher_;
  GoogleString cache_path_;  // used to derive license file path

  // Rate limiting for proxy requests.
  std::atomic<bool> request_in_flight_{false};
  std::atomic<int64_t> last_request_time_ms_{0};
  static const int64_t kMinRequestIntervalMs = 1000;

  // Watchdog: timestamp of when request_in_flight_ was last set to true.
  // If a fetch callback never fires, AcquireRequestSlot() will force-reset
  // request_in_flight_ after kRequestTimeoutMs.
  std::atomic<int64_t> last_request_start_ms_{0};
  static const int64_t kRequestTimeoutMs = 60000;  // 60 seconds

  // Atomically acquire the request_in_flight_ slot.  If the slot has been
  // held longer than kRequestTimeoutMs (stuck fetch), force-resets it and
  // retries.  On success, records the current timestamp.  Returns true if
  // the slot was acquired.
  bool AcquireRequestSlot();

  // Rate limiting for auto-renewal (1 hour minimum between attempts).
  std::atomic<int64_t> last_renewal_attempt_ms_{0};
  static const int64_t kMinRenewalIntervalMs = 3600000;  // 1 hour
  // Renew when within 7 days of expiry.
  static const int64_t kRenewalWindowSec = 7 * 24 * 3600;
  static const int64_t kGracePeriodSec = 72 * 3600;  // 72-hour grace period

  // Mutex protecting all license state fields below.
  // INVARIANT: Must NEVER be held across fetcher_->Fetch() calls
  // (risk of deadlock if fetch callback acquires the same mutex).
  mutable std::mutex license_mu_;

  // License state — guarded by license_mu_.
  GoogleString license_token_;      // guarded by license_mu_
  bool license_valid_ = false;      // guarded by license_mu_
  bool license_expired_ = false;    // guarded by license_mu_
  int64_t license_expires_at_ = 0;  // guarded by license_mu_
  GoogleString license_plan_;       // guarded by license_mu_
  GoogleString license_sub_;        // guarded by license_mu_
  GoogleString
      license_sid_;  // guarded by license_mu_ (FastSpring subscription ID)
  int64_t license_iat_ = 0;  // guarded by license_mu_

  // Notify listener of license state changes.
  void NotifyLicenseStateChange();

  // IMPORTANT: Must be set during initialization (PostInitHook) before any
  // requests are served. Not protected by a mutex — relies on happens-before
  // from server startup sequencing. Must not be modified after Init.
  std::function<void(bool)> license_state_callback_;

  friend class LicenseProxyFetch;
  friend class RenewalFetch;

  AdminLicenseHandler(const AdminLicenseHandler&) = delete;
  AdminLicenseHandler& operator=(const AdminLicenseHandler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_ADMIN_LICENSE_HANDLER_H_
