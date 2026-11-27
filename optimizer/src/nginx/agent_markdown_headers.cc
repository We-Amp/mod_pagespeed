// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/agent_markdown_headers.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

uint64_t EstimateMarkdownTokens(size_t body_bytes) {
  return static_cast<uint64_t>(body_bytes) / 4;
}

std::string MarkdownTokensHeaderValue(size_t body_bytes) {
  return std::to_string(EstimateMarkdownTokens(body_bytes));
}

bool IsValidContentSignalValue(std::string_view value) {
  if (value.empty() || value.size() > kMaxContentSignalBytes) return false;
  for (char c : value) {
    const auto u = static_cast<unsigned char>(c);
    const bool visible = u >= 0x21 && u <= 0x7E;
    const bool blank = u == ' ' || u == '\t';
    if (!visible && !blank) return false;
  }
  const auto is_blank = [](char c) { return c == ' ' || c == '\t'; };
  return !is_blank(value.front()) && !is_blank(value.back());
}

}  // namespace pagespeed
