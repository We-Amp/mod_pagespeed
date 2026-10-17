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

//
// Body accounting for the native fetcher (NgxFetch): how many newly read
// bytes belong to the response, and whether a response whose connection
// closed is complete.
//
// Deliberately nginx-free so it can be unit tested standalone
// (test/pagespeed/nginx).

#ifndef NGX_FETCH_BODY_H_
#define NGX_FETCH_BODY_H_

#include <cstddef>

#include "pagespeed/kernel/base/basictypes.h"

namespace net_instaweb {

struct NgxFetchBodyStep {
  // How many of the newly read bytes are part of the response body.
  size_t accept;
  // True when the body is now complete.
  bool complete;
  // True when more bytes arrived than the response announced.
  bool overflow;
};

// Accounts for `available` newly read body bytes, given the bytes already
// received.  When the content length is unknown, every byte is accepted
// and the body completes only when the connection closes.
NgxFetchBodyStep NgxFetchAcceptBody(bool content_length_known,
                                    int64 content_length, int64 bytes_received,
                                    size_t available);

// Whether a response carries no body whatever its headers announce: a 204
// or 304, or any response to a HEAD request. Such a response is complete as
// soon as its headers are.
bool NgxFetchResponseHasNoBody(int status_code, bool head_request);

// Whether a response is complete when the origin closes the connection.
// With an announced content length the body must be exactly that long;
// without one, a response whose headers parsed is complete at close.
bool NgxFetchCompleteOnClose(bool headers_complete, bool content_length_known,
                             int64 content_length, int64 bytes_received);

}  // namespace net_instaweb

#endif  // NGX_FETCH_BODY_H_
