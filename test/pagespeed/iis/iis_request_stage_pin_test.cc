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

// Request notifications IIS runs during its access decision. None of them may
// carry work of this module.
const char* const kStagesBeforeAccessDecision[] = {
    "RQ_AUTHENTICATE_REQUEST",
    "RQ_AUTHORIZE_REQUEST",
};

// Handler overrides for those stages, in the whitespace-free form.
const char* const kHandlersBeforeAccessDecision[] = {
    "OnPostBeginRequest(",
    "OnAuthenticateRequest(",
    "OnPostAuthenticateRequest(",
    "OnAuthorizeRequest(",
};

// RQ_BEGIN_REQUEST, before the access decision, carries exactly one step:
// removing an entity tag this module minted from the request's conditionals.
// Its handler may make only these member calls; anything else (serving,
// configuration, module contexts) is a change to this pin.
const char* const kCallsAllowedInBeginRequest[] = {
    "GetRequest",
    "GetHeader",
    "DeleteHeader",
    "GetResponse",
    "SetStatus",
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

TEST_F(IisRequestStagePinTest, BeginRequestOnlyStripsModuleEntityTags) {
  const GoogleString dll = ReadStrippedSource("dll_main.cc");
  const GoogleString registration =
      FunctionBody(dll, "HRESULT__stdcallRegisterModule(");
  ASSERT_FALSE(registration.empty());
  EXPECT_NE(GoogleString::npos,
            registration.find(
                "RQ_BEGIN_REQUEST|RQ_SEND_RESPONSE|RQ_RESOLVE_REQUEST_CACHE,"))
      << "the conditional strip is no longer registered";
  EXPECT_NE(GoogleString::npos,
            registration.find("SetPriorityForRequestNotification("
                              "RQ_BEGIN_REQUEST,PRIORITY_ALIAS_FIRST)"))
      << "the conditional strip no longer runs first";
  EXPECT_EQ(2, CountOf(dll, "RQ_BEGIN_REQUEST"))
      << "dll_main.cc uses RQ_BEGIN_REQUEST for more than the strip";

  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const GoogleString begin = FunctionBody(cc, "IisHttpModule::OnBeginRequest(");
  ASSERT_FALSE(begin.empty())
      << "OnBeginRequest was not found; this pin needs updating";
  EXPECT_EQ(1, CountOf(begin, "StashClientConditionals("));
  EXPECT_NE(GoogleString::npos, begin.find("DeleteHeader("));
  // One way to end the request (the 500 when a removal fails), one status,
  // and otherwise the request continues to IIS's access decision.
  EXPECT_EQ(1, CountOf(begin, "RQ_NOTIFICATION_FINISH_REQUEST"));
  EXPECT_EQ(1, CountOf(begin, "GetResponse()"));
  EXPECT_EQ(1, CountOf(begin, "SetStatus(500,"));
  EXPECT_EQ(2, CountOf(begin, "return"));
  EXPECT_EQ(1, CountOf(begin, "returnRQ_NOTIFICATION_CONTINUE;"));
  // Every member call is on the allowed list.
  for (size_t at = begin.find("->"); at != GoogleString::npos;
       at = begin.find("->", at + 2)) {
    const size_t open = begin.find('(', at);
    ASSERT_NE(GoogleString::npos, open);
    const GoogleString name = begin.substr(at + 2, open - at - 2);
    bool allowed = false;
    for (const char* call : kCallsAllowedInBeginRequest) {
      allowed = allowed || name == call;
    }
    EXPECT_TRUE(allowed) << "OnBeginRequest calls ->" << name
                         << " before IIS has decided whether the client may "
                            "have the URL";
  }
  // No configuration, context or serving names.
  for (const char* name : {"config", "Config", "context_", "Context(",
                           "OnBeginRequestPageSpeed", "ServeFrom", "Fetch"}) {
    EXPECT_EQ(GoogleString::npos, begin.find(name))
        << "OnBeginRequest names " << name;
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
