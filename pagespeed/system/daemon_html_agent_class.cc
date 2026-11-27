// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_agent_class.h"

#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/http/known_agent_table.h"

namespace net_instaweb {

const char kDaemonHtmlAgentClassC1[] = "daemon_html_agent_class_c1";
const char kDaemonHtmlAgentClassC3[] = "daemon_html_agent_class_c3";
const char kDaemonHtmlAgentClassC4[] = "daemon_html_agent_class_c4";
const char kDaemonHtmlAgentClassC1NotReady[] =
    "daemon_html_agent_class_c1_not_ready";
const char kDaemonHtmlAgentClassSignatureFailed[] =
    "daemon_html_agent_class_signature_failed";

DaemonHtmlAgentClass ClassifyDaemonHtmlAgent(StringPiece user_agent,
                                             DaemonHtmlAgentSignature signature,
                                             const KnownAgentMatcher* matcher) {
  DaemonHtmlAgentClass result;
  if (signature == DaemonHtmlAgentSignature::kVerified) {
    result.agent_class = kDaemonHtmlAgentClassSigned;
    return result;
  }
  result.c1_not_ready = signature == DaemonHtmlAgentSignature::kNotReady;
  result.signature_failed = signature == DaemonHtmlAgentSignature::kFailed;
  if (matcher == nullptr) matcher = &KnownAgentMatcher::Default();
  if (matcher->Match(user_agent) != nullptr) {
    result.agent_class = kDaemonHtmlAgentClassToken;
  }
  return result;
}

namespace {

void Add(Statistics* statistics, const char* name) {
  Variable* v = statistics->FindVariable(name);
  if (v != nullptr) v->Add(1);
}

}  // namespace

void CountDaemonHtmlAgentClass(const DaemonHtmlAgentClass& result,
                               Statistics* statistics) {
  if (statistics == nullptr) return;
  switch (result.agent_class) {
    case kDaemonHtmlAgentClassSigned:
      Add(statistics, kDaemonHtmlAgentClassC1);
      break;
    case kDaemonHtmlAgentClassToken:
      Add(statistics, kDaemonHtmlAgentClassC3);
      break;
    default:
      Add(statistics, kDaemonHtmlAgentClassC4);
      break;
  }
  if (result.c1_not_ready) Add(statistics, kDaemonHtmlAgentClassC1NotReady);
  if (result.signature_failed) {
    Add(statistics, kDaemonHtmlAgentClassSignatureFailed);
  }
}

int DaemonHtmlAgentClassSlot::Resolve(StringPiece user_agent,
                                      DaemonHtmlAgentSignature signature,
                                      Statistics* statistics) {
  if (!resolved_) {
    const DaemonHtmlAgentClass result =
        ClassifyDaemonHtmlAgent(user_agent, signature);
    CountDaemonHtmlAgentClass(result, statistics);
    agent_class_ = result.agent_class;
    resolved_ = true;
  }
  return agent_class_;
}

}  // namespace net_instaweb
