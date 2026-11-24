// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "net/instaweb/rewriter/public/served_savings.h"

#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"

namespace net_instaweb {

namespace {

const char kRangeHeader[] = "Range";

enum ServedType { kNoType, kImageType, kCssType, kJavascriptType };

// True for a request whose answer is not a whole body sent to a visitor.
// Walks the attributes instead of looking names up, which would build the
// headers' lookup map on every served response.
bool HasRangeOrIsModuleFetch(const RequestHeaders& request_headers) {
  for (int i = 0, n = request_headers.NumAttributes(); i < n; ++i) {
    const GoogleString& name = request_headers.Name(i);
    if (StringCaseEqual(name, kRangeHeader)) {
      return true;
    }
    if (StringCaseEqual(name, HttpAttributes::kUserAgent) &&
        request_headers.Value(i).find(kModPagespeedSubrequestUserAgent) !=
            GoogleString::npos) {
      return true;
    }
  }
  return false;
}

void Add(ServedType type, int64 original_size, int64 optimized_size,
         RewriteStats* stats) {
  switch (type) {
    case kImageType:
      stats->image_served_original_bytes()->Add(original_size);
      stats->image_served_optimized_bytes()->Add(optimized_size);
      break;
    case kCssType:
      stats->css_served_original_bytes()->Add(original_size);
      stats->css_served_optimized_bytes()->Add(optimized_size);
      break;
    case kJavascriptType:
      stats->javascript_served_original_bytes()->Add(original_size);
      stats->javascript_served_optimized_bytes()->Add(optimized_size);
      break;
    case kNoType:
      break;
  }
}

}  // namespace

ServedSavings::Outcome ServedSavings::Record(const Response& response,
                                             RewriteStats* stats) {
  if (response.nested_driver || response.request_headers == nullptr ||
      response.request_headers->method() != RequestHeaders::kGet ||
      response.status_code != HttpStatus::kOK ||
      HasRangeOrIsModuleFetch(*response.request_headers)) {
    return kNotCounted;
  }

  if (response.size_record != nullptr) {
    const ResponseHeaders::SizeRecord& record = *response.size_record;
    const StringPiece filter_id(record.filter_id);
    const bool size_changed = record.original_size != record.optimized_size;
    ServedType type = kNoType;
    if (filter_id == RewriteOptions::kImageCompressionId) {
      type = kImageType;
    } else if (filter_id == RewriteOptions::kCssFilterId ||
               (filter_id == RewriteOptions::kCssCombinerId && size_changed)) {
      type = kCssType;
    } else if (filter_id == RewriteOptions::kJavascriptMinId ||
               (filter_id == RewriteOptions::kJavascriptCombinerId &&
                size_changed)) {
      type = kJavascriptType;
    }
    if (type == kNoType || record.original_size < 0 ||
        record.optimized_size < 0) {
      return kNotCounted;
    }
    Add(type, record.original_size, record.optimized_size, stats);
    return kCounted;
  }

  // No record: an entry stored before the record existed, or an output that
  // was written without knowing the size of what it was made from.
  if (response.url_filter_id == RewriteOptions::kImageCompressionId) {
    if (response.header_original_size >= 0 &&
        response.uncompressed_body_size >= 0) {
      Add(kImageType, response.header_original_size,
          response.uncompressed_body_size, stats);
      return kCounted;
    }
    stats->served_savings_unmeasured_responses()->Add(1);
    return kUnmeasured;
  }
  if (response.url_filter_id == RewriteOptions::kCssFilterId ||
      response.url_filter_id == RewriteOptions::kJavascriptMinId) {
    stats->served_savings_unmeasured_responses()->Add(1);
    return kUnmeasured;
  }
  return kNotCounted;
}

}  // namespace net_instaweb
