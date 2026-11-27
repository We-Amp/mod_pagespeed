// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_agent_class.h"

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/known_agent_table.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/agent_vectors.h"

namespace net_instaweb {
namespace {

const char kGptBot[] = "Mozilla/5.0 (compatible; GPTBot/1.2; +https://openai.com/gptbot)";
const char kBrowser[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";

class DaemonHtmlAgentClassTest : public testing::Test {
 protected:
  DaemonHtmlAgentClassTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()) {
    // The counters are registered with the engine's.
    RewriteStats::InitStats(&stats_);
  }

  int64 Count(const char* name) { return stats_.GetVariable(name)->Get(); }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
};

TEST_F(DaemonHtmlAgentClassTest, ClassValuesAreTheScopes) {
  EXPECT_EQ(0, kDaemonHtmlAgentClassUnknown);
  EXPECT_EQ(1, kDaemonHtmlAgentClassSigned);
  EXPECT_EQ(2, kDaemonHtmlAgentClassPublishedRange);
  EXPECT_EQ(3, kDaemonHtmlAgentClassToken);
}

// Every shared vector, with no verifier (Apache and IIS, and nginx with
// WebBotAuth off).
TEST_F(DaemonHtmlAgentClassTest, EverySharedVector) {
  const AgentVectors vectors = LoadAgentVectors();
  int64 c3 = 0, c4 = 0;
  for (const AgentVector& v : vectors.vectors) {
    const DaemonHtmlAgentClass result = ClassifyDaemonHtmlAgent(
        v.user_agent, DaemonHtmlAgentSignature::kNone);
    EXPECT_EQ(v.expected_class, result.agent_class)
        << "agent_vectors.txt:" << v.line << ": " << v.user_agent;
    EXPECT_FALSE(result.c1_not_ready);
    EXPECT_FALSE(result.signature_failed);
    CountDaemonHtmlAgentClass(result, &stats_);
    (v.expected_class == 3 ? c3 : c4)++;
  }
  EXPECT_EQ(c3, Count(kDaemonHtmlAgentClassC3));
  EXPECT_EQ(c4, Count(kDaemonHtmlAgentClassC4));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC1));
}

// Class 2 is never produced, and class 1 never comes from a User-Agent.
TEST_F(DaemonHtmlAgentClassTest, NoUserAgentGivesClassOneOrTwo) {
  for (const AgentVector& v : LoadAgentVectors().vectors) {
    for (DaemonHtmlAgentSignature s :
         {DaemonHtmlAgentSignature::kNone, DaemonHtmlAgentSignature::kNotReady,
          DaemonHtmlAgentSignature::kFailed}) {
      const int c = ClassifyDaemonHtmlAgent(v.user_agent, s).agent_class;
      EXPECT_TRUE(c == 0 || c == 3) << v.line;
    }
  }
  EXPECT_EQ(3, ClassifyDaemonHtmlAgent("signed-agent verified-bot GPTBot",
                                       DaemonHtmlAgentSignature::kNone)
                   .agent_class);
}

TEST_F(DaemonHtmlAgentClassTest, AVerifiedSignatureIsClassOne) {
  for (const char* ua : {kGptBot, kBrowser, ""}) {
    const DaemonHtmlAgentClass result =
        ClassifyDaemonHtmlAgent(ua, DaemonHtmlAgentSignature::kVerified);
    EXPECT_EQ(kDaemonHtmlAgentClassSigned, result.agent_class) << ua;
    EXPECT_FALSE(result.c1_not_ready);
    EXPECT_FALSE(result.signature_failed);
    CountDaemonHtmlAgentClass(result, &stats_);
  }
  EXPECT_EQ(3, Count(kDaemonHtmlAgentClassC1));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC3));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC4));
}

// No verdict yet at the slot: the User-Agent's own class, counted.
TEST_F(DaemonHtmlAgentClassTest, NotReadyGivesTheUserAgentsClass) {
  DaemonHtmlAgentClass result =
      ClassifyDaemonHtmlAgent(kGptBot, DaemonHtmlAgentSignature::kNotReady);
  EXPECT_EQ(kDaemonHtmlAgentClassToken, result.agent_class);
  EXPECT_TRUE(result.c1_not_ready);
  CountDaemonHtmlAgentClass(result, &stats_);
  result =
      ClassifyDaemonHtmlAgent(kBrowser, DaemonHtmlAgentSignature::kNotReady);
  EXPECT_EQ(kDaemonHtmlAgentClassUnknown, result.agent_class);
  EXPECT_TRUE(result.c1_not_ready);
  CountDaemonHtmlAgentClass(result, &stats_);
  EXPECT_EQ(2, Count(kDaemonHtmlAgentClassC1NotReady));
  EXPECT_EQ(1, Count(kDaemonHtmlAgentClassC3));
  EXPECT_EQ(1, Count(kDaemonHtmlAgentClassC4));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC1));
}

// A signature that did not verify drops to the User-Agent's own class: it
// never raises the class, and never lowers a token match.
TEST_F(DaemonHtmlAgentClassTest, AFailedSignatureDropsToTheUserAgentsClass) {
  DaemonHtmlAgentClass result =
      ClassifyDaemonHtmlAgent(kGptBot, DaemonHtmlAgentSignature::kFailed);
  EXPECT_EQ(kDaemonHtmlAgentClassToken, result.agent_class);
  EXPECT_TRUE(result.signature_failed);
  EXPECT_FALSE(result.c1_not_ready);
  CountDaemonHtmlAgentClass(result, &stats_);
  result = ClassifyDaemonHtmlAgent(kBrowser, DaemonHtmlAgentSignature::kFailed);
  EXPECT_EQ(kDaemonHtmlAgentClassUnknown, result.agent_class);
  CountDaemonHtmlAgentClass(result, &stats_);
  EXPECT_EQ(2, Count(kDaemonHtmlAgentClassSignatureFailed));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC1));
}

TEST_F(DaemonHtmlAgentClassTest, NullStatisticsCountNothing) {
  CountDaemonHtmlAgentClass(
      ClassifyDaemonHtmlAgent(kGptBot, DaemonHtmlAgentSignature::kNone),
      nullptr);
  DaemonHtmlAgentClassSlot slot;
  EXPECT_EQ(3, slot.Resolve(kGptBot, DaemonHtmlAgentSignature::kNone, nullptr));
}

// The slot classifies and counts on its first use only: an internal
// redirect, a subrequest or a later reader sees the first pass's class,
// whatever it reads then.
TEST_F(DaemonHtmlAgentClassTest, TheSlotResolvesOnce) {
  DaemonHtmlAgentClassSlot slot;
  EXPECT_FALSE(slot.resolved());
  EXPECT_EQ(0, slot.agent_class());
  EXPECT_EQ(3, slot.Resolve(kGptBot, DaemonHtmlAgentSignature::kNotReady,
                            &stats_));
  EXPECT_TRUE(slot.resolved());
  EXPECT_EQ(3, slot.Resolve(kBrowser, DaemonHtmlAgentSignature::kVerified,
                            &stats_));
  EXPECT_EQ(3, slot.Resolve("", DaemonHtmlAgentSignature::kFailed, &stats_));
  EXPECT_EQ(3, slot.agent_class());
  EXPECT_EQ(1, Count(kDaemonHtmlAgentClassC3));
  EXPECT_EQ(1, Count(kDaemonHtmlAgentClassC1NotReady));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC1));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassC4));
  EXPECT_EQ(0, Count(kDaemonHtmlAgentClassSignatureFailed));
}

TEST_F(DaemonHtmlAgentClassTest, AnInjectedTable) {
  const KnownAgent table[] = {{"Lane-Agent", 10, KnownAgentKind::kOther}};
  KnownAgentMatcher matcher(table, 1);
  EXPECT_EQ(3, ClassifyDaemonHtmlAgent("x Lane-Agent/1",
                                       DaemonHtmlAgentSignature::kNone, &matcher)
                   .agent_class);
  EXPECT_EQ(0, ClassifyDaemonHtmlAgent(kGptBot, DaemonHtmlAgentSignature::kNone,
                                       &matcher)
                   .agent_class);
}

}  // namespace
}  // namespace net_instaweb
