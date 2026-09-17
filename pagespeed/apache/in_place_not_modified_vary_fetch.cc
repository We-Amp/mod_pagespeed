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

#include "pagespeed/apache/in_place_not_modified_vary_fetch.h"

#include "pagespeed/apache/streaming_pagespeed_resource_fetch.h"
#include "pagespeed/kernel/http/http_names.h"

namespace net_instaweb {

InPlaceNotModifiedVaryFetch::InPlaceNotModifiedVaryFetch(
    const char* request_content_type, bool request_is_head,
    AsyncFetch* base_fetch)
    : SharedAsyncFetch(base_fetch),
      request_content_type_(request_content_type),
      request_is_head_(request_is_head) {}

void InPlaceNotModifiedVaryFetch::HandleHeadersComplete() {
  // ADDED, never REPLACED: a `Vary` this response already carries states
  // axes the origin negotiated on, and the composition appends the encoding
  // axis to that set rather than overwriting it.
  if (response_headers()->status_code() == HttpStatus::kNotModified &&
      !request_is_head_ &&
      StreamingPagespeedResourceFetch::IsCompressibleContentType(
          request_content_type_)) {
    response_headers()->Add(HttpAttributes::kVary,
                            HttpAttributes::kAcceptEncoding);
  }
  SharedAsyncFetch::HandleHeadersComplete();
}

}  // namespace net_instaweb
