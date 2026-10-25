// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The per-site process context's release path.
//
// The module factory keeps one IisProcessContext per site in its site map
// and releases it when the site's pagespeed.config changes on disk (the
// next request after an operator repairs it) or when the application pool
// recycles. A site whose startup failed -- a missing or unwritable
// FileCachePath, a LogDir auto-create failure, the optimizer volume split
// -- leaves the context in the map with GetServerContext() returned early,
// so releasing it must tolerate exactly the state a failed startup left
// behind: Shutdown() null-checks every member it deletes, and a member
// that was never created must read as null, not as whatever the heap
// block happened to contain.
//
// The context sits behind the IIS runtime headers, so no unit test can
// construct one; like the recorder gate seam, this pins the lifecycle as
// text over the port's sources and runs on every platform.

#include <cctype>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class IisProcessContextSeamTest : public testing::Test {
 protected:
  StdioFileSystem file_system_;
  NullMessageHandler handler_;
};

// Returns `source` with every whitespace character removed, so a pin is
// proof against reformatting and against CRLF checkouts on Windows.
GoogleString Compacted(const GoogleString& source) {
  GoogleString compact;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) compact.push_back(c);
  }
  return compact;
}

TEST_F(IisProcessContextSeamTest, EveryOwnedPointerStartsNull) {
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/iis/iis_process_context.h");
  GoogleString header;
  ASSERT_TRUE(file_system_.ReadFile(path.c_str(), &header, &handler_))
      << path;
  // The members Shutdown() may delete, or GetServerContext() may create
  // lazily, all start null at their declarations. message_handler_ and
  // root_options_ are the exceptions the constructor's initializer list
  // always supplies (the next test pins the first of those).
  for (const char* member :
       {"driver_factory_", "fetch_factory_", "server_context_",
        "loggingMutex_", "server_context_mutex_"}) {
    EXPECT_NE(GoogleString::npos, header.find(StrCat(member, " = nullptr;")))
        << path << " does not null-initialize " << member
        << ", so a context whose startup failed releases whatever the heap "
           "block contained instead of skipping it";
  }
}

TEST_F(IisProcessContextSeamTest, ShutdownDeletesOnlyWhatWasCreated) {
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/iis/iis_process_context.cpp");
  GoogleString source;
  ASSERT_TRUE(file_system_.ReadFile(path.c_str(), &source, &handler_))
      << path;
  // Each lazily created member is null-checked before it is deleted, in
  // Shutdown() itself. Positions, not mere presence: a guard that appears
  // only after the delete, or in another function, fails this pin.
  const size_t shutdown = source.find("void IisProcessContext::Shutdown()");
  ASSERT_NE(GoogleString::npos, shutdown)
      << "Shutdown() was renamed; this pin needs updating";
  // The function's closing brace sits at column 0; "\n}" also matches a
  // CRLF checkout on Windows, where this target runs too.
  const size_t shutdown_end = source.find("\n}", shutdown);
  ASSERT_NE(GoogleString::npos, shutdown_end);
  const GoogleString body =
      Compacted(source.substr(shutdown, shutdown_end - shutdown));
  for (const char* member :
       {"driver_factory_", "fetch_factory_", "root_options_",
        "server_context_mutex_"}) {
    const size_t guard = body.find(StrCat("if(", member, "!=nullptr)"));
    ASSERT_NE(GoogleString::npos, guard)
        << path << ": Shutdown() deletes " << member << " unguarded";
    const size_t del = body.find(StrCat("delete", member, ";"));
    ASSERT_NE(GoogleString::npos, del)
        << path << ": Shutdown() no longer deletes " << member;
    EXPECT_LT(guard, del) << path << ": Shutdown() deletes " << member
                          << " before checking it was created";
  }
  // message_handler_ is deleted without a guard, which is safe only
  // because the constructor's initializer list creates it unconditionally;
  // pin that too, so a constructor edit cannot turn that delete into the
  // next crash of this class.
  const GoogleString compact_source = Compacted(source);
  EXPECT_NE(GoogleString::npos,
            compact_source.find("message_handler_(newGoogleMessageHandler())"))
      << path << ": the constructor no longer creates message_handler_ at "
                 "initialization, but Shutdown() deletes it unconditionally";
}

}  // namespace
}  // namespace net_instaweb
