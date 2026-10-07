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

#include "pagespeed/nginx/ngx_etag_match.h"

#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(NgxEtagMatchTest, StarMatchesAnyTag) {
  EXPECT_TRUE(NgxIfNoneMatchMatches("*", "\"abc\""));
  EXPECT_TRUE(NgxIfNoneMatchMatches("*", "W/\"abc\""));
  // nginx's rule: the whole field, exactly one byte -- whitespace around
  // the star makes it a list candidate, which cannot match a quoted tag.
  EXPECT_FALSE(NgxIfNoneMatchMatches(" * ", "\"abc\""));
}

TEST(NgxEtagMatchTest, StarInAListDoesNotMatch) {
  // A "*" inside a list is an ordinary candidate: it does not match a
  // quoted tag, and it does not keep the list's real tags from matching.
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"x\", *", "\"abc\""));
  EXPECT_FALSE(NgxIfNoneMatchMatches("*, \"x\"", "\"abc\""));
  EXPECT_TRUE(NgxIfNoneMatchMatches("*, \"abc\"", "\"abc\""));
}

TEST(NgxEtagMatchTest, CommaInsideAQuotedTagDoesNotSplit) {
  // RFC 9110's etagc includes ",": "a,b" is ONE tag.
  EXPECT_TRUE(NgxIfNoneMatchMatches("\"a,b\"", "\"a,b\""));
  EXPECT_TRUE(NgxIfNoneMatchMatches("\"x\", \"a,b\", \"y\"", "\"a,b\""));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"a,b\"", "\"a\""));
}

TEST(NgxEtagMatchTest, ExactMatch) {
  EXPECT_TRUE(NgxIfNoneMatchMatches("\"abc\"", "\"abc\""));
}

TEST(NgxEtagMatchTest, WeakComparisonIgnoresTheWeakPrefixOnBothSides) {
  // Weak comparison: a leading W/ never decides the outcome, on either side.
  EXPECT_TRUE(NgxIfNoneMatchMatches("W/\"abc\"", "\"abc\""));
  EXPECT_TRUE(NgxIfNoneMatchMatches("\"abc\"", "W/\"abc\""));
  EXPECT_TRUE(NgxIfNoneMatchMatches("W/\"abc\"", "W/\"abc\""));
}

TEST(NgxEtagMatchTest, OpaqueTagsCompareByteForByte) {
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"abd\"", "\"abc\""));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"ABC\"", "\"abc\""));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"abc \"", "\"abc\""));
}

TEST(NgxEtagMatchTest, ListWithSpaces) {
  EXPECT_TRUE(NgxIfNoneMatchMatches("\"x\" ,  \"abc\" ,\"y\"", "\"abc\""));
}

TEST(NgxEtagMatchTest, OnlyTheLastListEntryMatches) {
  EXPECT_TRUE(NgxIfNoneMatchMatches("\"x\", \"y\", \"abc\"", "\"abc\""));
}

TEST(NgxEtagMatchTest, NoMatch) {
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"x\", \"y\"", "\"abc\""));
}

TEST(NgxEtagMatchTest, EmptyFieldNeverMatches) {
  EXPECT_FALSE(NgxIfNoneMatchMatches("", "\"abc\""));
  EXPECT_FALSE(NgxIfNoneMatchMatches("  ", "\"abc\""));
}

TEST(NgxEtagMatchTest, UnquotedTagNeverMatches) {
  // A server entity-tag is quoted by definition; an unquoted one is not a
  // tag and must not match even the identical bytes.
  EXPECT_FALSE(NgxIfNoneMatchMatches("abc", "abc"));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"abc\"", "abc"));
}

TEST(NgxEtagMatchTest, UnterminatedTagNeverMatches) {
  // The closing quote is required too: an unterminated tag is not a tag.
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"abc", "\"abc"));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"abc\"", "\"abc"));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"ab", "\"abc\""));
}

TEST(NgxEtagMatchTest, WAloneDoesNotMatch) {
  // "W/" with no tag after it is not an entity-tag, on either side.
  EXPECT_FALSE(NgxIfNoneMatchMatches("W/", "\"abc\""));
  EXPECT_FALSE(NgxIfNoneMatchMatches("\"abc\"", "W/"));
  EXPECT_FALSE(NgxIfNoneMatchMatches("W/", "W/"));
}

}  // namespace
}  // namespace net_instaweb
