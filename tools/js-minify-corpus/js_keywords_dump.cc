// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2024-2026 We-Amp B.V.
//
// js_keywords_dump: one-shot equivalence dump for the JsKeywords table.
// Prints every keyword row (name, Lookup Type, iterator Type, Flag,
// CanKeywordPrecedeRegEx) in name-sorted order, followed by a fixed probe
// set of non-keywords (near-misses, case variants, the empty string, and
// every keyword with one char appended/removed).
//
// Purpose: when the lookup implementation changes (e.g. the gperf table was
// replaced by a hand-written sorted array), build this binary before and
// after the change and diff the two outputs — they must be byte-identical.
// The output is order-normalized (sorted by name) so it does not depend on
// the iteration order of the underlying implementation.
//
// Usage: bazel run //tools/js-minify-corpus:js_keywords_dump > dump.txt

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/js/js_keywords.h"

using pagespeed::JsKeywords;

namespace {

void DumpProbe(const std::string& name) {
  JsKeywords::Flag flag = JsKeywords::kNone;
  const JsKeywords::Type type = JsKeywords::Lookup(name, &flag);
  std::printf("probe \"%s\" type=%d flag=%d regex=%d\n", name.c_str(),
              static_cast<int>(type), static_cast<int>(flag),
              JsKeywords::CanKeywordPrecedeRegEx(name) ? 1 : 0);
}

}  // namespace

int main() {
  // Collect (name, iterator Type) via the public iterator, then sort by name
  // so the dump is independent of iteration order.
  std::vector<std::pair<std::string, int>> rows;
  for (JsKeywords::Iterator iter; !iter.AtEnd(); iter.Next()) {
    rows.emplace_back(iter.name(), static_cast<int>(iter.keyword()));
  }
  std::sort(rows.begin(), rows.end());

  std::printf("keyword_count %d\n", static_cast<int>(rows.size()));
  for (const auto& row : rows) {
    JsKeywords::Flag flag = JsKeywords::kNone;
    const JsKeywords::Type type = JsKeywords::Lookup(row.first, &flag);
    std::printf("kw \"%s\" type=%d itertype=%d flag=%d regex=%d\n",
                row.first.c_str(), static_cast<int>(type), row.second,
                static_cast<int>(flag),
                JsKeywords::CanKeywordPrecedeRegEx(row.first) ? 1 : 0);
  }

  // Fixed non-keyword probes.
  const char* const kFixedProbes[] = {"awai", "awaits", "Await", "yield_",
                                      "off",  "o",      "",      "zzzz"};
  for (const char* probe : kFixedProbes) {
    DumpProbe(probe);
  }
  // Every keyword with one char appended / removed.
  for (const auto& row : rows) {
    DumpProbe(row.first + "x");
    DumpProbe(row.first.substr(0, row.first.size() - 1));
  }
  return 0;
}
