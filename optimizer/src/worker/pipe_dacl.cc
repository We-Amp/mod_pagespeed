// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/pipe_dacl.h"

#include <string>

#ifdef _WIN32
// clang-format off
// windows.h MUST precede sddl.h, which depends on it; keep this order
// (clang-format would otherwise sort windows.h last).
#include <windows.h>
#include <sddl.h>
// clang-format on

#include <cstdio>
#include <vector>
#endif

namespace pagespeed {

#ifdef _WIN32

namespace {

// The process's own account as an SDDL SID string ("S-1-5-...").
std::string OwnSidString() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  std::string result;
  if (size != 0 &&
      GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPSTR text = nullptr;
    if (ConvertSidToStringSidA(user->User.Sid, &text)) {
      result = text;
      LocalFree(text);
    }
  }
  CloseHandle(token);
  return result;
}

}  // namespace

std::string OpenPipeSddl() {
  const std::string self = OwnSidString();
  if (self.empty()) return {};
  char mask[16];
  std::snprintf(mask, sizeof(mask), "0x%lx", kPipeClientAccessMask);
  return "D:(A;;GA;;;SY)(A;;GA;;;" + self + ")(A;;" + mask + ";;;AU)";
}

std::string RestrictedPipeSddl() {
  const std::string self = OwnSidString();
  if (self.empty()) return {};
  return "D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + self + ")";
}

#else  // !_WIN32

std::string OpenPipeSddl() { return {}; }
std::string RestrictedPipeSddl() { return {}; }

#endif  // _WIN32

}  // namespace pagespeed
