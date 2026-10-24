// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A replaced site state is shut down on the release thread, never on the
// thread that dropped its last reference (it can be one of that state's own
// worker threads) and never under the factory lock. The module sits behind
// the IIS runtime headers, so this reads its source as data.

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class IisProcessContextReleasePinTest : public testing::Test {
 protected:
  GoogleString Read(const char* name) {
    GoogleString contents;
    const GoogleString path = StrCat(GTestSrcDir(), "/pagespeed/iis/", name);
    EXPECT_TRUE(file_system_.ReadFile(path.c_str(), &contents, &handler_))
        << path;
    return contents;
  }

  // The text from |start| up to the next |end| after it.
  static GoogleString Between(const GoogleString& s, const char* start,
                              const char* end) {
    size_t from = s.find(start);
    EXPECT_NE(GoogleString::npos, from) << start;
    if (from == GoogleString::npos) return GoogleString();
    size_t to = s.find(end, from);
    EXPECT_NE(GoogleString::npos, to) << end;
    if (to == GoogleString::npos) return GoogleString();
    return s.substr(from, to - from);
  }

  StdioFileSystem file_system_;
  NullMessageHandler handler_;
};

TEST_F(IisProcessContextReleasePinTest, ReleaseRefDoesNotShutDownInline) {
  GoogleString header = Read("iis_process_context.h");
  GoogleString body = Between(header, "void ReleaseRef()", "}");
  EXPECT_NE(GoogleString::npos, body.find("ReleaseLast(this)")) << body;
  EXPECT_EQ(GoogleString::npos, body.find("Shutdown")) << body;
  EXPECT_EQ(GoogleString::npos, body.find("delete")) << body;
}

TEST_F(IisProcessContextReleasePinTest, TheReleaseThreadDoesTheShutdown) {
  GoogleString source = Read("iis_process_context.cpp");
  GoogleString body = Between(source, "IisProcessContext::ReleaseLast(",
                              "IisProcessContext::DrainDeferredReleases()");
  EXPECT_NE(GoogleString::npos, body.find("->Post(")) << body;
  EXPECT_NE(GoogleString::npos, body.find("context->Shutdown();")) << body;
}

TEST_F(IisProcessContextReleasePinTest, TheReplacedContextIsReleasedAfterTheLock) {
  GoogleString source = Read("iis_module_factory.cpp");
  GoogleString body = Between(source, "IisModuleFactory::GetProcessContext(",
                              "IisModuleFactory::GetHttpModule(");
  size_t leave = body.rfind("LeaveCriticalSection(&mycs)");
  size_t release = body.find("old_context->ReleaseRef()");
  ASSERT_NE(GoogleString::npos, leave);
  ASSERT_NE(GoogleString::npos, release);
  EXPECT_LT(leave, release)
      << "the replaced context is released while the factory lock is held";
  EXPECT_EQ(release, body.rfind("old_context->ReleaseRef()"));
}

TEST_F(IisProcessContextReleasePinTest, TerminateSkipsNullSlotsAndDrains) {
  GoogleString source = Read("iis_module_factory.cpp");
  GoogleString body = Between(source, "IisModuleFactory::Terminate()",
                              "delete this;");
  size_t null_check = body.find("current.second != nullptr");
  size_t release = body.find("current.second->ReleaseRef()");
  size_t drain = body.find("IisProcessContext::DrainDeferredReleases()");
  size_t del_cs = body.find("DeleteCriticalSection(&mycs)");
  ASSERT_NE(GoogleString::npos, null_check);
  ASSERT_NE(GoogleString::npos, release);
  ASSERT_NE(GoogleString::npos, drain);
  ASSERT_NE(GoogleString::npos, del_cs);
  EXPECT_LT(null_check, release);
  EXPECT_LT(release, drain);
  EXPECT_LT(drain, del_cs);
}

}  // namespace
}  // namespace net_instaweb
