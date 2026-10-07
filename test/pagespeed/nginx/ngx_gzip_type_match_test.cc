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

// The gzip_types matching rules, tested directly: nginx matches a response
// Content-Type the way ngx_http_test_content_type does -- parameters
// ignored, trailing blanks trimmed, case-insensitively, text/html implied.
// The caller hands the origin's Content-Type in verbatim, so every one of
// those shapes is exercised here.

#include "pagespeed/nginx/ngx_gzip_type_match.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// A list shaped like the module's own (the accessor's single source is the
// gzip setter's array; the matcher is what this test binds).
const char* const kTypes[] = {"application/javascript",
                              "application/json",
                              "application/pdf",
                              "application/postscript",
                              "image/svg+xml",
                              "text/css",
                              "text/csv",
                              "text/plain",
                              "text/xml",
                              nullptr};

TEST(NgxGZipTypeMatchTest, ExactMatch) {
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "text/css"));
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "application/pdf"));
}

TEST(NgxGZipTypeMatchTest, ParametersAreIgnored) {
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "text/csv; charset=utf-8"));
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "text/css;charset=UTF-8"));
}

TEST(NgxGZipTypeMatchTest, CaseIsIgnored) {
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "Application/PDF"));
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "TEXT/CSS"));
}

TEST(NgxGZipTypeMatchTest, TrailingBlanksBeforeParametersAreTrimmed) {
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "text/css ; charset=utf-8"));
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "text/csv\t; charset=utf-8"));
}

TEST(NgxGZipTypeMatchTest, TextHtmlIsAlwaysTrue) {
  // nginx's implied default: compressed whether or not it is listed, and it
  // cannot be unlisted.
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "text/html"));
  EXPECT_TRUE(NgxGZipTypeListMatches(kTypes, "Text/Html; charset=utf-8"));
}

TEST(NgxGZipTypeMatchTest, NullAndEmptyAreFalse) {
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, nullptr));
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, ""));
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, "; charset=utf-8"));
}

TEST(NgxGZipTypeMatchTest, ATypeNotInTheListIsFalse) {
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, "image/png"));
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, "application/wasm"));
  // A parameterised miss is still a miss.
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, "image/png; charset=utf-8"));
}

TEST(NgxGZipTypeMatchTest, APrefixInEitherDirectionIsNotAMatch) {
  // The whole type must equal a whole entry: neither a shorter input nor a
  // longer one matches "text/css".
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, "text/cs"));
  EXPECT_FALSE(NgxGZipTypeListMatches(kTypes, "text/cssx"));
}

TEST(NgxGZipTypeMatchTest, AnEmptyEntryMatchesNothing) {
  const char* const types_with_an_empty_entry[] = {"", "text/css", nullptr};
  EXPECT_FALSE(NgxGZipTypeListMatches(types_with_an_empty_entry, ""));
  EXPECT_FALSE(NgxGZipTypeListMatches(types_with_an_empty_entry, " ; a=b"));
  EXPECT_TRUE(NgxGZipTypeListMatches(types_with_an_empty_entry, "text/css"));
}

}  // namespace
}  // namespace net_instaweb
