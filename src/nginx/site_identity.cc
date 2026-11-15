// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/site_identity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sha256.hpp"

namespace pagespeed {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void SortAndDedupe(std::vector<std::string>* values) {
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

// Each value carries its length, so no two lists read the same.
void AppendSection(char tag, const std::vector<std::string>& values,
                   std::string* out) {
  for (const std::string& value : values) {
    out->push_back(tag);
    out->append(std::to_string(value.size()));
    out->push_back(':');
    out->append(value);
    out->push_back('\n');
  }
}

}  // namespace

std::string DeriveServerBlockSite(std::vector<std::string> names,
                                  std::vector<std::string> listen_addresses) {
  for (std::string& name : names) {
    if (!name.empty() && name.front() == '~') {
      continue;  // a regular expression keeps its case
    }
    for (char& c : name) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
  }
  SortAndDedupe(&names);
  SortAndDedupe(&listen_addresses);

  std::string input = "nginx-server-block-site-v1\n";
  AppendSection('n', names, &input);
  AppendSection('a', listen_addresses, &input);

  const auto digest = cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(input.data()), input.size()));
  std::string out;
  out.reserve(digest.size() * 2);
  for (const std::byte b : digest) {
    const auto value = static_cast<uint8_t>(b);
    out.push_back(kHexDigits[value >> 4]);
    out.push_back(kHexDigits[value & 0x0F]);
  }
  return out;
}

}  // namespace pagespeed
