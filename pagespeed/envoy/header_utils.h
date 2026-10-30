// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <memory>

#include "absl/strings/numbers.h"
#include "envoy/http/header_map.h"
#include "pagespeed/kernel/http/headers.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

class HeaderUtils {
 public:
  // Whether a response to a request with this method may be served from the
  // cache: GET and HEAD only.
  static bool MayServeFromCache(RequestHeaders::Method method) {
    return method == RequestHeaders::kGet || method == RequestHeaders::kHead;
  }

  // Whether the response to a request with this method may be stored in the
  // cache: GET only.
  static bool MayStoreInCache(RequestHeaders::Method method) {
    return method == RequestHeaders::kGet;
  }

  static std::unique_ptr<RequestHeaders> toPageSpeedRequestHeaders(
      Envoy::Http::HeaderMap& headers) {
    std::unique_ptr<RequestHeaders> request_headers =
        std::make_unique<RequestHeaders>();
    auto callback = [&request_headers](const Envoy::Http::HeaderEntry& entry)
        -> Envoy::Http::HeaderMap::Iterate {
      if (entry.key().getStringView() == ":method") {
        request_headers->set_method(
            RequestHeaders::MethodFromString(entry.value().getStringView()));
      }
      request_headers->Add(entry.key().getStringView(),
                           entry.value().getStringView());
      return Envoy::Http::HeaderMap::Iterate::Continue;
    };
    headers.iterate(callback);
    return request_headers;
  }

  static std::unique_ptr<ResponseHeaders> toPageSpeedResponseHeaders(
      Envoy::Http::HeaderMap& headers) {
    std::unique_ptr<ResponseHeaders> response_headers =
        std::make_unique<ResponseHeaders>();
    // response_headers->set_major_version(r->http_version / 1000);
    // response_headers->set_minor_version(r->http_version % 1000);
    headers.iterate([&response_headers](const Envoy::Http::HeaderEntry& entry)
                        -> Envoy::Http::HeaderMap::Iterate {
      auto key = entry.key().getStringView();
      auto value = entry.value().getStringView();

      if (key == ":status") {
        int status_code;
        if (absl::SimpleAtoi(value, &status_code)) {
          // XXX(oschaaf): safety
          auto code = static_cast<net_instaweb::HttpStatus::Code>(status_code);
          response_headers->set_status_code(code);
          response_headers->set_reason_phrase(
              net_instaweb::HttpStatus::GetReasonPhrase(code));
        } else {
          // XXX(oschaaf)
        }
      } else {
        response_headers->Add(entry.key().getStringView(), value);
      }
      return Envoy::Http::HeaderMap::Iterate::Continue;
    });
    response_headers->ComputeCaching();

    return response_headers;
  }
};

}  // namespace net_instaweb