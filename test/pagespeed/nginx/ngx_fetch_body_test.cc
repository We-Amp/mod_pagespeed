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

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(NgxFetchBodyTest, ShortBodyAtCloseIsNotComplete) {
  // Headers parsed, 100 bytes announced, 50 received, then the origin
  // closes the connection: the response is incomplete.
  NgxFetchBodyStep step = NgxFetchAcceptBody(true, 100, 0, 50);
  EXPECT_EQ(50u, step.accept);
  EXPECT_FALSE(step.complete);
  EXPECT_FALSE(step.overflow);
  EXPECT_FALSE(NgxFetchCompleteOnClose(true, true, 100, 50));
}

TEST(NgxFetchBodyTest, ExactBodyIsComplete) {
  NgxFetchBodyStep step = NgxFetchAcceptBody(true, 100, 60, 40);
  EXPECT_EQ(40u, step.accept);
  EXPECT_TRUE(step.complete);
  EXPECT_FALSE(step.overflow);
  EXPECT_TRUE(NgxFetchCompleteOnClose(true, true, 100, 100));
}

TEST(NgxFetchBodyTest, LongBodyIsCappedAndFlagged) {
  // 100 bytes announced, 90 already received, 30 more read: only 10 belong
  // to the response.
  NgxFetchBodyStep step = NgxFetchAcceptBody(true, 100, 90, 30);
  EXPECT_EQ(10u, step.accept);
  EXPECT_TRUE(step.complete);
  EXPECT_TRUE(step.overflow);
}

TEST(NgxFetchBodyTest, LongBodyAtCloseIsNotComplete) {
  EXPECT_FALSE(NgxFetchCompleteOnClose(true, true, 100, 150));
}

TEST(NgxFetchBodyTest, ZeroLengthBodyRejectsAnyBytes) {
  NgxFetchBodyStep step = NgxFetchAcceptBody(true, 0, 0, 5);
  EXPECT_EQ(0u, step.accept);
  EXPECT_TRUE(step.complete);
  EXPECT_TRUE(step.overflow);
}

TEST(NgxFetchBodyTest, UnknownLengthAcceptsEverythingUntilClose) {
  // No content-length: a close-delimited body (the fetcher sends HTTP/1.0
  // requests, so it does not negotiate chunked framing). Every byte is
  // accepted and completion is decided at close by the headers alone.
  NgxFetchBodyStep step = NgxFetchAcceptBody(false, -1, 1000, 4096);
  EXPECT_EQ(4096u, step.accept);
  EXPECT_FALSE(step.complete);
  EXPECT_FALSE(step.overflow);
  EXPECT_TRUE(NgxFetchCompleteOnClose(true, false, -1, 5096));
  EXPECT_TRUE(NgxFetchCompleteOnClose(true, false, -1, 0));
}

TEST(NgxFetchBodyTest, HeadResponseIsCompleteWithItsHeaders) {
  // A HEAD response with Content-Length: 100 and no body bytes. The close
  // rule alone would reject it (0 of 100 bytes), so the fetcher finishes it
  // as soon as its headers are complete.
  EXPECT_TRUE(NgxFetchResponseHasNoBody(200, true));
  EXPECT_FALSE(NgxFetchCompleteOnClose(true, true, 100, 0));
  // 204 and 304 carry no body for any method; other GET responses do.
  EXPECT_TRUE(NgxFetchResponseHasNoBody(204, false));
  EXPECT_TRUE(NgxFetchResponseHasNoBody(304, false));
  EXPECT_FALSE(NgxFetchResponseHasNoBody(200, false));
}

TEST(NgxFetchBodyTest, IncompleteHeadersAtCloseAreNotComplete) {
  EXPECT_FALSE(NgxFetchCompleteOnClose(false, false, -1, 0));
}

}  // namespace
}  // namespace net_instaweb
