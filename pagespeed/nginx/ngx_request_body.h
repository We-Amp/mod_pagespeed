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
// Loading a client request body that nginx spooled to a temporary file into
// one contiguous buffer, for the beacon and admin POST handlers.
//
// Deliberately nginx-free so it can be unit tested standalone
// (test/pagespeed/nginx): the file read and the pool allocation are passed
// in.

#ifndef NGX_REQUEST_BODY_H_
#define NGX_REQUEST_BODY_H_

#include <cstddef>
#include <functional>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The largest request body the module loads into memory, matching the
// Apache port's limit for the same handlers.
const size_t kNgxMaxRequestBodyBytes = 131072;

enum NgxRequestBodyStatus {
  kNgxRequestBodyOk,
  kNgxRequestBodyReadError,
  kNgxRequestBodyTooLarge,
};

// Reads up to `size` bytes at `offset` into `buf`: returns the number of
// bytes read, 0 at the end of the body, or a negative value on error.
typedef std::function<int64(char* buf, size_t size, int64 offset)>
    NgxRequestBodyReader;

// Reads the whole body through `read` into `*body`, never holding more than
// `max_bytes` of it.
NgxRequestBodyStatus NgxReadRequestBody(const NgxRequestBodyReader& read,
                                        size_t max_bytes, GoogleString* body);

// Copies `body` into memory from `alloc` and points `*out` at it.  Returns
// false, leaving `*out` untouched, when the allocation fails.
bool NgxCopyRequestBody(StringPiece body,
                        const std::function<void*(size_t)>& alloc,
                        StringPiece* out);

}  // namespace net_instaweb

#endif  // NGX_REQUEST_BODY_H_
