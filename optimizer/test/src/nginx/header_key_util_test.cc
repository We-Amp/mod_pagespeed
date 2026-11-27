// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/header_key_util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {
namespace {

// Reference: nginx's ngx_hash_key_lc, h = h * 31 + lowercase(c).
std::uintptr_t ReferenceHash(const std::string& lower) {
  std::uintptr_t h = 0;
  for (unsigned char c : lower) h = h * 31 + c;
  return h;
}

TEST(HeaderKeyUtilTest, InjectedHeaderHasLowercaseKeyAndHash) {
  for (std::string_view name : {"If-None-Match", "If-Modified-Since"}) {
    std::string lower(name);
    for (char& c : lower) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    std::string out(name.size(), '\0');
    std::uintptr_t hash =
        LowercaseHeaderKey(name, reinterpret_cast<unsigned char*>(out.data()));
    EXPECT_EQ(out, lower);
    EXPECT_EQ(hash, ReferenceHash(lower));
  }
}

// Pinned values (64-bit ngx_uint_t) so a formula error is caught.
TEST(HeaderKeyUtilTest, PinnedHashes) {
  if (sizeof(std::uintptr_t) != 8) GTEST_SKIP();
  std::string out(17, '\0');
  EXPECT_EQ(LowercaseHeaderKey("If-None-Match",
                               reinterpret_cast<unsigned char*>(out.data())),
            0xa046e4fc26824da0ULL);
  EXPECT_EQ(out.substr(0, 13), "if-none-match");
  EXPECT_EQ(LowercaseHeaderKey("If-Modified-Since",
                               reinterpret_cast<unsigned char*>(out.data())),
            0x51c3e557893731e6ULL);
  EXPECT_EQ(out, "if-modified-since");
}

}  // namespace
}  // namespace pagespeed
