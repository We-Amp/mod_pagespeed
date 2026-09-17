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

#include "pagespeed/envoy/envoy_rewrite_options.h"

namespace net_instaweb {

EnvoyVHostConfigManager::EnvoyVHostConfigManager() : sorted_(false) {}

EnvoyVHostConfigManager::~EnvoyVHostConfigManager() = default;

void EnvoyVHostConfigManager::AddVHost(
    StringPiece host_pattern, int32 priority,
    std::unique_ptr<EnvoyRewriteOptions> options) {
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
