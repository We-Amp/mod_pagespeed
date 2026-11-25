// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The shared HTML cache policy cases: where the file is and the hash that pins
// it. The file's own "about" block documents the row format. A policy runner
// reads the file through these helpers and checks the pin before it runs a
// row, so a runner never asserts a file other than the reviewed one.

#ifndef TEST_LIB_CACHE_DAEMON_HTML_POLICY_CASES_H_
#define TEST_LIB_CACHE_DAEMON_HTML_POLICY_CASES_H_

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace pagespeed {
namespace test {

// Relative to the test's working directory (the runfiles root).
inline constexpr std::string_view kDaemonHtmlPolicyCasesPath =
    "test/lib/cache/daemon_html_policy_cases.json";

// FNV-1a 64 of the file with every carriage return removed. Any edit to the
// file must update this value in the same change.
inline constexpr uint64_t kDaemonHtmlPolicyCasesFnv1a64 = 0xa46d06358ab43123ULL;

// FNV-1a 64 (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
inline uint64_t PolicyCasesFnv1a64(std::string_view data) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// The file's contents with carriage returns removed; empty when unreadable.
inline std::string ReadDaemonHtmlPolicyCases() {
  std::ifstream in(std::string(kDaemonHtmlPolicyCasesPath), std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string contents = buffer.str();
  contents.erase(std::remove(contents.begin(), contents.end(), '\r'),
                 contents.end());
  return contents;
}

}  // namespace test
}  // namespace pagespeed

#endif  // TEST_LIB_CACHE_DAEMON_HTML_POLICY_CASES_H_
