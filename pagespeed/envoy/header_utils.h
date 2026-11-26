// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <memory>

#include "absl/strings/numbers.h"
#include "envoy/http/header_map.h"
#include "pagespeed/kernel/http/headers.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/module_entity_tags.h"

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

  // The module entity tag rule (pagespeed/system/module_entity_tags.h) on
  // an Envoy request: fills `stash` with the client's conditionals as they
  // arrived and, when If-None-Match carries a tag this module minted, removes
  // every If-None-Match and If-Modified-Since entry, so neither the router
  // nor a later filter sees them.  Returns whether it removed them.
  static bool StashAndStripClientConditionals(
      Envoy::Http::RequestHeaderMap& headers, ClientConditionalStash* stash) {
    static const Envoy::Http::LowerCaseString kIfNoneMatch("if-none-match");
    static const Envoy::Http::LowerCaseString kIfModifiedSince(
        "if-modified-since");
    const auto collect = [&headers](const Envoy::Http::LowerCaseString& name) {
      StringPieceVector lines;
      const Envoy::Http::HeaderMap::GetResult entries = headers.get(name);
      for (size_t i = 0; i < entries.size(); ++i) {
        lines.push_back(entries[i]->value().getStringView());
      }
      return lines;
    };
    if (!StashClientConditionals(collect(kIfNoneMatch),
                                 collect(kIfModifiedSince), stash)) {
      return false;
    }
    headers.remove(kIfNoneMatch);
    headers.remove(kIfModifiedSince);
    return true;
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