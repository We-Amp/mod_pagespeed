// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/site_purge_epoch.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace pagespeed {

namespace {

// "pspe1 " + 20 zero-padded decimal digits + "\n": every value has the same
// width, so a write replaces the whole record in place and never leaves a
// shorter tail behind.
constexpr std::string_view kPrefix = "pspe1 ";
constexpr size_t kDigits = 20;
constexpr size_t kRecordBytes = 6 + kDigits + 1;
static_assert(kRecordBytes == kPrefix.size() + kDigits + 1);

// The byte range the advisory lock covers: one byte far past the record, so
// that on Windows (where byte-range locks are mandatory) the lock never
// blocks the record's own reads and writes.
constexpr uint64_t kLockOffset = uint64_t{1} << 30;

constexpr auto kLockWait = std::chrono::milliseconds(2000);
constexpr auto kLockRetry = std::chrono::milliseconds(5);

// fcntl record locks belong to the PROCESS (and closing any descriptor of
// the file drops them), so every use of the file in this process — reads
// included — goes through this mutex.
std::mutex& ProcessEpochMutex() {
  static std::mutex m;
  return m;
}

std::string Render(uint64_t epoch) {
  char digits[kDigits + 1];
  std::snprintf(digits, sizeof(digits), "%020llu",
                static_cast<unsigned long long>(epoch));
  std::string out(kPrefix);
  out.append(digits, kDigits);
  out += '\n';
  return out;
}

enum class OpenMode : std::uint8_t { kRead, kWrite };

#ifdef _WIN32

std::wstring Wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), w.data(), n);
  return w;
}

class EpochFile {
 public:
  // On success `missing` is false and the file is open and locked (shared for
  // kRead, exclusive for kWrite); `created` says whether this call made it.
  EpochFile(const std::string& path, OpenMode mode) {
    const std::wstring wpath = Wide(path);
    if (wpath.empty()) return;
    const DWORD access =
        mode == OpenMode::kWrite ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT;
    if (mode == OpenMode::kWrite) {
      h_ = CreateFileW(wpath.c_str(), access, share, nullptr, CREATE_NEW, flags,
                       nullptr);
      if (h_ != INVALID_HANDLE_VALUE) {
        created_ = true;
      } else if (GetLastError() == ERROR_FILE_EXISTS) {
        h_ = CreateFileW(wpath.c_str(), access, share, nullptr, OPEN_EXISTING,
                         flags, nullptr);
      }
    } else {
      h_ = CreateFileW(wpath.c_str(), access, share, nullptr, OPEN_EXISTING,
                       flags, nullptr);
      if (h_ == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        missing_ = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
      }
    }
    if (h_ == INVALID_HANDLE_VALUE) return;
    const DWORD lock_flags =
        LOCKFILE_FAIL_IMMEDIATELY |
        (mode == OpenMode::kWrite ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    const auto deadline = std::chrono::steady_clock::now() + kLockWait;
    for (;;) {
      OVERLAPPED ov{};
      ov.Offset = static_cast<DWORD>(kLockOffset & 0xFFFFFFFFu);
      ov.OffsetHigh = static_cast<DWORD>(kLockOffset >> 32);
      if (LockFileEx(h_, lock_flags, 0, 1, 0, &ov)) {
        locked_ = true;
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(kLockRetry);
    }
  }
  ~EpochFile() {
    if (h_ == INVALID_HANDLE_VALUE) return;
    if (locked_) {
      OVERLAPPED ov{};
      ov.Offset = static_cast<DWORD>(kLockOffset & 0xFFFFFFFFu);
      ov.OffsetHigh = static_cast<DWORD>(kLockOffset >> 32);
      UnlockFileEx(h_, 0, 1, 0, &ov);
    }
    CloseHandle(h_);
  }
  EpochFile(const EpochFile&) = delete;
  EpochFile& operator=(const EpochFile&) = delete;

  bool missing() const { return missing_; }
  bool created() const { return created_; }
  bool ready() const { return locked_; }

  // The record's bytes (at most kRecordBytes + 1, enough to see a longer
  // file), or nullopt on an I/O error.
  std::optional<std::string> ReadRecord() {
    char buf[kRecordBytes + 1];
    OVERLAPPED ov{};
    DWORD got = 0;
    if (!ReadFile(h_, buf, sizeof(buf), &got, &ov)) {
      if (GetLastError() != ERROR_HANDLE_EOF) return std::nullopt;
      got = 0;
    }
    return std::string(buf, got);
  }

  bool WriteRecord(const std::string& record) {
    OVERLAPPED ov{};
    DWORD wrote = 0;
    if (!WriteFile(h_, record.data(), static_cast<DWORD>(record.size()), &wrote,
                   &ov) ||
        wrote != record.size()) {
      return false;
    }
    // A longer foreign file being repaired keeps no tail past the record.
    LARGE_INTEGER end;
    end.QuadPart = static_cast<LONGLONG>(record.size());
    if (!SetFilePointerEx(h_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(h_)) {
      return false;
    }
    return FlushFileBuffers(h_) != 0;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  bool missing_ = false;
  bool created_ = false;
  bool locked_ = false;
};

#else  // !_WIN32

class EpochFile {
 public:
  EpochFile(const std::string& path, OpenMode mode) {
    if (mode == OpenMode::kWrite) {
      fd_ = ::open(path.c_str(),
                   O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0660);
      if (fd_ >= 0) {
        created_ = true;
        // Exact mode, whatever the umask: the cache group shares the file.
        (void)::fchmod(fd_, 0660);
        // Make the new directory entry durable, once: otherwise a power loss
        // right after the first write could lose the file (and read as 0).
        const std::string::size_type slash = path.rfind('/');
        const std::string dir = slash == std::string::npos
                                    ? std::string(".")
                                    : path.substr(0, slash == 0 ? 1 : slash);
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
          (void)::fsync(dfd);
          ::close(dfd);
        }
      } else if (errno == EEXIST) {
        fd_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
      }
    } else {
      fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd_ < 0) missing_ = errno == ENOENT;
    }
    if (fd_ < 0) return;
    struct flock fl{};
    fl.l_type = mode == OpenMode::kWrite ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = static_cast<off_t>(kLockOffset);
    fl.l_len = 1;
    const auto deadline = std::chrono::steady_clock::now() + kLockWait;
    for (;;) {
      if (::fcntl(fd_, F_SETLK, &fl) == 0) {
        locked_ = true;
        return;
      }
      if (errno != EAGAIN && errno != EACCES && errno != EINTR) return;
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(kLockRetry);
    }
  }
  ~EpochFile() {
    if (fd_ < 0) return;
    if (locked_) {
      struct flock fl{};
      fl.l_type = F_UNLCK;
      fl.l_whence = SEEK_SET;
      fl.l_start = static_cast<off_t>(kLockOffset);
      fl.l_len = 1;
      (void)::fcntl(fd_, F_SETLK, &fl);
    }
    ::close(fd_);
  }
  EpochFile(const EpochFile&) = delete;
  EpochFile& operator=(const EpochFile&) = delete;

  bool missing() const { return missing_; }
  bool created() const { return created_; }
  bool ready() const { return locked_; }

  std::optional<std::string> ReadRecord() {
    char buf[kRecordBytes + 1];
    size_t got = 0;
    while (got < sizeof(buf)) {
      const ssize_t n =
          ::pread(fd_, buf + got, sizeof(buf) - got, static_cast<off_t>(got));
      if (n < 0) {
        if (errno == EINTR) continue;
        return std::nullopt;
      }
      if (n == 0) break;
      got += static_cast<size_t>(n);
    }
    return std::string(buf, got);
  }

  bool WriteRecord(const std::string& record) {
    size_t written = 0;
    while (written < record.size()) {
      const ssize_t n =
          ::pwrite(fd_, record.data() + written, record.size() - written,
                   static_cast<off_t>(written));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      written += static_cast<size_t>(n);
    }
    // A longer foreign file being repaired keeps no tail past the record.
    if (::ftruncate(fd_, static_cast<off_t>(record.size())) != 0) return false;
    return ::fsync(fd_) == 0;
  }

 private:
  int fd_ = -1;
  bool missing_ = false;
  bool created_ = false;
  bool locked_ = false;
};

#endif  // _WIN32

}  // namespace

bool IsSitePurgeEpochSite(std::string_view site) {
  if (site.size() != 64) return false;
  for (char c : site) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

std::string SitePurgeEpochPath(std::string_view volume_path,
                               std::string_view site) {
  if (volume_path.empty() || !IsSitePurgeEpochSite(site)) return std::string();
  std::string out(volume_path);
  out += ".purge-epoch.";
  out += site;
  return out;
}

std::expected<uint64_t, SitePurgeEpochError> ParseSitePurgeEpoch(
    std::string_view content) {
  if (content.size() != kRecordBytes ||
      content.substr(0, kPrefix.size()) != kPrefix || content.back() != '\n') {
    return std::unexpected(SitePurgeEpochError::kCorrupted);
  }
  const std::string_view digits = content.substr(kPrefix.size(), kDigits);
  uint64_t value = 0;
  const auto [end, ec] =
      std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc() || end != digits.data() + digits.size()) {
    return std::unexpected(SitePurgeEpochError::kCorrupted);
  }
  return value;
}

std::string RenderSitePurgeEpoch(uint64_t epoch_seconds) {
  return Render(epoch_seconds);
}

std::expected<uint64_t, SitePurgeEpochError> ReadSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site) {
  const std::string path = SitePurgeEpochPath(volume_path, site);
  if (path.empty()) {
    return std::unexpected(SitePurgeEpochError::kInvalidArgument);
  }
  std::lock_guard in_process(ProcessEpochMutex());
  EpochFile file(path, OpenMode::kRead);
  if (file.missing()) return 0;
  if (!file.ready()) return std::unexpected(SitePurgeEpochError::kIo);
  auto record = file.ReadRecord();
  if (!record.has_value()) return std::unexpected(SitePurgeEpochError::kIo);
  // An empty file is one whose creator has not written yet, or died before
  // writing: nothing was recorded, the same as no file.  (A write to an
  // existing record never shortens it below the record.)
  if (record->empty()) return 0;
  return ParseSitePurgeEpoch(*record);
}

std::expected<void, SitePurgeEpochError> WriteSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site, uint64_t epoch_seconds,
    uint64_t now_seconds) {
  const std::string path = SitePurgeEpochPath(volume_path, site);
  if (path.empty()) {
    return std::unexpected(SitePurgeEpochError::kInvalidArgument);
  }
  if (epoch_seconds > now_seconds + kSitePurgeEpochSkewSeconds) {
    return std::unexpected(SitePurgeEpochError::kInvalidArgument);
  }
  std::lock_guard in_process(ProcessEpochMutex());
  EpochFile file(path, OpenMode::kWrite);
  if (!file.ready()) return std::unexpected(SitePurgeEpochError::kIo);
  // Always read under the lock, even when this call created the file: a
  // writer that opened it a moment later may have locked and written first.
  auto record = file.ReadRecord();
  if (!record.has_value()) return std::unexpected(SitePurgeEpochError::kIo);
  uint64_t to_write = epoch_seconds;
  if (!record->empty()) {
    auto current = ParseSitePurgeEpoch(*record);
    if (!current.has_value()) {
      // Torn, short or foreign: the stored value is unknowable, so it may
      // have been anything up to now. Replace it with the larger of the
      // caller's value and the present, which fences every entry stored so
      // far (fail closed) and heals the file.
      to_write = epoch_seconds > now_seconds ? epoch_seconds : now_seconds;
    } else if (epoch_seconds <= *current) {
      return {};
    }
  }
  if (!file.WriteRecord(Render(to_write))) {
    return std::unexpected(SitePurgeEpochError::kIo);
  }
  return {};
}

}  // namespace pagespeed
