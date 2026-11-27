// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/header_key_util.h"

namespace pagespeed {

std::uintptr_t LowercaseHeaderKey(std::string_view name, unsigned char* out) {
  std::uintptr_t hash = 0;
  for (std::size_t i = 0; i < name.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    out[i] = c;
    hash = hash * 31 + c;
  }
  return hash;
}

}  // namespace pagespeed
