// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/value_mask.h"

#include <regex>

namespace net_instaweb {
namespace lint {

namespace {

#include "pagespeed/tools/cli/safe_option_names.inc"

constexpr char kOptionPrefix[] = "ModPagespeed";

}  // namespace

ValueMask::ValueMask() {
  // The generator already left out every name matching SECRET_NAME; checking
  // again here keeps the two-part rule in one place even if the generated
  // list were edited by hand.
  const std::regex secret(kSecretNamePattern,
                          std::regex::ECMAScript | std::regex::icase);
  for (const char* name : kSafeOptionNames) {
    if (!std::regex_search(name, secret)) {
      safe_names_.insert(name);
    }
  }
}

int ValueMask::SourceEntryCount() { return kSafeOptionNamesTsCount; }

int ValueMask::SourceSecretExcludedCount() {
  return kSafeOptionNamesSecretExcluded;
}

bool ValueMask::ValuesAreSafe(StringPiece directive) const {
  if (show_all_) {
    return true;
  }
  if (!StringCaseStartsWith(directive, kOptionPrefix)) {
    return false;
  }
  StringPiece option = directive.substr(strlen(kOptionPrefix));
  // The plain "ModPagespeed" directive names no option, so it is masked like
  // any other name outside the list.
  return !option.empty() && safe_names_.count(option.as_string()) > 0;
}

std::vector<GoogleString> ValueMask::Args(
    StringPiece directive, const std::vector<GoogleString>& args) const {
  if (ValuesAreSafe(directive)) {
    return args;
  }
  return std::vector<GoogleString>(args.size(), kMasked);
}

GoogleString ValueMask::Text(StringPiece text) const {
  return show_all_ ? text.as_string() : GoogleString(kMasked);
}

}  // namespace lint
}  // namespace net_instaweb
