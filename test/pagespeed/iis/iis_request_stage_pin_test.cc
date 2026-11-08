// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The stage at which the IIS port takes up a request.
//
// IIS decides access in its authentication and authorization stages, and
// everything the module serves (optimized resources, in-place hits from the
// classic cache or the optimizer worker, admin and static pages) must be
// served only after those stages have admitted the request. The module
// therefore registers its request work at RQ_RESOLVE_REQUEST_CACHE, the stage
// IIS runs right after authorization. The registration and the handler sit
// behind the IIS runtime headers, so they are pinned by reading the port
// source as data, the same idiom as iis_request_cleanup_pin_test.cc. The
// ordering itself is checked on a real IIS by the access-rule system test.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Request notifications IIS runs before or during its access decision. None
// of them may carry work of this module.
const char* const kStagesBeforeAccessDecision[] = {
    "RQ_BEGIN_REQUEST",
    "RQ_AUTHENTICATE_REQUEST",
    "RQ_AUTHORIZE_REQUEST",
};

// Handler overrides for those stages, in the whitespace-free form.
const char* const kHandlersBeforeAccessDecision[] = {
    "OnBeginRequest(",        "OnPostBeginRequest(",
    "OnAuthenticateRequest(", "OnPostAuthenticateRequest(",
    "OnAuthorizeRequest(",
};

// Reads a port source file and removes every whitespace byte.
GoogleString ReadStrippedSource(const GoogleString& file_name) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = RunfilePath(StrCat("pagespeed/iis/", file_name));
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  CHECK(!source.empty()) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// Returns the body of the function whose definition starts with `signature`,
// braces included, or an empty string.
GoogleString FunctionBody(const GoogleString& text,
                          const GoogleString& signature) {
  const size_t at = text.find(signature);
  if (at == GoogleString::npos) {
    return "";
  }
  const size_t open = text.find('{', at);
  int depth = 0;
  for (size_t i = open; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}' && --depth == 0) {
      return text.substr(open, i + 1 - open);
    }
  }
  return "";
}

int CountOf(const GoogleString& text, const GoogleString& token) {
  int count = 0;
  for (size_t at = text.find(token); at != GoogleString::npos;
       at = text.find(token, at + 1)) {
    ++count;
  }
  return count;
}

class IisRequestStagePinTest : public testing::Test {};

TEST_F(IisRequestStagePinTest, RequestWorkIsRegisteredAfterAuthorization) {
  const GoogleString dll = ReadStrippedSource("dll_main.cc");
  const GoogleString registration =
      FunctionBody(dll, "HRESULT__stdcallRegisterModule(");
  ASSERT_FALSE(registration.empty())
      << "RegisterModule was not found; this pin needs updating";

  EXPECT_NE(GoogleString::npos,
            registration.find("RQ_SEND_RESPONSE|RQ_RESOLVE_REQUEST_CACHE,"))
      << "the module no longer takes up requests at the stage that follows "
         "IIS authorization";
  EXPECT_NE(GoogleString::npos,
            registration.find("SetPriorityForRequestNotification("
                              "RQ_RESOLVE_REQUEST_CACHE,PRIORITY_ALIAS_FIRST)"))
      << "the module no longer runs first within that stage";
  for (const char* stage : kStagesBeforeAccessDecision) {
    EXPECT_EQ(GoogleString::npos, dll.find(stage))
        << "dll_main.cc names " << stage
        << ": the module would act on a request before IIS has decided "
           "whether the client may have it";
  }
}

TEST_F(IisRequestStagePinTest, TheModuleHandlesNoStageBeforeAuthorization) {
  const GoogleString header = ReadStrippedSource("iis_http_module.h");
  EXPECT_NE(GoogleString::npos, header.find("OnResolveRequestCache("))
      << "the module no longer handles the stage that follows authorization";
  for (const char* handler : kHandlersBeforeAccessDecision) {
    EXPECT_EQ(GoogleString::npos, header.find(handler))
        << "the module declares " << handler
        << " which runs before IIS has decided access";
  }
}

TEST_F(IisRequestStagePinTest, EveryServeIsReachedOnlyFromThatStage) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const GoogleString stage_handler =
      FunctionBody(cc, "IisHttpModule::OnResolveRequestCache(");
  ASSERT_FALSE(stage_handler.empty())
      << "OnResolveRequestCache was not found; this pin needs updating";
  const GoogleString serving =
      FunctionBody(cc, "IisHttpModule::OnBeginRequestPageSpeed(");
  ASSERT_FALSE(serving.empty())
      << "OnBeginRequestPageSpeed was renamed; this pin needs updating";

  // The routing function is entered from the post-authorization handler and
  // from nowhere else: one definition plus that one call.
  EXPECT_NE(GoogleString::npos,
            stage_handler.find("OnBeginRequestPageSpeed(pHttpContext,"))
      << "the post-authorization handler no longer routes the request";
  EXPECT_EQ(2, CountOf(cc, "OnBeginRequestPageSpeed("))
      << "the routing function is entered from a second place";

  // Each serve kind is started from inside the routing function only.
  const char* const kServeCalls[] = {
      "ResourceFetch::Start(",
      "ServeFromIisDaemonSubstrate(pHttpContext,",
      "->FetchInPlaceResource(",
  };
  for (const char* call : kServeCalls) {
    EXPECT_EQ(1, CountOf(serving, call))
        << call << " is no longer started from the routing function";
    EXPECT_EQ(1, CountOf(cc, call))
        << call << " is started from outside the routing function";
  }

  // A request parked by a serve resumes through the completion handler for
  // the same stage.
  const GoogleString completion =
      FunctionBody(cc, "IisHttpModule::OnAsyncCompletion(");
  ASSERT_FALSE(completion.empty());
  EXPECT_NE(GoogleString::npos,
            completion.find("dwNotification==RQ_RESOLVE_REQUEST_CACHE"))
      << "completions of a parked serve are routed for a different stage";
  for (const char* stage : kStagesBeforeAccessDecision) {
    EXPECT_EQ(GoogleString::npos, cc.find(stage))
        << "iis_http_module.cpp names " << stage;
  }
}

}  // namespace
}  // namespace net_instaweb
