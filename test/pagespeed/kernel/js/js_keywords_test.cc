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

#include "pagespeed/kernel/js/js_keywords.h"

#include <map>
#include <string>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

using pagespeed::JsKeywords;

namespace {

struct KeywordRow {
  const char* name;
  JsKeywords::Type type;
  JsKeywords::Flag flag;
};

// The full keyword map: every name Lookup() recognizes, with its Type and
// Flag.  This is the reference copy of the table; a row changed, added or
// dropped in the implementation must show up here as a diff.
const KeywordRow kExpectedKeywords[] = {
    // literals
    {"false", JsKeywords::kFalse, JsKeywords::kIsValue},
    {"null", JsKeywords::kNull, JsKeywords::kIsValue},
    {"true", JsKeywords::kTrue, JsKeywords::kIsValue},
    // keywords that can precede regular expressions
    {"await", JsKeywords::kAwait, JsKeywords::kCanPrecedeRegEx},
    {"case", JsKeywords::kCase, JsKeywords::kCanPrecedeRegEx},
    {"delete", JsKeywords::kDelete, JsKeywords::kCanPrecedeRegEx},
    {"do", JsKeywords::kDo, JsKeywords::kCanPrecedeRegEx},
    {"in", JsKeywords::kIn, JsKeywords::kCanPrecedeRegEx},
    {"instanceof", JsKeywords::kInstanceof, JsKeywords::kCanPrecedeRegEx},
    {"new", JsKeywords::kNew, JsKeywords::kCanPrecedeRegEx},
    {"return", JsKeywords::kReturn, JsKeywords::kCanPrecedeRegEx},
    {"throw", JsKeywords::kThrow, JsKeywords::kCanPrecedeRegEx},
    {"typeof", JsKeywords::kTypeof, JsKeywords::kCanPrecedeRegEx},
    {"void", JsKeywords::kVoid, JsKeywords::kCanPrecedeRegEx},
    {"yield", JsKeywords::kYield, JsKeywords::kCanPrecedeRegEx},
    // keywords that can't precede regular expressions
    {"break", JsKeywords::kBreak, JsKeywords::kNone},
    {"catch", JsKeywords::kCatch, JsKeywords::kNone},
    {"const", JsKeywords::kConst, JsKeywords::kNone},
    {"continue", JsKeywords::kContinue, JsKeywords::kNone},
    {"debugger", JsKeywords::kDebugger, JsKeywords::kNone},
    {"default", JsKeywords::kDefault, JsKeywords::kNone},
    {"else", JsKeywords::kElse, JsKeywords::kNone},
    {"finally", JsKeywords::kFinally, JsKeywords::kNone},
    {"for", JsKeywords::kFor, JsKeywords::kNone},
    {"function", JsKeywords::kFunction, JsKeywords::kNone},
    {"if", JsKeywords::kIf, JsKeywords::kNone},
    {"switch", JsKeywords::kSwitch, JsKeywords::kNone},
    {"this", JsKeywords::kThis, JsKeywords::kNone},
    {"try", JsKeywords::kTry, JsKeywords::kNone},
    {"while", JsKeywords::kWhile, JsKeywords::kNone},
    {"with", JsKeywords::kWith, JsKeywords::kNone},
    {"var", JsKeywords::kVar, JsKeywords::kNone},
    // reserved for future use
    {"class", JsKeywords::kClass, JsKeywords::kIsReservedNonStrict},
    {"enum", JsKeywords::kEnum, JsKeywords::kIsReservedNonStrict},
    {"export", JsKeywords::kExport, JsKeywords::kIsReservedNonStrict},
    {"extends", JsKeywords::kExtends, JsKeywords::kIsReservedNonStrict},
    {"import", JsKeywords::kImport, JsKeywords::kIsReservedNonStrict},
    {"super", JsKeywords::kSuper, JsKeywords::kIsReservedNonStrict},
    // reserved for future use in strict code
    {"implements", JsKeywords::kImplements, JsKeywords::kIsReservedStrict},
    {"interface", JsKeywords::kInterface, JsKeywords::kIsReservedStrict},
    {"let", JsKeywords::kLet, JsKeywords::kIsReservedStrict},
    {"package", JsKeywords::kPackage, JsKeywords::kIsReservedStrict},
    {"private", JsKeywords::kPrivate, JsKeywords::kIsReservedStrict},
    {"protected", JsKeywords::kProtected, JsKeywords::kIsReservedStrict},
    {"public", JsKeywords::kPublic, JsKeywords::kIsReservedStrict},
    {"static", JsKeywords::kStatic, JsKeywords::kIsReservedStrict},
};

TEST(JsKeywordsTest, LookupFindsEveryKeywordWithExactTypeAndFlag) {
  EXPECT_EQ(static_cast<int>(JsKeywords::kNotAKeyword),
            static_cast<int>(arraysize(kExpectedKeywords)));
  for (const KeywordRow& row : kExpectedKeywords) {
    JsKeywords::Flag flag = JsKeywords::kNone;
    const JsKeywords::Type type = JsKeywords::Lookup(row.name, &flag);
    EXPECT_EQ(row.type, type) << row.name;
    EXPECT_EQ(row.flag, flag) << row.name;
    EXPECT_TRUE(JsKeywords::IsAKeyword(type)) << row.name;
    EXPECT_EQ(row.flag == JsKeywords::kCanPrecedeRegEx,
              JsKeywords::CanKeywordPrecedeRegEx(row.name))
        << row.name;
  }
}

TEST(JsKeywordsTest, IteratorWalksExactlyTheKeywordMap) {
  std::map<std::string, JsKeywords::Type> iterated;
  for (JsKeywords::Iterator iter; !iter.AtEnd(); iter.Next()) {
    EXPECT_TRUE(iterated.emplace(iter.name(), iter.keyword()).second)
        << "duplicate: " << iter.name();
  }
  EXPECT_EQ(arraysize(kExpectedKeywords), iterated.size());
  for (const KeywordRow& row : kExpectedKeywords) {
    const auto it = iterated.find(row.name);
    ASSERT_TRUE(it != iterated.end()) << row.name;
    EXPECT_EQ(row.type, it->second) << row.name;
  }
}

TEST(JsKeywordsTest, NonKeywordsAreRejected) {
  const char* const kFixedProbes[] = {"awai", "awaits", "Await", "yield_",
                                      "off",  "o",      "",      "zzzz"};
  std::vector<std::string> probes(kFixedProbes,
                                  kFixedProbes + arraysize(kFixedProbes));
  // Every keyword with one char appended / removed is not a keyword.
  for (const KeywordRow& row : kExpectedKeywords) {
    const std::string name(row.name);
    probes.push_back(name + "x");
    probes.push_back(name.substr(0, name.size() - 1));
  }
  for (const std::string& probe : probes) {
    JsKeywords::Flag flag = JsKeywords::kNone;
    const JsKeywords::Type type = JsKeywords::Lookup(probe, &flag);
    EXPECT_EQ(JsKeywords::kNotAKeyword, type) << "\"" << probe << "\"";
    EXPECT_FALSE(JsKeywords::IsAKeyword(type)) << "\"" << probe << "\"";
    EXPECT_FALSE(JsKeywords::CanKeywordPrecedeRegEx(probe))
        << "\"" << probe << "\"";
  }
}

}  // namespace
