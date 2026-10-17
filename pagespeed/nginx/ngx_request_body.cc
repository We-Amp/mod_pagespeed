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

#include "pagespeed/nginx/ngx_request_body.h"

#include <cstring>

namespace net_instaweb {

namespace {
const size_t kReadChunkBytes = 65536;
}  // namespace

NgxRequestBodyStatus NgxReadRequestBody(const NgxRequestBodyReader& read,
                                        size_t max_bytes, GoogleString* body) {
  char buf[kReadChunkBytes];
  int64 offset = 0;
  int64 ret;
  while ((ret = read(buf, kReadChunkBytes, offset)) > 0) {
    if (static_cast<uint64>(ret) > kReadChunkBytes) {
      return kNgxRequestBodyReadError;
    }
    // Stop before holding more than max_bytes, rather than spooling the
    // whole file into memory first.
    if (static_cast<uint64>(ret) > max_bytes - body->size()) {
      return kNgxRequestBodyTooLarge;
    }
    body->append(buf, static_cast<size_t>(ret));
    offset += ret;
  }
  if (ret < 0) {
    return kNgxRequestBodyReadError;
  }
  return kNgxRequestBodyOk;
}

bool NgxCopyRequestBody(StringPiece body,
                        const std::function<void*(size_t)>& alloc,
                        StringPiece* out) {
  char* data = static_cast<char*>(alloc(body.size()));
  if (data == nullptr) {
    return false;
  }
  if (!body.empty()) {
    memcpy(data, body.data(), body.size());
  }
  *out = StringPiece(data, body.size());
  return true;
}

}  // namespace net_instaweb
