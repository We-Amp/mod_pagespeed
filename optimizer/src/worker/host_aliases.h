// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef SRC_WORKER_HOST_ALIASES_H_
#define SRC_WORKER_HOST_ALIASES_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pagespeed {

// Returns path to pagespeed-hosts.conf given the cache_path.
// Same parent directory as SharedConfigFilePath().
std::string HostAliasesFilePath(const std::string& cache_path);

// Write pagespeed-hosts.conf atomically (tmp + fsync + rename).
// Mode 0644. Returns true on success.
// Aliases are written as key=value lines (source=canonical).
bool WriteHostAliasesFile(
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& aliases);

// Read and parse pagespeed-hosts.conf from disk.
// Returns empty map if file missing, unreadable, or version mismatch.
std::unordered_map<std::string, std::string> ReadHostAliasesFile(
    const std::string& path);

// Parse the file content directly (for testing).
std::unordered_map<std::string, std::string> ParseHostAliases(
    std::string_view content);

}  // namespace pagespeed

#endif  // SRC_WORKER_HOST_ALIASES_H_
