// SPDX-License-Identifier: Apache-2.0
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-js-kernel.sh.
// Canonical source: mod_pagespeed 1.15, pagespeed/kernel/js/js_keywords.cc.
// Synced from the commit pinned in lib/js/JS_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-js-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/js/LICENSE.apache-2.0 for the license text.

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

// Hand-written replacement for the former gperf-generated keyword table
// (js_keywords.gperf): a name-sorted static array searched with
// std::lower_bound.  gperf is deliberately not a build dependency of this
// library anymore.

#include "lib/js/js_keywords.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "lib/js/compat/logging.h"
#include "lib/js/compat/string_util.h"

namespace pagespeed {

namespace {

struct JsKeywordMap {
  const char* name;
  JsKeywords::Type keyword;
  JsKeywords::Flag flag;
};

// Sorted by name; Lookup() binary-searches it.  Every entry's Type must be
// one for which IsAKeyword() is true.
constexpr JsKeywordMap kJsKeywordsTable[] = {
    {"await", JsKeywords::kAwait, JsKeywords::kCanPrecedeRegEx},
    {"break", JsKeywords::kBreak, JsKeywords::kNone},
    {"case", JsKeywords::kCase, JsKeywords::kCanPrecedeRegEx},
    {"catch", JsKeywords::kCatch, JsKeywords::kNone},
    {"class", JsKeywords::kClass, JsKeywords::kIsReservedNonStrict},
    {"const", JsKeywords::kConst, JsKeywords::kNone},
    {"continue", JsKeywords::kContinue, JsKeywords::kNone},
    {"debugger", JsKeywords::kDebugger, JsKeywords::kNone},
    {"default", JsKeywords::kDefault, JsKeywords::kNone},
    {"delete", JsKeywords::kDelete, JsKeywords::kCanPrecedeRegEx},
    {"do", JsKeywords::kDo, JsKeywords::kCanPrecedeRegEx},
    {"else", JsKeywords::kElse, JsKeywords::kNone},
    {"enum", JsKeywords::kEnum, JsKeywords::kIsReservedNonStrict},
    {"export", JsKeywords::kExport, JsKeywords::kIsReservedNonStrict},
    {"extends", JsKeywords::kExtends, JsKeywords::kIsReservedNonStrict},
    {"false", JsKeywords::kFalse, JsKeywords::kIsValue},
    {"finally", JsKeywords::kFinally, JsKeywords::kNone},
    {"for", JsKeywords::kFor, JsKeywords::kNone},
    {"function", JsKeywords::kFunction, JsKeywords::kNone},
    {"if", JsKeywords::kIf, JsKeywords::kNone},
    {"implements", JsKeywords::kImplements, JsKeywords::kIsReservedStrict},
    {"import", JsKeywords::kImport, JsKeywords::kIsReservedNonStrict},
    {"in", JsKeywords::kIn, JsKeywords::kCanPrecedeRegEx},
    {"instanceof", JsKeywords::kInstanceof, JsKeywords::kCanPrecedeRegEx},
    {"interface", JsKeywords::kInterface, JsKeywords::kIsReservedStrict},
    {"let", JsKeywords::kLet, JsKeywords::kIsReservedStrict},
    {"new", JsKeywords::kNew, JsKeywords::kCanPrecedeRegEx},
    {"null", JsKeywords::kNull, JsKeywords::kIsValue},
    {"package", JsKeywords::kPackage, JsKeywords::kIsReservedStrict},
    {"private", JsKeywords::kPrivate, JsKeywords::kIsReservedStrict},
    {"protected", JsKeywords::kProtected, JsKeywords::kIsReservedStrict},
    {"public", JsKeywords::kPublic, JsKeywords::kIsReservedStrict},
    {"return", JsKeywords::kReturn, JsKeywords::kCanPrecedeRegEx},
    {"static", JsKeywords::kStatic, JsKeywords::kIsReservedStrict},
    {"super", JsKeywords::kSuper, JsKeywords::kIsReservedNonStrict},
    {"switch", JsKeywords::kSwitch, JsKeywords::kNone},
    {"this", JsKeywords::kThis, JsKeywords::kNone},
    {"throw", JsKeywords::kThrow, JsKeywords::kCanPrecedeRegEx},
    {"true", JsKeywords::kTrue, JsKeywords::kIsValue},
    {"try", JsKeywords::kTry, JsKeywords::kNone},
    {"typeof", JsKeywords::kTypeof, JsKeywords::kCanPrecedeRegEx},
    {"var", JsKeywords::kVar, JsKeywords::kNone},
    {"void", JsKeywords::kVoid, JsKeywords::kCanPrecedeRegEx},
    {"while", JsKeywords::kWhile, JsKeywords::kNone},
    {"with", JsKeywords::kWith, JsKeywords::kNone},
    {"yield", JsKeywords::kYield, JsKeywords::kCanPrecedeRegEx},
};

constexpr int kNumJsKeywords = static_cast<int>(std::size(kJsKeywordsTable));

// Layout pin: the table must cover exactly the keyword Types, i.e. every
// enumerator below kNotAKeyword.  A Type added or removed in js_keywords.h
// must be reflected here.
static_assert(JsKeywords::kNotAKeyword == 46,
              "JsKeywords::Type layout changed; update kJsKeywordsTable");
static_assert(kNumJsKeywords == JsKeywords::kNotAKeyword,
              "kJsKeywordsTable must have one row per keyword Type");

// Value pins: the numeric value of every enumerator is load-bearing (the
// kernel vendoring design pins enum VALUES, not just cardinality), so a pure
// permutation of Type or Flag must fail to compile, not just reshuffle.
static_assert(JsKeywords::kNull == 0);
static_assert(JsKeywords::kTrue == 1);
static_assert(JsKeywords::kFalse == 2);
static_assert(JsKeywords::kBreak == 3);
static_assert(JsKeywords::kCase == 4);
static_assert(JsKeywords::kCatch == 5);
static_assert(JsKeywords::kConst == 6);
static_assert(JsKeywords::kDefault == 7);
static_assert(JsKeywords::kFinally == 8);
static_assert(JsKeywords::kFor == 9);
static_assert(JsKeywords::kInstanceof == 10);
static_assert(JsKeywords::kNew == 11);
static_assert(JsKeywords::kVar == 12);
static_assert(JsKeywords::kContinue == 13);
static_assert(JsKeywords::kFunction == 14);
static_assert(JsKeywords::kReturn == 15);
static_assert(JsKeywords::kVoid == 16);
static_assert(JsKeywords::kDelete == 17);
static_assert(JsKeywords::kIf == 18);
static_assert(JsKeywords::kThis == 19);
static_assert(JsKeywords::kDo == 20);
static_assert(JsKeywords::kWhile == 21);
static_assert(JsKeywords::kElse == 22);
static_assert(JsKeywords::kIn == 23);
static_assert(JsKeywords::kSwitch == 24);
static_assert(JsKeywords::kThrow == 25);
static_assert(JsKeywords::kTry == 26);
static_assert(JsKeywords::kTypeof == 27);
static_assert(JsKeywords::kWith == 28);
static_assert(JsKeywords::kDebugger == 29);
static_assert(JsKeywords::kClass == 30);
static_assert(JsKeywords::kEnum == 31);
static_assert(JsKeywords::kExport == 32);
static_assert(JsKeywords::kExtends == 33);
static_assert(JsKeywords::kImport == 34);
static_assert(JsKeywords::kSuper == 35);
static_assert(JsKeywords::kImplements == 36);
static_assert(JsKeywords::kInterface == 37);
static_assert(JsKeywords::kLet == 38);
static_assert(JsKeywords::kPackage == 39);
static_assert(JsKeywords::kPrivate == 40);
static_assert(JsKeywords::kProtected == 41);
static_assert(JsKeywords::kPublic == 42);
static_assert(JsKeywords::kStatic == 43);
static_assert(JsKeywords::kYield == 44);
static_assert(JsKeywords::kAwait == 45);

static_assert(JsKeywords::kNone == 0);
static_assert(JsKeywords::kIsValue == 1);
static_assert(JsKeywords::kIsReservedNonStrict == 2);
static_assert(JsKeywords::kIsReservedStrict == 3);
static_assert(JsKeywords::kCanPrecedeRegEx == 4);

constexpr bool KeywordTableIsSorted() {
  for (int i = 1; i < kNumJsKeywords; ++i) {
    if (std::string_view(kJsKeywordsTable[i - 1].name) >=
        std::string_view(kJsKeywordsTable[i].name)) {
      return false;
    }
  }
  return true;
}

static_assert(KeywordTableIsSorted(),
              "kJsKeywordsTable must stay sorted by name");

}  // namespace

JsKeywords::Type JsKeywords::Lookup(const StringPiece& keyword, Flag* flag) {
  const JsKeywordMap* const end = kJsKeywordsTable + kNumJsKeywords;
  const JsKeywordMap* it =
      std::lower_bound(kJsKeywordsTable, end, keyword,
                       [](const JsKeywordMap& entry, const StringPiece& name) {
                         return StringPiece(entry.name) < name;
                       });
  if (it != end && StringPiece(it->name) == keyword) {
    *flag = it->flag;
    return it->keyword;
  }
  return kNotAKeyword;
}

bool JsKeywords::CanKeywordPrecedeRegEx(const StringPiece& name) {
  Flag flag;
  Type type = Lookup(name, &flag);
  return (IsAKeyword(type) && flag == kCanPrecedeRegEx);
}

int JsKeywords::num_keywords() { return kNumJsKeywords; }

bool JsKeywords::Iterator::AtEnd() const { return index_ >= kNumJsKeywords; }

void JsKeywords::Iterator::Next() {
  DCHECK(!AtEnd());
  ++index_;
}

const char* JsKeywords::Iterator::name() const {
  DCHECK(!AtEnd());
  return kJsKeywordsTable[index_].name;
}

JsKeywords::Type JsKeywords::Iterator::keyword() const {
  DCHECK(!AtEnd());
  return kJsKeywordsTable[index_].keyword;
}

}  // namespace pagespeed
