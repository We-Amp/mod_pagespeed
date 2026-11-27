// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Windows: the notification client connects at identification level.

#include <gtest/gtest.h>

#ifdef _WIN32
// clang-format off
// windows.h must come first: sddl.h declares its helpers only once
// _WIN32_WINNT is set (clang-format would otherwise sort windows.h last).
#include <windows.h>
#include <sddl.h>
// clang-format on

#include <cstdio>
#include <string>
#include <thread>

#include "src/proto/notification_sender.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/pipe_dacl.h"
#endif

namespace pagespeed {
namespace {

#ifdef _WIN32

TEST(NotificationSenderWinTest, ConnectsAtIdentificationLevel) {
  const std::string name =
      "pagespeed-identity-level-test-" + std::to_string(GetCurrentProcessId());
  const std::string full = "\\\\.\\pipe\\" + name;
  HANDLE server = CreateNamedPipeA(
      full.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
      /*nMaxInstances=*/1, /*nOutBufferSize=*/4096, /*nInBufferSize=*/4096,
      /*nDefaultTimeOut=*/0, /*lpSecurityAttributes=*/nullptr);
  ASSERT_NE(server, INVALID_HANDLE_VALUE) << GetLastError();

  CacheNotification notification{};
  notification.url = "http://example.test/a.css";
  notification.hostname = "example.test";
  notification.scheme = "http";

  // The persistent sender keeps its handle open after the write, so the
  // client is still connected when the server impersonates it.
  NotificationSendResult sent;
  std::thread client(
      [&] { sent = SendNotificationPersistent(name, notification); });

  const BOOL connected = ConnectNamedPipe(server, nullptr);
  const DWORD connect_error = connected ? 0 : GetLastError();
  char buffer[4096];
  DWORD read = 0;
  // A server may impersonate only after it has read from the pipe.
  const BOOL got_data =
      ReadFile(server, buffer, sizeof(buffer), &read, nullptr);
  // Joined before any assertion, so a failure fails the test rather than
  // ending the process with a joinable thread.
  client.join();
  ASSERT_TRUE(connected || connect_error == ERROR_PIPE_CONNECTED);
  ASSERT_TRUE(got_data);
  ASSERT_TRUE(sent.success) << sent.error_message;

  ASSERT_TRUE(ImpersonateNamedPipeClient(server)) << GetLastError();
  HANDLE token = nullptr;
  const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY,
                                      /*OpenAsSelf=*/TRUE, &token);
  SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
  DWORD length = 0;
  const BOOL queried =
      opened && GetTokenInformation(token, TokenImpersonationLevel, &level,
                                    sizeof(level), &length);
  RevertToSelf();
  if (token != nullptr) CloseHandle(token);
  ClosePersistentConnection();
  CloseHandle(server);

  ASSERT_TRUE(opened);
  ASSERT_TRUE(queried);
  EXPECT_EQ(level, SecurityIdentification) << "got " << level;
}

// The worker grants other accounts kPipeClientAccessMask on its pipes. A pipe
// whose only entry grants the current user that mask is reached by this
// process the way a foreign account is: GENERIC_WRITE asks for the pipe
// instance right the mask withholds, the sender's access does not.
TEST(NotificationSenderWinTest, ConnectsToAPipeThatGrantsOnlyTheClientMask) {
  static_assert((kNotificationPipeDesiredAccess & ~kPipeClientAccessMask) == 0,
                "the sender asks for nothing the worker does not grant");
  HANDLE token = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  std::string user(needed, '\0');
  ASSERT_TRUE(
      GetTokenInformation(token, TokenUser, user.data(), needed, &needed));
  CloseHandle(token);
  char* sid_text = nullptr;
  ASSERT_TRUE(ConvertSidToStringSidA(
      reinterpret_cast<PTOKEN_USER>(user.data())->User.Sid, &sid_text));
  char mask[16];
  snprintf(mask, sizeof(mask), "0x%lx", kPipeClientAccessMask);
  const std::string sddl =
      std::string("D:(A;;") + mask + ";;;" + sid_text + ")";
  LocalFree(sid_text);

  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorA(
      sddl.c_str(), SDDL_REVISION_1, &attributes.lpSecurityDescriptor,
      nullptr));
  const std::string name =
      "pagespeed-client-mask-test-" + std::to_string(GetCurrentProcessId());
  const std::string full = "\\\\.\\pipe\\" + name;
  HANDLE server = CreateNamedPipeA(
      full.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
      /*nMaxInstances=*/1, 4096, 4096, 0, &attributes);
  LocalFree(attributes.lpSecurityDescriptor);
  ASSERT_NE(server, INVALID_HANDLE_VALUE) << GetLastError();

  HANDLE generic = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
  if (generic != INVALID_HANDLE_VALUE) {
    CloseHandle(generic);
    CloseHandle(server);
    GTEST_SKIP() << "the pipe's DACL does not bind this process";
  }
  ASSERT_EQ(static_cast<DWORD>(ERROR_ACCESS_DENIED), GetLastError());

  CacheNotification notification{};
  notification.url = "http://example.test/a.css";
  notification.hostname = "example.test";
  notification.scheme = "http";
  const NotificationSendResult sent =
      SendNotificationPersistent(name, notification);
  ClosePersistentConnection();
  CloseHandle(server);
  EXPECT_TRUE(sent.success) << sent.error_message;
}

#else

TEST(NotificationSenderWinTest, WindowsOnly) {
  GTEST_SKIP() << "named pipe identity levels are a Windows concept";
}

#endif

}  // namespace
}  // namespace pagespeed
