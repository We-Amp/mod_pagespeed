// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The HTML cache's agent class on IIS: classified from the User-Agent (IIS
// has no signature verifier here), counted and kept on the request context
// where that one context is created, for a site whose HTML cache is on; the
// request facts read it from there.  The notifications sit behind the IIS runtime headers, so where it
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

// Every shared vector, classified as the port does.
TEST(IisDaemonHtmlAgentClassTest, EverySharedVector) {
  for (const AgentVector& v : LoadAgentVectors().vectors) {
    EXPECT_EQ(v.expected_class,
              ClassifyDaemonHtmlAgent(v.user_agent,
                                      DaemonHtmlAgentSignature::kNone)
                  .agent_class)
        << "agent_vectors.txt:" << v.line << ": " << v.user_agent;
  }
}

// Classified (a token scan) and counted once, where the request's one
// context is created, and only for a site whose HTML cache is on: no other
// request reads or scans the User-Agent.  A child request finds its
// parent's context and creates none.
TEST(IisDaemonHtmlAgentClassTest, OnlyWhereTheContextIsCreatedWithAnEngine) {
  const GoogleString src = ReadStrippedSource("iis_http_module.cpp");
  const size_t keep =
      src.find("ctx->GetInnerContext()->set_client_conditionals("
               "client_conditionals_);");
  ASSERT_NE(GoogleString::npos, keep);
  const size_t gate = src.find(
      "DaemonHtmlEngine*engine=sc==nullptr?nullptr:"
      "sc->daemon_html_engine(inner->process_context()->site_id());"
      "if(engine!=nullptr){",
      keep);
  ASSERT_NE(GoogleString::npos, gate);
  EXPECT_LT(gate - keep, 800u);
  const size_t read = src.find("GetHeader(HttpHeaderUserAgent,&ua_length);", gate);
  const size_t scan = src.find("ClassifyDaemonHtmlAgent(", gate);
  const size_t count = src.find(
      "CountDaemonHtmlAgentClass(agent_class,&engine->agent_class_counters());",
      gate);
  ASSERT_NE(GoogleString::npos, read);
  ASSERT_NE(GoogleString::npos, scan);
  ASSERT_NE(GoogleString::npos, count);
  EXPECT_LT(read, scan);
  EXPECT_LT(scan, count);
  EXPECT_LT(count - gate, 600u);
  // Nowhere else: the begin-request notification neither reads nor scans.
  for (const char* once :
       {"ClassifyDaemonHtmlAgent(", "CountDaemonHtmlAgentClass(",
        "HttpHeaderUserAgent"}) {
    size_t n = 0;
    for (size_t at = src.find(once); at != GoogleString::npos;
         at = src.find(once, at + 1)) {
      ++n;
    }
    EXPECT_EQ(1u, n) << once;
  }
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
