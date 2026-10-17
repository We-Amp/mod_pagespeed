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

#include "pagespeed/nginx/ngx_fetch_body.h"

namespace net_instaweb {

NgxFetchBodyStep NgxFetchAcceptBody(bool content_length_known,
                                    int64 content_length, int64 bytes_received,
                                    size_t available) {
  NgxFetchBodyStep step;
  step.accept = available;
  step.complete = false;
  step.overflow = false;
  if (!content_length_known) {
    return step;
  }
  // Bytes past the announced length are never part of the response.
  int64 remaining = content_length - bytes_received;
  if (remaining < 0) {
    remaining = 0;
  }
  if (static_cast<uint64>(remaining) < available) {
    step.accept = static_cast<size_t>(remaining);
    step.overflow = true;
  }
  step.complete =
      bytes_received + static_cast<int64>(step.accept) == content_length;
  return step;
}

bool NgxFetchResponseHasNoBody(int status_code, bool head_request) {
  return head_request || status_code == 304 || status_code == 204;
}

bool NgxFetchCompleteOnClose(bool headers_complete, bool content_length_known,
                             int64 content_length, int64 bytes_received) {
  // An announced length is a promise: a body that closes short of it (or
  // ran past it) is not a complete response.
  if (content_length_known) {
    return headers_complete && content_length == bytes_received;
  }
  return headers_complete;
}

}  // namespace net_instaweb
