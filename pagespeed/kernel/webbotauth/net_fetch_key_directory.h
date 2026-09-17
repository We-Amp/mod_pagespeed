// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Production KeyDirectoryProvider: a GUARDED wrapper over the engine
// UrlAsyncFetcher that fetches a Web-Bot-Auth JWKS key directory and extracts
// the signer's Ed25519 key. This is the network-trust model and is NET-NEW
// (the engine has no SSRF helper). It is intentionally NOT part of the
// hermetic unit test's dependency closure (the test injects a fake provider);
// it ships in the `:webbotauth_netfetch` target and is wired into the nginx
// server context in a later, default-off pass.
//
// Trust model (all enforced here; fail-closed to kError on any doubt):
//   * HOST ALLOWLIST -- default EMPTY => feature effectively disabled. Only
//     operator-configured https origins of published key-directory hosts are
//     fetched. The directory URL for a keyid comes from operator config, NOT
//     from any request-controlled header.
//   * SSRF GUARD -- https only; reject private (RFC1918), loopback, link-local
//     (incl. 169.254.169.254 metadata), unique-local, multicast, reserved /
//     non-global resolved IPs; no off-allowlist redirects.
//   * RESPONSE SIZE CAP and CONNECT+TOTAL TIMEOUT.
// Every failure -> KeyLookupResult::kError -> Verdict::kUnknown.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_NET_FETCH_KEY_DIRECTORY_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_NET_FETCH_KEY_DIRECTORY_H_

#include <cstdint>
#include <map>
#include <set>

#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/opt/http/request_context.h"

namespace net_instaweb {
namespace webbotauth {

// Returns true iff `ip_literal` (a textual IPv4 or IPv6 address) is safe to
// connect to (i.e. a global unicast address). Rejects private/loopback/
// link-local/unique-local/multicast/reserved. Exposed for unit testing the
// SSRF predicate without a network.
bool IsGloballyRoutableIp(StringPiece ip_literal);

// Returns true iff `url` is an https URL whose host is in `allowlist` (compared
// case-insensitively on scheme+host[:port]). Exposed for testing.
bool IsAllowlistedHttpsUrl(StringPiece url,
                           const std::set<GoogleString>& allow);

class NetFetchKeyDirectory : public KeyDirectoryProvider {
 public:
  struct Options {
    std::set<GoogleString> host_allowlist;  // operator https origins
    std::map<GoogleString, GoogleString>
        host_to_directory_url;  // host->JWKS url
    int64_t fetch_timeout_ms = 2000;
    size_t max_response_bytes = 256 * 1024;  // 256 KiB
    bool follow_redirects = false;           // default: do not follow
  };

  // Does NOT take ownership of fetcher or handler.
  NetFetchKeyDirectory(UrlAsyncFetcher* fetcher, MessageHandler* handler,
                       const RequestContextPtr& request_context,
                       const Options& options)
      : fetcher_(fetcher),
        handler_(handler),
        request_context_(request_context),
        options_(options) {}

  KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                         int64_t now_unix_sec) override;

 private:
  UrlAsyncFetcher* fetcher_;
  MessageHandler* handler_;
  RequestContextPtr request_context_;
  Options options_;
};

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_NET_FETCH_KEY_DIRECTORY_H_
