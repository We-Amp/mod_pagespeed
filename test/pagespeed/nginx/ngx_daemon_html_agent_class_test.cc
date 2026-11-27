// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The HTML cache's agent class on nginx.  The verdict reading and the
// classes are unit tested through the nginx-free half (ngx_daemon_html.h);
// where the class is computed and kept sits behind the nginx runtime headers
// (nginx cannot be linked into a test), so it is pinned by reading the module
// source as data.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/classifier.h"
#include "pagespeed/nginx/ngx_daemon_html.h"
#include "pagespeed/system/daemon_html_agent_class.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/agent_vectors.h"

namespace net_instaweb {
namespace {

// As ngx_webbotauth_handler.cc formats a verified bot.
const char kSuffix[] = ", ed25519-verified";

DaemonHtmlAgentSignature Read(bool enabled, const char* verdict) {
  if (verdict == nullptr) {
    return NgxDaemonHtmlAgentSignature(enabled, nullptr, kSuffix);
  }
  const StringPiece value(verdict);
  return NgxDaemonHtmlAgentSignature(enabled, &value, kSuffix);
}

TEST(NgxDaemonHtmlAgentClassTest, TheVerifiersVerdicts) {
  using webbotauth::Verdict;
  using webbotauth::VerdictToken;
  EXPECT_EQ(DaemonHtmlAgentSignature::kVerified,
            Read(true, VerdictToken(Verdict::kSignedAgent)));
  EXPECT_EQ(DaemonHtmlAgentSignature::kVerified,
            Read(true, "Lane Bot, ed25519-verified"));
  EXPECT_EQ(DaemonHtmlAgentSignature::kFailed,
            Read(true, VerdictToken(Verdict::kUnknown)));
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone,
            Read(true, VerdictToken(Verdict::kHuman)));
  EXPECT_EQ(DaemonHtmlAgentSignature::kNotReady, Read(true, nullptr));
  // Off: no verifier, whatever a slot holds.
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone, Read(false, nullptr));
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone,
            Read(false, VerdictToken(Verdict::kSignedAgent)));
  // Only the exact tokens count.
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone, Read(true, "Signed-Agent"));
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone, Read(true, "signed-agent "));
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone, Read(true, ""));
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone, Read(true, "ed25519-verified"));
  // An empty suffix never makes every value verified.
  const StringPiece human("human");
  EXPECT_EQ(DaemonHtmlAgentSignature::kNone,
            NgxDaemonHtmlAgentSignature(true, &human, ""));
}

// Every shared vector under each verdict nginx can read at the slot.
TEST(NgxDaemonHtmlAgentClassTest, EverySharedVectorUnderEachVerdict) {
  for (const AgentVector& v : LoadAgentVectors().vectors) {
    const StringPiece ua = v.user_agent;
    EXPECT_EQ(v.expected_class,
              ClassifyDaemonHtmlAgent(ua, Read(false, nullptr)).agent_class)
        << v.line;
    EXPECT_EQ(v.expected_class,
              ClassifyDaemonHtmlAgent(ua, Read(true, "human")).agent_class)
        << v.line;
    const DaemonHtmlAgentClass not_ready =
        ClassifyDaemonHtmlAgent(ua, Read(true, nullptr));
    EXPECT_EQ(v.expected_class, not_ready.agent_class) << v.line;
    EXPECT_TRUE(not_ready.c1_not_ready);
    const DaemonHtmlAgentClass failed =
        ClassifyDaemonHtmlAgent(ua, Read(true, "unknown"));
    EXPECT_EQ(v.expected_class, failed.agent_class) << v.line;
    EXPECT_TRUE(failed.signature_failed);
    EXPECT_EQ(kDaemonHtmlAgentClassSigned,
              ClassifyDaemonHtmlAgent(ua, Read(true, "signed-agent"))
                  .agent_class)
        << v.line;
  }
}

GoogleString ReadStripped(const char* file) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/pagespeed/nginx/", file);
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

// Kept in a cleanup of the main request's pool, found there before anything
// is computed: an internal redirect (error_page, try_files, index) keeps the
// pool and a subrequest shares it, so the class is computed and counted once
// per client request.
TEST(NgxDaemonHtmlAgentClassTest, KeptOncePerClientRequest) {
  const GoogleString body = Body(ReadStripped("ngx_pagespeed.cc"),
                                 "intps_daemon_html_agent_class(ngx_http_request_t*r)");
  ASSERT_FALSE(body.empty()) << "renamed; update this pin";
  const size_t main = body.find("ngx_http_request_t*main=r->main;");
  const size_t lookup = body.find(
      "for(ngx_pool_cleanup_t*c=main->pool->cleanup;c!=nullptr;c=c->next){"
      "if(c->handler==ps_daemon_html_agent_class_cleanup){return");
  const size_t gate = body.find("if(ps_daemon_html_engine(main)==nullptr)");
  const size_t add = body.find("ngx_pool_cleanup_add(main->pool,");
  const size_t resolve = body.find(
      "slot->Resolve(user_agent,ps_webbotauth_agent_signature(main),");
  ASSERT_NE(GoogleString::npos, main);
  ASSERT_NE(GoogleString::npos, lookup);
  ASSERT_NE(GoogleString::npos, gate);
  ASSERT_NE(GoogleString::npos, add);
  ASSERT_NE(GoogleString::npos, resolve);
  EXPECT_LT(main, lookup);
  EXPECT_LT(lookup, gate);
  EXPECT_LT(gate, add);
  EXPECT_LT(add, resolve);
  // The User-Agent and the verdict are the client request's.
  EXPECT_NE(GoogleString::npos, body.find("main->headers_in.user_agent"));
}

// First computed in the module's phase handler, on the main request; the
// request facts read the same slot.
TEST(NgxDaemonHtmlAgentClassTest, ComputedInThePhaseHandlerAndReadByFacts) {
  const GoogleString src = ReadStripped("ngx_pagespeed.cc");
  const GoogleString phase = Body(
      src, "ngx_int_tps_phase_handler(ngx_http_request_t*r,ngx_http_phase_handler_t*ph)");
  ASSERT_FALSE(phase.empty());
  const size_t call = phase.find("if(r==r->main){ps_daemon_html_agent_class(r);}");
  ASSERT_NE(GoogleString::npos, call);
  EXPECT_LT(call, phase.find("ps_content_handler(r)"));
  const GoogleString facts =
      Body(src, "voidps_daemon_html_request_facts(ngx_http_request_t*r,");
  ASSERT_FALSE(facts.empty());
  EXPECT_NE(GoogleString::npos,
            facts.find("facts->agent_class=ps_daemon_html_agent_class(r);"));
}

// The Web Bot Auth handler runs ahead of the module's phase handler: nginx
// runs one phase's handlers in reverse registration order, and the phase
// handler is registered first.
TEST(NgxDaemonHtmlAgentClassTest, TheVerifierRunsFirst) {
  const GoogleString src = ReadStripped("ngx_pagespeed.cc");
  const size_t preaccess = src.find("*h=ps_preaccess_handler;");
  const size_t verifier = src.find("*wba_h=ps_webbotauth_preaccess_handler;");
  ASSERT_NE(GoogleString::npos, preaccess);
  ASSERT_NE(GoogleString::npos, verifier);
  EXPECT_LT(preaccess, verifier);
  EXPECT_NE(GoogleString::npos,
            src.find("ngx_array_push(&cmcf->phases[phase].handlers));if(wba_h"));
  EXPECT_NE(GoogleString::npos,
            src.find("ngx_array_push(&cmcf->phases[phase].handlers));if(h=="));
}

// The verdict is read from the main request's slot and never computed here.
TEST(NgxDaemonHtmlAgentClassTest, TheVerdictIsReadNotRecomputed) {
  const GoogleString body =
      Body(ReadStripped("ngx_webbotauth_handler.cc"),
           "DaemonHtmlAgentSignatureps_webbotauth_agent_signature("
           "ngx_http_request_t*r)");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(GoogleString::npos,
            body.find("&r->main->variables[g_xvb_var_index]"));
  EXPECT_NE(GoogleString::npos, body.find("->web_bot_auth()"));
  EXPECT_EQ(GoogleString::npos, body.find("ClassifyRequest("));
  EXPECT_NE(GoogleString::npos,
            body.find("returnNgxDaemonHtmlAgentSignature(enabled,verdict,"
                      "kVerifiedBotSuffix);"));
}

}  // namespace
}  // namespace net_instaweb
