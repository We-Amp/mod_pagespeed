// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The agent class of a request, for the HTML cache serve scope.
//
//   1  a signed agent: the Web Bot Auth verifier (nginx only) found a valid
//      signature (signed-agent or verified-bot);
//   2  reserved (published address ranges); never produced;
//   3  the User-Agent carries a product token of the known-agent table
//      (pagespeed/kernel/http/known_agent_table.h);
//   0  anything else (unknown).
//
// A port computes the class once per client request, in its first request
// hook, and keeps it where an internal redirect does not clear it
// (DaemonHtmlAgentClassSlot).  When the verifier is on but has no verdict
// for the request at that point, or its verdict is a failed signature, the
// class is the User-Agent's own, and that is counted.
//
// The class is a claim the client makes, except for 1, and it only widens
// or narrows which requests the serve scope considers; it is not a security
// boundary and selects no action.

#ifndef PAGESPEED_SYSTEM_DAEMON_HTML_AGENT_CLASS_H_
#define PAGESPEED_SYSTEM_DAEMON_HTML_AGENT_CLASS_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class KnownAgentMatcher;
class Statistics;

constexpr int kDaemonHtmlAgentClassUnknown = 0;
constexpr int kDaemonHtmlAgentClassSigned = 1;
constexpr int kDaemonHtmlAgentClassPublishedRange = 2;  // never produced
constexpr int kDaemonHtmlAgentClassToken = 3;

// The counters (registered by RewriteStats::InitStats with the engine's).
// c1, c3 and c4 partition the classified requests; c1_not_ready and
// signature_failed are parts of c3 and c4.
extern const char kDaemonHtmlAgentClassC1[];
extern const char kDaemonHtmlAgentClassC3[];
extern const char kDaemonHtmlAgentClassC4[];
extern const char kDaemonHtmlAgentClassC1NotReady[];
extern const char kDaemonHtmlAgentClassSignatureFailed[];

// What the signature verifier says about the request when the class is
// computed.
enum class DaemonHtmlAgentSignature {
  kNone,      // verifier off, or no web-bot-auth signature on the request
  kNotReady,  // verifier on, no verdict yet for this request
  kVerified,  // signed-agent or verified-bot
  kFailed,    // signature material present but not valid
};

struct DaemonHtmlAgentClass {
  int agent_class = kDaemonHtmlAgentClassUnknown;
  bool c1_not_ready = false;
  bool signature_failed = false;
};

// Pure.  `matcher` null means the compiled-in table.
DaemonHtmlAgentClass ClassifyDaemonHtmlAgent(
    StringPiece user_agent, DaemonHtmlAgentSignature signature,
    const KnownAgentMatcher* matcher = nullptr);

// Adds one request to the counters.  `statistics` may be null.
void CountDaemonHtmlAgentClass(const DaemonHtmlAgentClass& result,
                               Statistics* statistics);

// The once-per-request record a port keeps with the client request.
class DaemonHtmlAgentClassSlot {
 public:
  bool resolved() const { return resolved_; }
  // The class; 0 until resolved.
  int agent_class() const { return agent_class_; }

  // On the first call, classifies, counts and keeps the class; every later
  // call returns the kept class and neither classifies nor counts.
  int Resolve(StringPiece user_agent, DaemonHtmlAgentSignature signature,
              Statistics* statistics);

 private:
  bool resolved_ = false;
  int agent_class_ = kDaemonHtmlAgentClassUnknown;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_AGENT_CLASS_H_
