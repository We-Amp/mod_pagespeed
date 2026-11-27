// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The known AI agent table and its User-Agent matcher.
//
// The table is compiled in (known_agent_table.inc, a build-time snapshot of
// the optimizer's agent list at the commit test/system/known-agent-table-pin
// .json names; tools/gen_known_agent_table.py writes it and the pull-request
// tier checks it). Nothing is loaded or fetched at run time.
//
// MATCHING.  A table token matches a User-Agent when it occurs in it,
// compared ASCII case-insensitively, and both ends of the occurrence fall on
// a token boundary: the start or the end of the string, or one of the
// characters ' ', '(', ')', ';', '/'.  A token with a space matches as one
// phrase whose two ends are checked.  Characters inside a token are literal.
// There is no substring match, no glob and no regular expression.  The
// shared vectors (test/system/daemon_html/agent_vectors.txt) pin the rule.
//
// A User-Agent is whatever the client sends, so a match says only what the
// client claims to be.  The result feeds the HTML cache serve scope, which is
// not a security boundary; it never selects an action.
//
// COST.  One pass over the User-Agent: at each position that starts on a
// boundary, only the tokens that share its first (lower-cased) byte are
// compared.  No allocation.

#ifndef PAGESPEED_KERNEL_HTTP_KNOWN_AGENT_TABLE_H_
#define PAGESPEED_KERNEL_HTTP_KNOWN_AGENT_TABLE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

enum class KnownAgentKind { kTraining, kSearch, kFetcher, kOther };

struct KnownAgent {
  const char* token;
  size_t length;
  KnownAgentKind kind;
};

// True for ' ', '(', ')', ';' and '/'.
bool IsKnownAgentTokenBoundary(char c);

class KnownAgentMatcher {
 public:
  // `table` must outlive the matcher.  Order does not matter.
  KnownAgentMatcher(const KnownAgent* table, size_t size);

  KnownAgentMatcher(const KnownAgentMatcher&) = delete;
  KnownAgentMatcher& operator=(const KnownAgentMatcher&) = delete;

  // The compiled-in table's matcher, built on first use.
  static const KnownAgentMatcher& Default();

  // A table entry whose token matches `user_agent`, or null.  When several
  // match, the one that starts first, then the longest.
  const KnownAgent* Match(StringPiece user_agent) const;

  size_t size() const { return entries_.size(); }
  const KnownAgent& entry(size_t i) const { return *entries_[i]; }

 private:
  // Sorted by lower-cased first byte; bucket_[b] .. bucket_[b + 1] are the
  // entries whose token starts with byte b (lower-cased).
  std::vector<const KnownAgent*> entries_;
  std::array<uint16_t, 257> bucket_;
};

// The compiled-in table, in the snapshot's order.
const KnownAgent* KnownAgentTable(size_t* size);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTTP_KNOWN_AGENT_TABLE_H_
