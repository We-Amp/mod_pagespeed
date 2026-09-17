/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/apache/streaming_pagespeed_resource_fetch.h"

#include "net/instaweb/public/global_constants.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/http_names.h"
#include "util_filter.h"  // NOLINT - for ap_add_output_filter

namespace net_instaweb {

StreamingPagespeedResourceFetch::StreamingPagespeedResourceFetch(
    request_rec* request, AsyncFetch* base_fetch)
    : SharedAsyncFetch(base_fetch), request_(request) {}

/* static */
bool StreamingPagespeedResourceFetch::IsCompressibleContentType(
    const char* content_type) {
  if (content_type == nullptr) {
    return false;
  }
  GoogleString type = content_type;
  size_t separator_idx = type.find(';');
  if (separator_idx != GoogleString::npos) {
    type.erase(separator_idx);
  }

  bool res = false;
  if (type.find("text/") == 0) {
    res = true;
  } else if (type.find("application/") == 0) {
    if (type.find("javascript") != type.npos ||
        type.find("json") != type.npos ||
        type.find("ecmascript") != type.npos ||
        type == "application/livescript" || type == "application/js" ||
        type == "application/jscript" || type == "application/x-js" ||
        type == "application/xhtml+xml" || type == "application/xml") {
      res = true;
    }
  }

  return res;
}

void StreamingPagespeedResourceFetch::HandleHeadersComplete() {
  // ResourceFetch adds X-Page-Speed header, old mod_pagespeed code
  // did not. For now, we remove that header for consistency.
  // TODO(sligocki): Consistently use X- headers in MPS and PSOL.
  // I think it would be good to change X-Mod-Pagespeed -> X-Page-Speed
  // and use that for all HTML and resource requests.
  response_headers()->RemoveAll(kPageSpeedHeader);

  if (response_headers()->status_code() == HttpStatus::kOK &&
      IsCompressibleContentType(
          response_headers()->Lookup1(HttpAttributes::kContentType))) {
    // Make sure compression is enabled for this response.
    ap_add_output_filter("DEFLATE", nullptr, request_, request_->connection);
  }
  SharedAsyncFetch::HandleHeadersComplete();
}

}  // namespace net_instaweb
