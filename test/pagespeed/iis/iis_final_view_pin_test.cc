// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The final-view registration and the HTML cache capture on IIS, pinned by
// reading the port source as data (the registration and the notifications
// sit behind the IIS runtime headers), the idiom of
// iis_request_stage_pin_test.cc.  What is pinned:
//
//   * RegisterModule tells the two registrations apart by name, runs the
//     process-wide setup once, and sets the module id every per-request
//     context is kept under from the main registration only;
//   * the final-view registration has one send notification, at
//     PRIORITY_ALIAS_FIRST, and its handler only reads: it reaches the
//     request's state only through the main registration's module context,
//     changes nothing, completes nothing and takes no lock;
//   * the capture's body is fed only from the main registration's send,
//     before that send clears the response for the rewriter, so the
//     module's rewritten output can never be recorded.
//
// The behaviour itself is checked on a real IIS by the rig acceptance
// script under test/system/iis/html_report/.

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

// The body of the function whose definition starts with `signature`, braces
// included, or an empty string.
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

// Member calls the final-view handler may make: reads of the request's
// state and of the response, and the capture's own notes.
const char* const kCallsAllowedInFinalView[] = {
    "GetInnerContext",  "daemon_html_capture", "GetHeadersBeingSent",
    "needs_final_view", "GetResponse",         "GetRawHttpResponse",
    "NoteFinalView",    "GetConnection",       "IsConnected",
    "NoteAborted",
};

class IisFinalViewPinTest : public testing::Test {};

TEST_F(IisFinalViewPinTest, RegisterModuleKeepsTheMainIdToTheMainEntry) {
  const GoogleString dll = ReadStrippedSource("dll_main.cc");
  const GoogleString registration =
      FunctionBody(dll, "HRESULT__stdcallRegisterModule(");
  ASSERT_FALSE(registration.empty())
      << "RegisterModule was not found; this pin needs updating";

  const size_t kind_at =
      registration.find("IisRegistrationKindForName(pModuleInfo->GetName())");
  const size_t noted_at = registration.find("registrations.NoteRegistered(");
  const size_t setup_at =
      registration.find("registrations.BeginProcessSetup()");
  const size_t final_at =
      registration.find("kind==net_instaweb::IisRegistrationKind::kFinalView");
  ASSERT_NE(GoogleString::npos, kind_at);
  ASSERT_NE(GoogleString::npos, noted_at);
  ASSERT_NE(GoogleString::npos, setup_at);
  ASSERT_NE(GoogleString::npos, final_at);
  EXPECT_LT(kind_at, noted_at);
  EXPECT_LT(noted_at, setup_at);
  EXPECT_LT(setup_at, final_at);

  // The process-wide setup is made once, inside the once-only branch.
  EXPECT_EQ(1, CountOf(dll, "newConfigFactory()"));
  const size_t config_at = registration.find("newConfigFactory()");
  EXPECT_LT(setup_at, config_at);
  EXPECT_LT(config_at, final_at);

  // The final-view branch returns before the main registration's work.
  const GoogleString final_branch =
      FunctionBody(registration.substr(final_at),
                   "kind==net_instaweb::IisRegistrationKind::kFinalView");
  ASSERT_FALSE(final_branch.empty());
  EXPECT_NE(
      GoogleString::npos,
      final_branch.find("SetRequestNotifications(newnet_instaweb::"
                        "IisFinalViewModuleFactory(),RQ_SEND_RESPONSE,0)"))
      << "the final-view registration takes more than its send";
  EXPECT_NE(GoogleString::npos,
            final_branch.find("SetPriorityForRequestNotification("
                              "RQ_SEND_RESPONSE,PRIORITY_ALIAS_FIRST)"))
      << "the final-view send no longer runs after IIS's header stages";
  EXPECT_EQ(CountOf(final_branch, "RQ_SEND_RESPONSE"),
            CountOf(final_branch, "RQ_"))
      << "the final-view branch names another notification";
  for (const char* name : {"set_module_id", "SetGlobalNotifications",
                           "IisModuleFactory(", "RQ_RESOLVE_REQUEST_CACHE"}) {
    EXPECT_EQ(GoogleString::npos, final_branch.find(name))
        << "the final-view branch names " << name;
  }

  // Only the main path, after the final-view branch, sets the module id
  // the per-request contexts are kept under, and registers the global
  // notification.
  EXPECT_EQ(1, CountOf(dll, "set_module_id("));
  const size_t id_at = registration.find("set_module_id(pModuleInfo->GetId())");
  ASSERT_NE(GoogleString::npos, id_at);
  EXPECT_LT(final_at + final_branch.size(), id_at);
  const size_t global_at = registration.find("SetGlobalNotifications(");
  ASSERT_NE(GoogleString::npos, global_at);
  EXPECT_LT(final_at + final_branch.size(), global_at);
}

TEST_F(IisFinalViewPinTest, TheFinalViewModuleOnlyReadsAtItsSend) {
  const GoogleString header = ReadStrippedSource("iis_final_view_module.h");
  EXPECT_NE(GoogleString::npos, header.find("OnSendResponse("));
  EXPECT_EQ(1, CountOf(header, "REQUEST_NOTIFICATION_STATUSOn"))
      << "the final-view module handles a notification besides its send";
  EXPECT_EQ(GoogleString::npos, header.find("OnAsyncCompletion("));

  const GoogleString cc = ReadStrippedSource("iis_final_view_module.cpp");
  const GoogleString send =
      FunctionBody(cc, "IisFinalViewModule::OnSendResponse(");
  ASSERT_FALSE(send.empty())
      << "IisFinalViewModule::OnSendResponse was not found; this pin needs "
         "updating";
  // The request's state comes from the main registration's module context
  // and from nowhere else.
  EXPECT_EQ(1, CountOf(send,
                       "IisModuleRequestContext::GetRequestContext("
                       "pHttpContext)"));
  for (const char* name :
       {"GetServerVariable", "GetHeader(", "GetModuleContext",
        "SetModuleContext", "thread_local", "Tls", "Lock",
        "EnterCriticalSection", "mutex"}) {
    EXPECT_EQ(GoogleString::npos, send.find(name))
        << "the final-view send names " << name;
  }
  EXPECT_EQ(GoogleString::npos, cc.find("thread_local"));
  // It continues the pipeline, always.
  EXPECT_EQ(CountOf(send, "return"),
            CountOf(send, "returnRQ_NOTIFICATION_CONTINUE;"));
  // Every member call is on the allowed list: nothing is set, deleted,
  // written, cleared or completed.
  for (size_t at = send.find("->"); at != GoogleString::npos;
       at = send.find("->", at + 2)) {
    const size_t open = send.find('(', at);
    ASSERT_NE(GoogleString::npos, open);
    GoogleString name = send.substr(at + 2, open - at - 2);
    size_t ident = 0;
    while (ident < name.size() &&
           (isalnum(static_cast<unsigned char>(name[ident])) ||
            name[ident] == '_')) {
      ++ident;
    }
    if (ident < name.size()) {
      // A field, not a call: only read, and only the status.
      EXPECT_EQ("StatusCode", name.substr(0, ident));
      EXPECT_NE('=', name[ident]) << "the final-view send writes ->" << name;
      continue;
    }
    bool allowed = false;
    for (const char* call : kCallsAllowedInFinalView) {
      allowed = allowed || name == call;
    }
    EXPECT_TRUE(allowed) << "the final-view send calls ->" << name;
  }
}

TEST_F(IisFinalViewPinTest, TheBodyIsRecordedOnlyBeforeTheResponseIsCleared) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const GoogleString send = FunctionBody(cc, "IisHttpModule::OnSendResponse(");
  ASSERT_FALSE(send.empty());
  const size_t clear_at = send.find("pHttpResponse->Clear();");
  ASSERT_NE(GoogleString::npos, clear_at);
  EXPECT_EQ(1, CountOf(send, "pHttpResponse->Clear();"));
  // Both feeds (as read, and after the inflater) sit where the rewriter is
  // fed, before the clear.
  EXPECT_EQ(2, CountOf(cc, "daemon_html_capture()->AddBody("));
  EXPECT_EQ(2, CountOf(send, "daemon_html_capture()->AddBody("));
  for (size_t at = send.find("daemon_html_capture()->AddBody(");
       at != GoogleString::npos;
       at = send.find("daemon_html_capture()->AddBody(", at + 1)) {
    EXPECT_LT(at, clear_at);
  }
  // The capture starts only once the rewriter has the response.
  const size_t start_at = send.find("StartDaemonHtmlCapture(pHttpContext,");
  ASSERT_NE(GoogleString::npos, start_at);
  EXPECT_LT(send.find("innerContext->set_proxy_fetch(f);"), start_at);
  EXPECT_EQ(2, CountOf(cc, "StartDaemonHtmlCapture("));

  // Nothing else feeds it: not the base fetch that writes the rewritten
  // output, not the final-view registration.
  for (const char* file :
       {"iis_module_base_fetch.cpp", "iis_final_view_module.cpp",
        "iis_request_context.cpp"}) {
    EXPECT_EQ(GoogleString::npos, ReadStrippedSource(file).find("AddBody("))
        << file << " feeds the capture's body";
  }

  // The module view is taken once, at the end of the module's own header
  // edits.
  const GoogleString fetch = ReadStrippedSource("iis_module_base_fetch.cpp");
  const GoogleString collect =
      FunctionBody(fetch, "intIisModuleBaseFetch::CollectHeaders()");
  ASSERT_FALSE(collect.empty());
  EXPECT_EQ(1, CountOf(fetch, "NoteDaemonHtmlModuleView("));
  EXPECT_EQ(1, CountOf(collect, "ctx_->NoteDaemonHtmlModuleView(r);"));
}

}  // namespace
}  // namespace net_instaweb
