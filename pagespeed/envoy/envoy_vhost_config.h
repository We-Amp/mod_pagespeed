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

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class MessageHandler;

// Configuration for a single virtual host.
struct VHostEntry {
  GoogleString host_pattern;  // Host pattern with wildcards (* and ?)
  int32 priority;             // Lower = higher priority
  std::unique_ptr<EnvoyRewriteOptions> options;

  VHostEntry(StringPiece pattern, int32 prio,
             std::unique_ptr<EnvoyRewriteOptions> opts)
      : host_pattern(pattern.as_string()),
        priority(prio),
        options(std::move(opts)) {}
};

// Manages virtual host configurations and provides host-based option lookup.
// Thread-safe for concurrent reads after initialization.
class EnvoyVHostConfigManager {
 public:
  EnvoyVHostConfigManager();
  ~EnvoyVHostConfigManager();

  // Add a virtual host configuration. Must be called during initialization,
  // before any lookups. The options are owned by this manager after the call.
  void AddVHost(StringPiece host_pattern, int32 priority,
                std::unique_ptr<EnvoyRewriteOptions> options);

  // Sort entries by priority after all VHosts have been added.
  // Must be called before FindOptionsForHost().
  void SortByPriority();

  // Find the options for a given hostname. Returns nullptr if no VHost matches.
  // The returned pointer is valid for the lifetime of this manager.
  const EnvoyRewriteOptions* FindOptionsForHost(StringPiece hostname) const;

  // Check if a host pattern matches a hostname.
  // Supports wildcards: * matches any sequence, ? matches single character.
  static bool HostPatternMatches(StringPiece pattern, StringPiece hostname);

  // Returns true if any virtual hosts are configured.
  bool HasVHosts() const { return !vhosts_.empty(); }

  // Returns the number of virtual hosts.
  size_t size() const { return vhosts_.size(); }

 private:
  std::vector<std::unique_ptr<VHostEntry>> vhosts_;
  bool sorted_;

  EnvoyVHostConfigManager(const EnvoyVHostConfigManager&) = delete;
  EnvoyVHostConfigManager& operator=(const EnvoyVHostConfigManager&) = delete;
};

}  // namespace net_instaweb
