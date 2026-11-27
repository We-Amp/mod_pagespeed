// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed pagespeed/kernel/webbotauth/classifier.cc
// @ 8e15a19ce (2026-07-02). Keep logic in sync manually; the RFC 9421/8941
// core is standards-frozen.

#include "src/crypto/webbotauth/classifier.h"

#include <string>
#include <string_view>

namespace pagespeed {
namespace webbotauth {

const char* VerdictToken(Verdict v) {
  switch (v) {
    case Verdict::kHuman:
      return "human";
    case Verdict::kSignedAgent:
      return "signed-agent";
    case Verdict::kVerifiedBot:
      return "verified-bot";
    case Verdict::kUnknown:
      return "unknown";
  }
  return "unknown";
}

void VerifiedBotRegistry::Register(std::string_view keyid,
                                   std::string_view bot_name) {
  map_[std::string(keyid)] = std::string(bot_name);
}

bool VerifiedBotRegistry::Lookup(std::string_view keyid,
                                 std::string* bot_name) const {
  auto it = map_.find(keyid);
  if (it == map_.end()) return false;
  if (bot_name != nullptr) *bot_name = it->second;
  return true;
}

Verdict ClassifyVerified(std::string_view keyid,
                         const VerifiedBotRegistry& registry,
                         std::string* bot_name_out) {
  std::string bot_name;
  if (registry.Lookup(keyid, &bot_name)) {
    if (bot_name_out != nullptr) *bot_name_out = bot_name;
    return Verdict::kVerifiedBot;
  }
  return Verdict::kSignedAgent;
}

}  // namespace webbotauth
}  // namespace pagespeed
