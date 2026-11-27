// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The HTML cache's agent class on IIS.  The begin-request notification
// classifies the User-Agent (IIS has no signature verifier here); the class
// is counted, and kept on the request context, where that one context is
// created, for a site whose HTML cache is on; the request facts read it from
// there.  The notifications sit behind the IIS runtime headers, so where it
// happens is pinned by reading the port source as data; the classes come
// from the shared vectors.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_agent_class.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/agent_vectors.h"

namespace net_instaweb {
namespace {

GoogleString ReadStrippedSource(const GoogleString& file_name) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = RunfilePath(StrCat("pagespeed/iis/", file_name));
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) out.push_back(c);
  }
  return out;
}

GoogleString Body(const GoogleString& src, const char* signature) {
  const size_t at = src.find(signature);
  if (at == GoogleString::npos) return "";
  const size_t open = src.find('{', at);
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') {
      ++depth;
    } else if (src[i] == '}' && --depth == 0) {
      return src.substr(open, i + 1 - open);
    }
  }
  return "";
}

// Every shared vector, classified as the begin-request notification does.
TEST(IisDaemonHtmlAgentClassTest, EverySharedVector) {
  for (const AgentVector& v : LoadAgentVectors().vectors) {
    EXPECT_EQ(v.expected_class,
              ClassifyDaemonHtmlAgent(v.user_agent,
                                      DaemonHtmlAgentSignature::kNone)
                  .agent_class)
        << "agent_vectors.txt:" << v.line << ": " << v.user_agent;
  }
}

TEST(IisDaemonHtmlAgentClassTest, BeginRequestClassifiesTheUserAgent) {
  const GoogleString body =
      Body(ReadStrippedSource("iis_http_module.cpp"),
           "IisHttpModule::OnBeginRequest(INIHttpContext*pHttpContext,");
  ASSERT_FALSE(body.empty()) << "renamed; update this pin";
  EXPECT_NE(GoogleString::npos,
            body.find("value=request->GetHeader(HttpHeaderUserAgent,&length);"
                      "agent_class_=ClassifyDaemonHtmlAgent("));
  EXPECT_NE(GoogleString::npos,
            body.find("DaemonHtmlAgentSignature::kNone);"));
  // Classified only: no counter moves here.
  EXPECT_EQ(GoogleString::npos, body.find("CountDaemonHtmlAgentClass"));
}

// Counted once, where the request's one context is created; a child
// request finds its parent's context and creates none.
TEST(IisDaemonHtmlAgentClassTest, CountedWhereTheContextIsCreated) {
  const GoogleString src = ReadStrippedSource("iis_http_module.cpp");
  const size_t keep =
      src.find("ctx->GetInnerContext()->set_client_conditionals("
               "client_conditionals_);");
  ASSERT_NE(GoogleString::npos, keep);
  const size_t gate = src.find(
      "sc->daemon_html_engine(inner->process_context()->site_id())!=nullptr)"
      "{inner->set_agent_class(agent_class_.agent_class);"
      "CountDaemonHtmlAgentClass(agent_class_,sc->statistics());}",
      keep);
  ASSERT_NE(GoogleString::npos, gate);
  EXPECT_LT(gate - keep, 400u);
  size_t count = 0;
  for (size_t at = src.find("CountDaemonHtmlAgentClass("); at != GoogleString::npos;
       at = src.find("CountDaemonHtmlAgentClass(", at + 1)) {
    ++count;
  }
  EXPECT_EQ(1u, count);
}

TEST(IisDaemonHtmlAgentClassTest, TheFactsReadTheKeptClass) {
  const GoogleString body =
      Body(ReadStrippedSource("iis_http_module.cpp"),
           "staticvoidBuildIisDaemonHtmlFacts(IHttpContext*http_context,");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(GoogleString::npos,
            body.find("facts->agent_class=inner->agent_class();"));
}

}  // namespace
}  // namespace net_instaweb
