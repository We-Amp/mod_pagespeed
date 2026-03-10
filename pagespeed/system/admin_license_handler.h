// Copyright (c) 2024-2026 We-Amp B.V.

// License HTTP handlers for the admin console.
// Provides /v1/license/status, /v1/license/apply, /v1/license/activate,
// /v1/license/trial, and /v1/license/consent endpoints.

#ifndef PAGESPEED_SYSTEM_ADMIN_LICENSE_HANDLER_H_
#define PAGESPEED_SYSTEM_ADMIN_LICENSE_HANDLER_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class AsyncFetch;
class MessageHandler;
class Timer;

// Manages license state and handles license-related HTTP requests
// from the admin console SPA.
class AdminLicenseHandler {
 public:
  AdminLicenseHandler(Timer* timer, MessageHandler* handler);
  ~AdminLicenseHandler() {}

  // Initialize: attempt to read license from disk.
  void Init();

  // Route a license API request.  Returns true if the path was handled.
  // Paths: /v1/license/status, /v1/license/apply
  bool HandleRequest(StringPiece path, StringPiece request_body,
                     AsyncFetch* fetch);

  // Get the current license token (may be empty).
  const GoogleString& license_token() const { return license_token_; }

  // Check if the current license is valid and not expired.
  bool IsLicenseValid() const;

 private:
  void HandleStatus(AsyncFetch* fetch);
  void HandleApply(StringPiece request_body, AsyncFetch* fetch);

  void WriteJsonResponse(AsyncFetch* fetch, StringPiece json);
  void WriteJsonError(AsyncFetch* fetch, int status_code, StringPiece error);

  // Verify and store a license token.
  bool ApplyToken(StringPiece token, GoogleString* error);

  Timer* timer_;
  MessageHandler* handler_;
  GoogleString license_token_;

  // Cached verification results.
  bool license_valid_ = false;
  bool license_expired_ = false;
  int64_t license_expires_at_ = 0;
  GoogleString license_plan_;
  GoogleString license_sub_;
  int64_t license_iat_ = 0;

  DISALLOW_COPY_AND_ASSIGN(AdminLicenseHandler);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_ADMIN_LICENSE_HANDLER_H_
