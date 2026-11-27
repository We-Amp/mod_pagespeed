/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/envoy/envoy_vhost_config.h"

#include <algorithm>
#include <memory>
#include <string>

#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// DaemonHtmlCacheIgnoreCookie as a comma-separated list: every element is
// set as its own line would be, with the setting's one warning, or none is.
bool ApplyIgnoreCookieList(EnvoyRewriteOptions* options,
                           const std::string& host_pattern,
                           const std::string& key, const std::string& value,
                           MessageHandler* handler) {
  StringPieceVector names;
  SplitStringPieceToVector(value, ",", &names, false);
  for (StringPiece& name : names) {
    TrimWhitespace(&name);
    if (!RewriteOptions::IsValidDaemonHtmlCacheIgnoreCookie(name)) {
      handler->Message(kWarning,
                       "VHost %s: Failed to set option %s: '%s' is not one "
                       "cookie name, or a prefix ending in *",
                       host_pattern.c_str(), key.c_str(),
                       name.as_string().c_str());
      return false;
    }
  }
  for (size_t i = 0; i < names.size(); ++i) {
    GoogleString msg;
    if (options->ParseAndSetOptionFromName1(key, names[i], &msg,
                                            i == 0 ? handler : nullptr) !=
        RewriteOptions::kOptionOk) {
      handler->Message(kWarning, "VHost %s: Failed to set option %s: %s",
                       host_pattern.c_str(), key.c_str(), msg.c_str());
      return false;
    }
  }
  handler->Message(kInfo, "VHost %s: Set option %s = %s",
                   host_pattern.c_str(), key.c_str(), value.c_str());
  return true;
}

}  // namespace

bool ApplyVHostCustomOption(EnvoyRewriteOptions* options,
                            const std::string& host_pattern,
                            const std::string& key, const std::string& value,
                            MessageHandler* handler) {
  if (StringCaseEqual(key, RewriteOptions::kDaemonHtmlCacheIgnoreCookie)) {
    return ApplyIgnoreCookieList(options, host_pattern, key, value, handler);
  }
  GoogleString msg;
  RewriteOptions::OptionSettingResult result =
      options->ParseAndSetOptionFromName1(key, value, &msg, handler);
  if (result == RewriteOptions::kOptionOk) {
    handler->Message(kInfo, "VHost %s: Set option %s = %s",
                     host_pattern.c_str(), key.c_str(), value.c_str());
    return true;
  }
  if (result == RewriteOptions::kOptionNameUnknown &&
      RewriteOptions::IsDeprecatedOptionName(key)) {
    handler->Message(kWarning,
                     "VHost %s: pagespeed %s is deprecated and has no "
                     "effect; please remove it from your configuration",
                     host_pattern.c_str(), key.c_str());
    return false;
  }
  handler->Message(kWarning, "VHost %s: Failed to set option %s: %s",
                   host_pattern.c_str(), key.c_str(), msg.c_str());
  return false;
}

EnvoyVHostConfigManager::EnvoyVHostConfigManager() : sorted_(false) {}

EnvoyVHostConfigManager::~EnvoyVHostConfigManager() = default;

void EnvoyVHostConfigManager::AddVHost(
    StringPiece host_pattern, int32 priority,
    std::unique_ptr<EnvoyRewriteOptions> options) {
  // The options are final once added: requests on every worker thread read
  // them from here on, so they are frozen now, on the configuration thread.
  options->Freeze();
  vhosts_.push_back(
      std::make_unique<VHostEntry>(host_pattern, priority, std::move(options)));
  sorted_ = false;
}

void EnvoyVHostConfigManager::SortByPriority() {
  std::stable_sort(vhosts_.begin(), vhosts_.end(),
                   [](const std::unique_ptr<VHostEntry>& a,
                      const std::unique_ptr<VHostEntry>& b) {
                     return a->priority < b->priority;
                   });
  sorted_ = true;
}

const EnvoyRewriteOptions* EnvoyVHostConfigManager::FindOptionsForHost(
    StringPiece hostname) const {
  // Entries should be sorted by priority before lookup.
  // If not sorted, we still search in order of addition.
  for (const auto& entry : vhosts_) {
    if (HostPatternMatches(entry->host_pattern, hostname)) {
      return entry->options.get();
    }
  }
  return nullptr;
}

// Wildcard pattern matching algorithm.
// '*' matches zero or more characters.
// '?' matches exactly one character.
// Matching is case-insensitive for hostnames.
bool EnvoyVHostConfigManager::HostPatternMatches(StringPiece pattern,
                                                 StringPiece hostname) {
  // Convert both to lowercase for case-insensitive matching.
  GoogleString pattern_lower;
  GoogleString hostname_lower;
  pattern.CopyToString(&pattern_lower);
  hostname.CopyToString(&hostname_lower);
  LowerString(&pattern_lower);
  LowerString(&hostname_lower);

  const char* p = pattern_lower.c_str();
  const char* h = hostname_lower.c_str();
  const char* star_p = nullptr;
  const char* star_h = nullptr;

  while (*h != '\0') {
    if (*p == '*') {
      // Star found - remember position and try matching zero chars
      star_p = p++;
      star_h = h;
    } else if (*p == '?' || *p == *h) {
      // Single char match (? or exact)
      p++;
      h++;
    } else if (star_p != nullptr) {
      // Mismatch after star - backtrack and try matching one more char
      p = star_p + 1;
      h = ++star_h;
    } else {
      // No match and no star to backtrack to
      return false;
    }
  }

  // Skip trailing stars in pattern
  while (*p == '*') {
    p++;
  }

  return *p == '\0';
}

}  // namespace net_instaweb
