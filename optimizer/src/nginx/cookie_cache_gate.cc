// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/cookie_cache_gate.h"

namespace pagespeed {

CookieShareClass StoredEntryCookieClass(std::string_view origin_content_type,
                                        ContentType stored_type) {
  if (!origin_content_type.empty()) {
    return CookieShareClassForContentType(origin_content_type);
  }
  switch (stored_type) {
    case ContentType::kCss:
    case ContentType::kJs:
    case ContentType::kImage:
      return CookieShareClass::kStaticAsset;
    default:
      return CookieShareClass::kPage;
  }
}

bool CookieCacheGateAllowsStore(bool request_has_cookie,
                                std::string_view content_type,
                                uint16_t origin_cc_flags,
                                uint32_t origin_max_age,
                                uint32_t origin_s_maxage) {
  return CookieRequestMayStore(
      request_has_cookie, CookieShareClassForContentType(content_type),
      HtmlExplicitlyShared(origin_cc_flags, origin_s_maxage),
      OriginStatesLifetime(origin_cc_flags, origin_max_age, origin_s_maxage));
}

bool CookieCacheGateAllowsServe(bool request_has_cookie,
                                CookieShareClass entry_class,
                                uint16_t origin_cc_flags,
                                uint32_t origin_s_maxage) {
  return CookieRequestMayServe(
      request_has_cookie, entry_class,
      HtmlExplicitlyShared(origin_cc_flags, origin_s_maxage));
}

}  // namespace pagespeed
