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

#include <algorithm>
#include <cstring>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// A spooled body of `total` bytes of 'x', served from offsets.
class FakeBodyFile {
 public:
  explicit FakeBodyFile(int64 total) : total_(total) {}

  NgxRequestBodyReader Reader() {
    return [this](char* buf, size_t size, int64 offset) -> int64 {
      ++reads_;
      if (offset < 0 || offset >= total_) {
        return 0;
      }
      int64 n = std::min<int64>(size, total_ - offset);
      memset(buf, 'x', n);
      return n;
    };
  }

  int reads() const { return reads_; }

 private:
  int64 total_;
  int reads_ = 0;
};

TEST(NgxRequestBodyTest, ReadsSmallBody) {
  FakeBodyFile file(1000);
  GoogleString body;
  EXPECT_EQ(kNgxRequestBodyOk,
            NgxReadRequestBody(file.Reader(), kNgxMaxRequestBodyBytes, &body));
  EXPECT_EQ(GoogleString(1000, 'x'), body);
}

TEST(NgxRequestBodyTest, ReadsBodyOfExactlyTheLimit) {
  FakeBodyFile file(kNgxMaxRequestBodyBytes);
  GoogleString body;
  EXPECT_EQ(kNgxRequestBodyOk,
            NgxReadRequestBody(file.Reader(), kNgxMaxRequestBodyBytes, &body));
  EXPECT_EQ(kNgxMaxRequestBodyBytes, body.size());
}

TEST(NgxRequestBodyTest, OversizedBodyIsRejectedWithBoundedWork) {
  // 64 MiB on disk: the read stops shortly past the limit instead of
  // buffering the whole file.
  FakeBodyFile file(int64{64} << 20);
  GoogleString body;
  EXPECT_EQ(kNgxRequestBodyTooLarge,
            NgxReadRequestBody(file.Reader(), kNgxMaxRequestBodyBytes, &body));
  EXPECT_LE(file.reads(), 4);
  EXPECT_LE(body.size(), kNgxMaxRequestBodyBytes);
}

TEST(NgxRequestBodyTest, ReadErrorIsReported) {
  NgxRequestBodyReader read = [](char*, size_t, int64) -> int64 { return -1; };
  GoogleString body;
  EXPECT_EQ(kNgxRequestBodyReadError,
            NgxReadRequestBody(read, kNgxMaxRequestBodyBytes, &body));
}

TEST(NgxRequestBodyTest, CopyIntoAllocatedBuffer) {
  char storage[16];
  StringPiece out;
  EXPECT_TRUE(NgxCopyRequestBody(
      "hello", [&](size_t n) -> void* { return n <= 16 ? storage : nullptr; },
      &out));
  EXPECT_EQ("hello", out);
  EXPECT_EQ(storage, out.data());
}

TEST(NgxRequestBodyTest, FailedAllocationIsReportedNotWritten) {
  StringPiece out("untouched");
  EXPECT_FALSE(NgxCopyRequestBody(
      "hello", [](size_t) -> void* { return nullptr; }, &out));
  EXPECT_EQ("untouched", out);
}

}  // namespace
}  // namespace net_instaweb
