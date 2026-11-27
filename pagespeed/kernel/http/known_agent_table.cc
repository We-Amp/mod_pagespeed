// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/kernel/http/known_agent_table.h"

#include <algorithm>

#include "base/logging.h"

namespace net_instaweb {

namespace {

#define KNOWN_AGENT(token, kind) \
  {token, sizeof(token) - 1, KnownAgentKind::kind},
const KnownAgent kKnownAgents[] = {
#include "pagespeed/kernel/http/known_agent_table.inc"
};
#undef KNOWN_AGENT

inline unsigned char AsciiLower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A'))
                                : c;
}

bool AsciiCaseEqual(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (AsciiLower(static_cast<unsigned char>(a[i])) !=
        AsciiLower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool IsKnownAgentTokenBoundary(char c) {
  return c == ' ' || c == '(' || c == ')' || c == ';' || c == '/';
}

KnownAgentMatcher::KnownAgentMatcher(const KnownAgent* table, size_t size) {
  CHECK_LT(size, 0xFFFFu);
  entries_.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    // An empty token would match at every boundary.
    CHECK_GT(table[i].length, 0u);
    entries_.push_back(&table[i]);
  }
  auto first = [](const KnownAgent* e) {
    return AsciiLower(static_cast<unsigned char>(e->token[0]));
  };
  std::stable_sort(entries_.begin(), entries_.end(),
                   [&first](const KnownAgent* a, const KnownAgent* b) {
                     return first(a) < first(b);
                   });
  size_t at = 0;
  for (int b = 0; b <= 256; ++b) {
    while (at < entries_.size() && first(entries_[at]) < b) ++at;
    bucket_[b] = static_cast<uint16_t>(at);
  }
}

const KnownAgentMatcher& KnownAgentMatcher::Default() {
  static const KnownAgentMatcher* const matcher = [] {
    size_t size = 0;
    const KnownAgent* table = KnownAgentTable(&size);
    return new KnownAgentMatcher(table, size);
  }();
  return *matcher;
}

const KnownAgent* KnownAgentMatcher::Match(StringPiece user_agent) const {
  const char* ua = user_agent.data();
  const size_t n = user_agent.size();
  for (size_t i = 0; i < n; ++i) {
    if (i > 0 && !IsKnownAgentTokenBoundary(ua[i - 1])) continue;
    const unsigned char c = AsciiLower(static_cast<unsigned char>(ua[i]));
    const KnownAgent* best = nullptr;
    for (uint16_t k = bucket_[c]; k < bucket_[c + 1]; ++k) {
      const KnownAgent* e = entries_[k];
      const size_t end = i + e->length;
      if (end > n) continue;
      if (end < n && !IsKnownAgentTokenBoundary(ua[end])) continue;
      if (!AsciiCaseEqual(ua + i, e->token, e->length)) continue;
      if (best == nullptr || e->length > best->length) best = e;
    }
    if (best != nullptr) return best;
  }
  return nullptr;
}

const KnownAgent* KnownAgentTable(size_t* size) {
  *size = sizeof(kKnownAgents) / sizeof(kKnownAgents[0]);
  return kKnownAgents;
}

}  // namespace net_instaweb
