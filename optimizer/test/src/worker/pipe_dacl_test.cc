// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Windows: the worker's pipe access rules. Clients may read and write, the
// worker's own account keeps full control, and the worker can still add the
// pipe's further instances once the rules are on it.

#include "src/worker/pipe_dacl.h"

#include <gtest/gtest.h>

#ifdef _WIN32
// clang-format off
// windows.h MUST precede the security headers, which depend on it; keep
// this order (clang-format would otherwise sort windows.h last).
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <string>
#endif

namespace pagespeed {
namespace {

#ifdef _WIN32

// FILE_CREATE_PIPE_INSTANCE shares its value with FILE_APPEND_DATA.
constexpr ACCESS_MASK kCreatePipeInstance = FILE_CREATE_PIPE_INSTANCE;

struct Descriptor {
  PSECURITY_DESCRIPTOR sd = nullptr;
  PACL dacl = nullptr;
  ~Descriptor() {
    if (sd != nullptr) LocalFree(sd);
  }
};

bool Parse(const std::string& sddl, Descriptor* out) {
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
          sddl.c_str(), SDDL_REVISION_1, &out->sd, nullptr)) {
    return false;
  }
  BOOL present = FALSE, defaulted = FALSE;
  return GetSecurityDescriptorDacl(out->sd, &present, &out->dacl, &defaulted) &&
         present && out->dacl != nullptr;
}

// The mask of the first allow entry for `sid`, or 0.
ACCESS_MASK MaskFor(PACL dacl, PSID sid) {
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* ace = nullptr;
    if (!GetAce(dacl, i, &ace)) continue;
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    if (allowed->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
    if (EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sid)) {
      return allowed->Mask;
    }
  }
  return 0;
}

TEST(PipeDaclTest, ClientsReadAndWriteButCannotAddInstances) {
  Descriptor d;
  ASSERT_TRUE(Parse(OpenPipeSddl(), &d)) << OpenPipeSddl();
  BYTE buffer[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(buffer);
  ASSERT_TRUE(
      CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, buffer, &size));
  const ACCESS_MASK clients = MaskFor(d.dacl, buffer);
  EXPECT_EQ(clients, kPipeClientAccessMask);
  EXPECT_EQ(clients & kCreatePipeInstance, 0u);
  EXPECT_EQ(clients & (WRITE_DAC | WRITE_OWNER), 0u);
}

TEST(PipeDaclTest, TheWorkersOwnAccountKeepsFullControl) {
  HANDLE token = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  BYTE user[256];
  DWORD size = sizeof(user);
  const BOOL got = GetTokenInformation(token, TokenUser, user, size, &size);
  CloseHandle(token);
  ASSERT_TRUE(got);
  PSID self = reinterpret_cast<TOKEN_USER*>(user)->User.Sid;
  for (const std::string& sddl : {OpenPipeSddl(), RestrictedPipeSddl()}) {
    Descriptor d;
    ASSERT_TRUE(Parse(sddl, &d)) << sddl;
    EXPECT_EQ(MaskFor(d.dacl, self), static_cast<ACCESS_MASK>(GENERIC_ALL))
        << sddl;
  }
}

TEST(PipeDaclTest, TheWorkerCanStillAddInstancesUnderTheRules) {
  const std::string name = "\\\\.\\pipe\\pagespeed-pipe-dacl-test-" +
                           std::to_string(GetCurrentProcessId());
  HANDLE first = CreateNamedPipeA(
      name.c_str(),
      // WRITE_DAC, as libuv opens its first instance, so the rules can be set.
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | WRITE_DAC,
      PIPE_TYPE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0,
      nullptr);
  ASSERT_NE(first, INVALID_HANDLE_VALUE) << GetLastError();
  Descriptor d;
  ASSERT_TRUE(Parse(OpenPipeSddl(), &d));
  EXPECT_EQ(static_cast<DWORD>(ERROR_SUCCESS),
            SetSecurityInfo(first, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, d.dacl, nullptr));
  HANDLE second = CreateNamedPipeA(
      name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
  const DWORD second_error =
      second == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
  CloseHandle(first);
  EXPECT_NE(second, INVALID_HANDLE_VALUE) << second_error;
}

#else

TEST(PipeDaclTest, WindowsOnly) {
  GTEST_SKIP() << "named pipe access rules are a Windows concept";
}

#endif

}  // namespace
}  // namespace pagespeed
