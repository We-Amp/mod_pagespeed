// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/classifier.h"

namespace net_instaweb {
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

void VerifiedBotRegistry::Register(StringPiece keyid, StringPiece bot_name) {
  map_[GoogleString(keyid.data(), keyid.size())] =
      GoogleString(bot_name.data(), bot_name.size());
}

bool VerifiedBotRegistry::Lookup(StringPiece keyid,
                                 GoogleString* bot_name) const {
  auto it = map_.find(GoogleString(keyid.data(), keyid.size()));
  if (it == map_.end()) return false;
  if (bot_name != nullptr) *bot_name = it->second;
  return true;
}

Verdict ClassifyVerified(StringPiece keyid, const VerifiedBotRegistry& registry,
                         GoogleString* bot_name_out) {
  GoogleString bot_name;
  if (registry.Lookup(keyid, &bot_name)) {
    if (bot_name_out != nullptr) *bot_name_out = bot_name;
    return Verdict::kVerifiedBot;
  }
  return Verdict::kSignedAgent;
}

}  // namespace webbotauth
}  // namespace net_instaweb
