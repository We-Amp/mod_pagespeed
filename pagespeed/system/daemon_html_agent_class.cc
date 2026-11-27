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

Variable* Find(Statistics* statistics, const char* name) {
  return statistics == nullptr ? nullptr : statistics->FindVariable(name);
}

void Add(Variable* v) {
  if (v != nullptr) v->Add(1);
}

}  // namespace

DaemonHtmlAgentClassCounters::DaemonHtmlAgentClassCounters(
    Statistics* statistics)
    : c1(Find(statistics, kDaemonHtmlAgentClassC1)),
      c3(Find(statistics, kDaemonHtmlAgentClassC3)),
      c4(Find(statistics, kDaemonHtmlAgentClassC4)),
      c1_not_ready(Find(statistics, kDaemonHtmlAgentClassC1NotReady)),
      signature_failed(Find(statistics, kDaemonHtmlAgentClassSignatureFailed)) {
}

void CountDaemonHtmlAgentClass(const DaemonHtmlAgentClass& result,
                               const DaemonHtmlAgentClassCounters* counters) {
  if (counters == nullptr) return;
  switch (result.agent_class) {
    case kDaemonHtmlAgentClassSigned:
      Add(counters->c1);
      break;
    case kDaemonHtmlAgentClassToken:
      Add(counters->c3);
      break;
    default:
      Add(counters->c4);
      break;
  }
  if (result.c1_not_ready) Add(counters->c1_not_ready);
  if (result.signature_failed) Add(counters->signature_failed);
}

int DaemonHtmlAgentClassSlot::Resolve(
    StringPiece user_agent, DaemonHtmlAgentSignature signature,
    const DaemonHtmlAgentClassCounters* counters) {
  if (!resolved_) {
    const DaemonHtmlAgentClass result =
        ClassifyDaemonHtmlAgent(user_agent, signature);
    CountDaemonHtmlAgentClass(result, counters);
    agent_class_ = result.agent_class;
    resolved_ = true;
  }
  return agent_class_;
}

}  // namespace net_instaweb
