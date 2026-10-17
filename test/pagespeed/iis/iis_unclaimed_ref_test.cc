// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The per-request module holds the reference IisModuleFactory hands out
// with a site's process context. A request context that is created takes
// the reference over and releases it when the request ends; when none is
// created (the site failed to initialize) the module must release it
// itself, or every request to that site pins the context for good.

#include "pagespeed/iis/iis_unclaimed_ref.h"

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class Counted {
 public:
  // One reference for the long-lived owner (the factory's site map).
  Counted() : refs_(1) {}
  void AddRef() { ++refs_; }
  void ReleaseRef() { --refs_; }
  int refs() const { return refs_; }

 private:
  int refs_;
};

// What IisModuleFactory::GetProcessContext does: add a reference for the
// caller.
Counted* HandOut(Counted* c) {
  c->AddRef();
  return c;
}

TEST(UnclaimedRefTest, UnclaimedReferenceIsReleased) {
  Counted context;
  {
    UnclaimedRef<Counted> ref;
    ref.Adopt(HandOut(&context));
    EXPECT_EQ(2, context.refs());
    EXPECT_EQ(&context, ref.get());
  }
  EXPECT_EQ(1, context.refs());
}

TEST(UnclaimedRefTest, ManyFailedRequestsDoNotAccumulateReferences) {
  Counted context;
  for (int i = 0; i < 100; ++i) {
    UnclaimedRef<Counted> ref;
    ref.Adopt(HandOut(&context));
  }
  EXPECT_EQ(1, context.refs());
}

TEST(UnclaimedRefTest, ClaimedReferenceIsLeftToTheNewOwner) {
  Counted context;
  Counted* owned = nullptr;
  {
    UnclaimedRef<Counted> ref;
    ref.Adopt(HandOut(&context));
    owned = ref.Claim();
    EXPECT_EQ(nullptr, ref.get());
  }
  EXPECT_EQ(&context, owned);
  EXPECT_EQ(2, context.refs());
  owned->ReleaseRef();
  EXPECT_EQ(1, context.refs());
}

TEST(UnclaimedRefTest, AdoptReleasesThePreviousReference) {
  Counted first;
  Counted second;
  UnclaimedRef<Counted> ref;
  ref.Adopt(HandOut(&first));
  ref.Adopt(HandOut(&second));
  EXPECT_EQ(1, first.refs());
  EXPECT_EQ(2, second.refs());
  ref.Reset();
  EXPECT_EQ(1, second.refs());
}

// The module keeps the process context reference in an UnclaimedRef, and a
// created request context claims it. The module itself sits behind the IIS
// runtime headers, so this reads its source as data.
TEST(UnclaimedRefTest, TheModuleHoldsItsProcessContextReferenceThisWay) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString header;
  GoogleString source;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/iis/");
  ASSERT_TRUE(file_system.ReadFile(StrCat(dir, "iis_http_module.h").c_str(),
                                   &header, &handler));
  ASSERT_TRUE(file_system.ReadFile(StrCat(dir, "iis_http_module.cpp").c_str(),
                                   &source, &handler));
  EXPECT_NE(GoogleString::npos,
            header.find("UnclaimedRef<IisProcessContext> process_context__;"));
  size_t create = source.find("IisHttpModule::CreateRequestContext(");
  ASSERT_NE(GoogleString::npos, create);
  size_t construct = source.find("new IisModuleRequestContext(", create);
  ASSERT_NE(GoogleString::npos, construct);
  EXPECT_NE(GoogleString::npos,
            source.find("process_context__.Claim();", construct));
}

}  // namespace
}  // namespace net_instaweb
