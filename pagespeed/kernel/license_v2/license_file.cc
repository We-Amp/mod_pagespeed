// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/license_v2/license_file.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32) || defined(WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "base/logging.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

std::filesystem::path LicenseFilePath(const GoogleString& cache_path) {
  if (!cache_path.empty()) {
    // Store alongside the cache directory, matching 2.0 convention.
    // e.g. cache_path="/var/cache/mod_pagespeed" → "/var/cache/pagespeed.license"
    // Note: std::filesystem::path("/a/b/").parent_path() returns "/a/b"
    // (strips the slash but stays at the same level), so we must strip
    // any trailing separator before calling parent_path().
    GoogleString s(cache_path);
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
      s.pop_back();
    std::filesystem::path p(s);
    return p.parent_path() / "pagespeed.license";
  }
  // Fallback when cache path is not available.
  return "/etc/modpagespeed/license.key";
}

static constexpr uintmax_t kMaxLicenseFileSize = 2048;

bool ReadLicenseFile(const std::filesystem::path& path, GoogleString* token) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;

  // Reject unexpectedly large files (license tokens are small JWTs).
  uintmax_t file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size > kMaxLicenseFileSize) return false;

  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file.is_open()) return false;

  GoogleString content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  // Trim whitespace (tokens should not have leading/trailing whitespace).
  auto start = content.find_first_not_of(" \t\n\r");
  if (start == GoogleString::npos) {
    token->clear();
    return false;
  }
  auto end = content.find_last_not_of(" \t\n\r");
  *token = content.substr(start, end - start + 1);
  return true;
}

bool WriteLicenseFile(const std::filesystem::path& path, StringPiece token) {
  // Create parent directories if they don't exist.
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
  }

  // Write to a temporary file first, then atomically rename.
  // This prevents corruption if the process crashes mid-write.
  // Use PID + counter to avoid predictable/colliding temp file names.
  static std::atomic<int> tmp_counter{0};
  std::filesystem::path tmp_path = path;
  tmp_path += ".tmp." + std::to_string(getpid()) + "." +
              std::to_string(tmp_counter.fetch_add(1));

  {
    std::ofstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(token.data(), token.size());
    file.put('\n');
    if (!file.good()) return false;
  }

  // Atomic rename (POSIX: rename() is atomic on same filesystem;
  // Windows: std::filesystem::rename uses MoveFileExW internally).
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    // Clean up temp file on failure.
    std::filesystem::remove(tmp_path, ec);
    return false;
  }

  // Restrict file permissions to owner-only (0600).
  // Best-effort: non-fatal if this fails (e.g. on Windows).
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
#if !defined(_WIN32) && !defined(WIN32)
  if (ec) {
    LOG(WARNING) << "Failed to set 0600 permissions on license file "
                 << path.string() << ": " << ec.message();
  }
#endif

  return true;
}

}  // namespace net_instaweb
