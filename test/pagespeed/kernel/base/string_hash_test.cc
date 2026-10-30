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

// Unit-test the string hash functors

#include "pagespeed/kernel/base/string_hash.h"

#include <cstddef>
#include <set>

#include "absl/container/flat_hash_map.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

// The Swiss-table probe tag is the top 7 bits of the hash.
size_t Tag(size_t hash) { return hash >> (sizeof(size_t) * 8 - 7); }

GoogleString Key(int i, bool upper) {
  GoogleString key = StrCat("key", IntegerToString(i));
  if (upper) {
    UpperString(&key);
  }
  return key;
}

}  // namespace

class StringHashTest : public testing::Test {};

TEST_F(StringHashTest, ShortKeysUseManyProbeTags) {
  std::set<size_t> preserve_tags;
  std::set<size_t> fold_tags;
  for (int i = 0; i < 1000; ++i) {
    GoogleString key = Key(i, false);  // at most 6 characters
    ASSERT_LE(key.size(), 8u);
    preserve_tags.insert(Tag(CasePreserveStringHash()(key)));
    fold_tags.insert(Tag(CaseFoldStringHash()(key)));
  }
  EXPECT_GT(preserve_tags.size(), 64u);
  EXPECT_GT(fold_tags.size(), 64u);
}

TEST_F(StringHashTest, FoldHashIgnoresCase) {
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(CaseFoldStringHash()(Key(i, false)),
              CaseFoldStringHash()(Key(i, true)));
  }
  EXPECT_EQ(CaseFoldStringPieceHash()("Content-Type"),
            CaseFoldStringPieceHash()("content-TYPE"));
}

TEST_F(StringHashTest, PreserveHashDistinguishesCase) {
  EXPECT_NE(CasePreserveStringHash()("abc"), CasePreserveStringHash()("ABC"));
}

TEST_F(StringHashTest, CaseInsensitiveMapFindsByAnyCase) {
  absl::flat_hash_map<GoogleString, int, CaseFoldStringHash,
                      CaseFoldStringEqual>
      map;
  for (int i = 0; i < 1000; ++i) {
    map[Key(i, false)] = i;
  }
  EXPECT_EQ(1000u, map.size());
  for (int i = 0; i < 1000; ++i) {
    auto it = map.find(Key(i, true));
    ASSERT_TRUE(it != map.end()) << i;
    EXPECT_EQ(i, it->second);
  }
  EXPECT_TRUE(map.find("missing") == map.end());
}

TEST_F(StringHashTest, CaseSensitiveMapSeparatesCase) {
  absl::flat_hash_map<GoogleString, int, CasePreserveStringHash> map;
  for (int i = 0; i < 1000; ++i) {
    map[Key(i, false)] = i;
    map[Key(i, true)] = -i - 1;
  }
  EXPECT_EQ(2000u, map.size());
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(i, map.find(Key(i, false))->second);
    EXPECT_EQ(-i - 1, map.find(Key(i, true))->second);
  }
}

}  // namespace net_instaweb
