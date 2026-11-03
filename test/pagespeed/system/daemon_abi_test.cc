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

// The ABI floor and the published layout this module binds.  Every cache call
// the in-place arms make is sited, so a library older than 1.12, or a 1.12
// library without the sited cache entry points, is refused at load, and the
// notification parameters are the published 1.12 layout field for field.

#include "pagespeed/system/daemon_abi.h"

#include <cstddef>
#include <cstdlib>
#include <memory>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"

namespace net_instaweb {

namespace {

#ifdef _WIN32
int setenv(const char* name, const char* value, int) {
  return _putenv_s(name, value);
}
int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

class DaemonAbiTest : public testing::Test {
 protected:
  void SetUp() override { unsetenv("PS_STUB_MINOR"); }
  void TearDown() override { unsetenv("PS_STUB_MINOR"); }

  GoogleString error_;
};

TEST_F(DaemonAbiTest, TheFloorIsOneTwelve) {
  EXPECT_EQ(1, kRequiredAbiMajor);
  EXPECT_EQ(12, kRequiredAbiMinor);
}

TEST_F(DaemonAbiTest, ALibraryReportingElevenIsRefused) {
  setenv("PS_STUB_MINOR", "11", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("reports API 1.11")) << error_;
  EXPECT_NE(GoogleString::npos, error_.find("needs 1.12")) << error_;
}

TEST_F(DaemonAbiTest, ALibraryReportingTwelveLoads) {
  setenv("PS_STUB_MINOR", "12", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_EQ(12, abi->VersionMinor());
}

TEST_F(DaemonAbiTest, ATwelveLibraryWithoutTheSitedCallsIsRefused) {
  // It reports the floor and lacks what the floor publishes.  The sited
  // cache calls are bound unconditionally, so the library is refused, naming
  // the first one it lacks; nothing falls back to an unsited call.
  setenv("PS_STUB_MINOR", "12", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub_no_site.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("_site")) << error_;
}

TEST_F(DaemonAbiTest, TheNotificationLayoutIsThePublishedOne) {
  // The size alone would not see two fields trade places.
  EXPECT_EQ(88u, sizeof(PsNotifyParams));
  EXPECT_EQ(0u, offsetof(PsNotifyParams, struct_size));
  EXPECT_EQ(8u, offsetof(PsNotifyParams, url));
  EXPECT_EQ(16u, offsetof(PsNotifyParams, hostname));
  EXPECT_EQ(24u, offsetof(PsNotifyParams, scheme));
  EXPECT_EQ(32u, offsetof(PsNotifyParams, content_type));
  EXPECT_EQ(36u, offsetof(PsNotifyParams, mask));
  EXPECT_EQ(40u, offsetof(PsNotifyParams, agent_request));
  EXPECT_EQ(48u, offsetof(PsNotifyParams, option_context));
  EXPECT_EQ(56u, offsetof(PsNotifyParams, option_context_length));
  EXPECT_EQ(64u, offsetof(PsNotifyParams, option_signature));
  EXPECT_EQ(72u, offsetof(PsNotifyParams, site));
  EXPECT_EQ(80u, offsetof(PsNotifyParams, notify_flags));
}

TEST_F(DaemonAbiTest, TheInitializerIsToldTheFullSize) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  PsNotifyParams params;
  abi->NotifyParamsInit(&params);
  EXPECT_EQ(sizeof(PsNotifyParams), params.struct_size);
  EXPECT_EQ(nullptr, params.site);
  EXPECT_EQ(0u, params.notify_flags);
}

TEST_F(DaemonAbiTest, TheFlagsAreThePublishedBits) {
  EXPECT_EQ(0x1u, kPsNotifyFlagHostVouched);
  EXPECT_EQ(0x2u, kPsNotifyFlagFromModule);
  EXPECT_EQ(64u, kPsSiteIdChars);
}

}  // namespace

}  // namespace net_instaweb
