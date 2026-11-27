// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/host_aliases.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "lib/base/atomic_file_writer.h"

namespace pagespeed {

std::string HostAliasesFilePath(const std::string& cache_path) {
  if (cache_path.empty()) {
    return {};
  }
  std::filesystem::path p(cache_path);
  return (p.parent_path() / "pagespeed-hosts.conf").string();
}

std::unordered_map<std::string, std::string> ParseHostAliases(
    std::string_view content) {
  std::unordered_map<std::string, std::string> aliases;

  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    if (eol == std::string_view::npos) eol = content.size();

    std::string_view line = content.substr(pos, eol - pos);
    pos = eol + 1;

    // Strip trailing \r.
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    // Skip empty lines and comments.
    if (line.empty() || line[0] == '#') continue;

    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;

    std::string_view key = line.substr(0, eq);
    std::string_view value = line.substr(eq + 1);

    // Reject values with null bytes.
    if (key.find('\0') != std::string_view::npos ||
        value.find('\0') != std::string_view::npos) {
      continue;
    }

    // version=N — return empty on version >= 2.
    if (key == "version") {
      int v = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), v);
      if (ec == std::errc{} && v >= 2) return {};
      continue;
    }

    if (!key.empty() && !value.empty()) {
      aliases[std::string(key)] = std::string(value);
    }
  }

  return aliases;
}

static constexpr size_t kMaxHostAliasesFileSize = size_t{64} * 1024;

std::unordered_map<std::string, std::string> ReadHostAliasesFile(
    const std::string& path) {
  if (path.empty()) return {};

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return {};

  auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxHostAliasesFileSize)
    return {};

  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (!f.read(content.data(), size)) return {};

  return ParseHostAliases(content);
}

bool WriteHostAliasesFile(
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& aliases) {
  if (path.empty()) return false;

  // Reject values that could inject extra config lines.
  auto has_newline = [](std::string_view s) {
    return s.find('\n') != std::string_view::npos ||
           s.find('\r') != std::string_view::npos;
  };
  for (const auto& [src, dst] : aliases) {
    if (has_newline(src) || has_newline(dst)) {
      return false;
    }
  }

  std::string content;
  content += "# Written by pagespeed-worker. Do not edit.\n";
  content += "version=1\n";
  for (const auto& [src, dst] : aliases) {
    content += src;
    content += '=';
    content += dst;
    content += '\n';
  }

  return AtomicWriteFile(path, content, 0644);
}

}  // namespace pagespeed
