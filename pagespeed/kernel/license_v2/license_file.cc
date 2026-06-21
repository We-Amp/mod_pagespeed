// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/license_v2/license_file.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
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
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    std::filesystem::path p(s);
    return p.parent_path() / "pagespeed.license";
  }
  // Fallback when cache path is not available.
  return "/etc/modpagespeed/license.key";
}

static constexpr uintmax_t kMaxLicenseFileSize = 2048;

namespace {

// Effective uid of the worker, for the diagnostic message. On Windows there is
// no uid concept; report -1 so the log line stays uniform.
long EffectiveUid() {
#if defined(_WIN32) || defined(WIN32)
  return -1;
#else
  return static_cast<long>(geteuid());
#endif
}

// Emit the single actionable WARNING for a license file that exists but could
// not be read. |reason| is a human-readable cause (typically strerror(errno)).
// Keeping this in one place ensures every "present but unreadable" path logs an
// operator-facing line, not just a silent false.
void LogUnreadable(const std::filesystem::path& path, const char* reason) {
  LOG(WARNING) << "license file " << path.string()
               << " exists but is unreadable by the worker (uid="
               << EffectiveUid() << "): " << reason
               << " — fix ownership/permissions (a root-owned 0600 file is "
                  "invisible to a non-root worker)";
}

}  // namespace

bool ReadLicenseFile(const std::filesystem::path& path, GoogleString* token,
                     LicenseFileReadStatus* status) {
  auto set_status = [status](LicenseFileReadStatus s) {
    if (status != nullptr) *status = s;
  };

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // A missing file is the normal unlicensed state — not an error, no log.
    set_status(LicenseFileReadStatus::kAbsent);
    return false;
  }

  // Reject unexpectedly large files (license tokens are small JWTs).
  uintmax_t file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    LogUnreadable(path, ec.message().c_str());
    set_status(LicenseFileReadStatus::kUnreadable);
    return false;
  }
  if (file_size > kMaxLicenseFileSize) {
    LogUnreadable(path, "file is larger than the maximum license token size");
    set_status(LicenseFileReadStatus::kTooLarge);
    return false;
  }

  // Open via fopen() first so errno reliably reflects the failure cause
  // (std::ifstream does not guarantee errno is set on failure). The most common
  // real-world failure here is EACCES on a root-owned 0600 file under a
  // non-root worker, which previously surfaced as a silent UNLICENSED banner.
  errno = 0;
  std::FILE* probe = std::fopen(path.string().c_str(), "rb");
  if (probe == nullptr) {
    LogUnreadable(path, std::strerror(errno));
    set_status(LicenseFileReadStatus::kUnreadable);
    return false;
  }
  std::fclose(probe);

  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    LogUnreadable(path, "open failed");
    set_status(LicenseFileReadStatus::kUnreadable);
    return false;
  }

  GoogleString content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  // Trim whitespace (tokens should not have leading/trailing whitespace).
  auto start = content.find_first_not_of(" \t\n\r");
  if (start == GoogleString::npos) {
    // File present but empty/whitespace-only: still operator-actionable (a
    // truncated or never-populated license file), so report it as kEmpty.
    token->clear();
    LogUnreadable(path, "file is empty");
    set_status(LicenseFileReadStatus::kEmpty);
    return false;
  }
  auto end = content.find_last_not_of(" \t\n\r");
  *token = content.substr(start, end - start + 1);
  set_status(LicenseFileReadStatus::kOk);
  return true;
}

bool ReadLicenseFile(const std::filesystem::path& path, GoogleString* token) {
  return ReadLicenseFile(path, token, /*status=*/nullptr);
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
    std::ofstream file(tmp_path,
                       std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(token.data(), token.size());
    file.put('\n');
    if (!file.good()) return false;
  }

  // Restrict to owner-only (0600) on the TEMP file, before publishing it via
  // rename, so the final path is never group/other-readable (it holds a bearer
  // token). rename() preserves the inode's mode, so the published file inherits
  // 0600. Best-effort: non-fatal if this fails (e.g. on Windows).
  std::filesystem::permissions(
      tmp_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
#if !defined(_WIN32) && !defined(WIN32)
  if (ec) {
    LOG(WARNING) << "Failed to set 0600 permissions on license file "
                 << tmp_path.string() << ": " << ec.message();
    ec.clear();
  }
#endif

  // Atomic rename (POSIX: rename() is atomic on same filesystem;
  // Windows: std::filesystem::rename uses MoveFileExW internally).
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    // Clean up temp file on failure.
    std::filesystem::remove(tmp_path, ec);
    return false;
  }

  return true;
}

}  // namespace net_instaweb
