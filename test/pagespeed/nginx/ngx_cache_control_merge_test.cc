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

#include "pagespeed/nginx/ngx_cache_control_merge.h"

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// What the module writes on rewritten HTML when it sets its own line.
const char kModuleValue[] = "max-age=0, no-cache";

// Notes every line in turn, as the module does over the response's
// Cache-Control lines, and returns the line the module writes.
GoogleString Merge(StringPiece module_value, const StringPieceVector& lines) {
  NgxCacheControlLimits limits;
  for (StringPiece line : lines) {
    NgxNoteCacheControlLimits(line, &limits);
  }
  return NgxKeepCacheControlLimits(module_value, limits);
}

TEST(NgxCacheControlMergeTest, NoStoreIsKept) {
  EXPECT_EQ("max-age=0, no-cache, no-store",
            Merge(kModuleValue, {"no-store"}));
}

TEST(NgxCacheControlMergeTest, PrivateIsKept) {
  EXPECT_EQ("max-age=0, no-cache, private", Merge(kModuleValue, {"private"}));
}

TEST(NgxCacheControlMergeTest, BothOnOneLineAreKept) {
  EXPECT_EQ("max-age=0, no-cache, private, no-store",
            Merge(kModuleValue, {"private, no-store"}));
  EXPECT_EQ("max-age=0, no-cache, private, no-store",
            Merge(kModuleValue, {"no-store, max-age=200, private"}));
}

TEST(NgxCacheControlMergeTest, DirectivesFromSeveralLinesAreKept) {
  // An origin line and an add_header line, or two add_header lines.
  EXPECT_EQ("max-age=0, no-cache, private, no-store",
            Merge(kModuleValue, {"private, max-age=600", "no-store"}));
  EXPECT_EQ("max-age=0, no-cache, private, no-store",
            Merge(kModuleValue, {"no-store", "public", "private"}));
  EXPECT_EQ("max-age=0, no-cache, no-store",
            Merge(kModuleValue,
                  {"max-age=600", "must-revalidate", "no-store"}));
}

TEST(NgxCacheControlMergeTest, NeitherPresentLeavesTheValueUnchanged) {
  EXPECT_EQ(kModuleValue, Merge(kModuleValue, {}));
  EXPECT_EQ(kModuleValue, Merge(kModuleValue, {"public, max-age=600"}));
  EXPECT_EQ(kModuleValue, Merge(kModuleValue, {"no-cache", "max-age=3600"}));
  EXPECT_EQ(kModuleValue, Merge(kModuleValue, {"", " , "}));
}

TEST(NgxCacheControlMergeTest, NotAddedTwice) {
  EXPECT_EQ("max-age=0, no-cache, no-store",
            Merge("max-age=0, no-cache, no-store", {"no-store"}));
  EXPECT_EQ("max-age=0, no-cache, no-store, private",
            Merge("max-age=0, no-cache, no-store", {"private, no-store"}));
}

TEST(NgxCacheControlMergeTest, NamesCompareCaseInsensitively) {
  EXPECT_EQ("max-age=0, no-cache, private, no-store",
            Merge(kModuleValue, {"Private", "NO-STORE"}));
}

TEST(NgxCacheControlMergeTest, QualifiedPrivateCountsAsPrivate) {
  EXPECT_EQ("max-age=0, no-cache, private",
            Merge(kModuleValue, {"private=\"Set-Cookie\""}));
}

TEST(NgxCacheControlMergeTest, OnlyWholeDirectiveNamesMatch) {
  EXPECT_EQ(kModuleValue,
            Merge(kModuleValue, {"x-private", "no-store-ext=1", "privates"}));
}

}  // namespace
}  // namespace net_instaweb
