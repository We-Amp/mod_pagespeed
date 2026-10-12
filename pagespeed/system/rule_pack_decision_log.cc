// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_decision_log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

#include "pagespeed/kernel/base/message_handler.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace net_instaweb {

namespace {

constexpr int64_t kMsPerDay = 24LL * 60 * 60 * 1000;
constexpr std::string_view kFilePrefix = "decisions-";
constexpr std::string_view kFileSuffix = ".jsonl";

// A UTF-8 path, also on Windows (where a narrow std::string would be read in
// the ANSI code page).
std::filesystem::path Utf8Path(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// True for "YYYY-MM-DD" made of digits and dashes.
bool IsDate(std::string_view s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

}  // namespace

RulePackDecisionLog::RulePackDecisionLog(std::string dir)
    : dir_(std::move(dir)) {}

RulePackDecisionLog::~RulePackDecisionLog() { Close(); }

std::string RulePackDecisionLog::UtcDate(int64_t ms) {
  // Days since 1970-01-01, rounded down, then the civil date (the
  // proleptic Gregorian calendar; H. Hinnant's days-to-civil algorithm).
  int64_t days = ms / kMsPerDay;
  if (ms % kMsPerDay < 0) --days;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) ++y;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld",
                static_cast<long long>(y), static_cast<long long>(m),
                static_cast<long long>(d));
  return buf;
}

std::string RulePackDecisionLog::FileName(std::string_view date) {
  std::string name(kFilePrefix);
  name.append(date);
  name.append(kFileSuffix);
  return name;
}

void RulePackDecisionLog::Close() {
#ifdef _WIN32
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
#endif
}

int64_t RulePackDecisionLog::FileSize() const {
#ifdef _WIN32
  if (handle_ == nullptr) return -1;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &size)) return -1;
  return static_cast<int64_t>(size.QuadPart);
#else
  if (fd_ < 0) return -1;
  struct stat st;
  if (fstat(fd_, &st) != 0) return -1;
  return static_cast<int64_t>(st.st_size);
#endif
}

bool RulePackDecisionLog::Append(std::string_view data) {
#ifdef _WIN32
  DWORD written = 0;
  return WriteFile(static_cast<HANDLE>(handle_), data.data(),
                   static_cast<DWORD>(data.size()), &written, nullptr) &&
         written == data.size();
#else
  // One write per line: a partial write would split a line, so it counts
  // as a failure and is not retried.
  ssize_t n;
  do {
    n = write(fd_, data.data(), data.size());
  } while (n < 0 && errno == EINTR);
  return n == static_cast<ssize_t>(data.size());
#endif
}

void RulePackDecisionLog::RemoveExpired(int64_t now_ms) {
  // Keep today and the kRetentionDays - 1 days before it.
  const std::string oldest_kept =
      UtcDate(now_ms - (kRetentionDays - 1) * kMsPerDay);
  std::error_code ec;
  std::filesystem::directory_iterator it(Utf8Path(dir_), ec);
  if (ec) return;
  for (const std::filesystem::directory_entry& entry : it) {
    const std::u8string u8 = entry.path().filename().u8string();
    const std::string name(u8.begin(), u8.end());
    if (name.size() != kFilePrefix.size() + 10 + kFileSuffix.size() ||
        name.compare(0, kFilePrefix.size(), kFilePrefix) != 0 ||
        name.compare(name.size() - kFileSuffix.size(), kFileSuffix.size(),
                     kFileSuffix) != 0) {
      continue;
    }
    const std::string_view date =
        std::string_view(name).substr(kFilePrefix.size(), 10);
    if (!IsDate(date) || date >= oldest_kept) continue;
    std::error_code remove_ec;
    // Another process may have removed it already; that is fine.
    std::filesystem::remove(entry.path(), remove_ec);
  }
}

void RulePackDecisionLog::Open(const std::string& date, int64_t now_ms,
                               MessageHandler* handler) {
  Close();
  date_ = date;
  capped_ = false;
  lines_since_stat_ = 0;

  std::error_code ec;
  const std::filesystem::path dir = Utf8Path(dir_);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (handler != nullptr) {
      handler->Message(kWarning,
                       "rule pack decision log: cannot create %s: %s; "
                       "decisions of this day are not logged by this process",
                       dir_.c_str(), ec.message().c_str());
    }
    return;
  }
  const std::filesystem::path file = dir / Utf8Path(FileName(date));
  bool created = false;
#ifdef _WIN32
  // FILE_APPEND_DATA without FILE_WRITE_DATA makes every write an append at
  // the end of the file, whichever process writes.
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, share, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    created = true;
  } else if (GetLastError() == ERROR_FILE_EXISTS) {
    h = CreateFileW(file.c_str(), FILE_APPEND_DATA, share, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (h == INVALID_HANDLE_VALUE) {
    if (handler != nullptr) {
      handler->Message(kWarning,
                       "rule pack decision log: cannot open %s/%s (error "
                       "%lu); decisions of this day are not logged by this "
                       "process",
                       dir_.c_str(), FileName(date).c_str(),
                       static_cast<unsigned long>(GetLastError()));
    }
    return;
  }
  handle_ = h;
#else
  const int flags = O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC;
  int fd;
  do {
    fd = open(file.c_str(), flags | O_EXCL, 0640);
  } while (fd < 0 && errno == EINTR);
  if (fd >= 0) {
    created = true;
  } else if (errno == EEXIST) {
    do {
      fd = open(file.c_str(), flags, 0640);
    } while (fd < 0 && errno == EINTR);
  }
  if (fd < 0) {
    if (handler != nullptr) {
      handler->Message(kWarning,
                       "rule pack decision log: cannot open %s/%s: %s; "
                       "decisions of this day are not logged by this process",
                       dir_.c_str(), FileName(date).c_str(),
                       std::strerror(errno));
    }
    return;
  }
  fd_ = fd;
#endif
  if (created) RemoveExpired(now_ms);
  capped_ = FileSize() >= max_bytes_per_day_;
}

RulePackDecisionLog::Result RulePackDecisionLog::Write(
    std::string_view dedup_key, std::string_view line, int64_t now_ms,
    MessageHandler* handler) {
  const size_t key = std::hash<std::string_view>()(dedup_key);
  std::lock_guard<std::mutex> lock(mutex_);
  if (seen_.count(key) != 0) return Result::kDuplicate;

  const std::string date = UtcDate(now_ms);
  if (date != date_) Open(date, now_ms, handler);
#ifdef _WIN32
  const bool open = handle_ != nullptr;
#else
  const bool open = fd_ >= 0;
#endif
  if (!open || capped_) return Result::kDropped;
  if (++lines_since_stat_ >= kStatEveryLines) {
    lines_since_stat_ = 0;
    capped_ = FileSize() >= max_bytes_per_day_;
    if (capped_) return Result::kDropped;
  }

  std::string data;
  data.reserve(line.size() + 1);
  data.append(line);
  data.push_back('\n');
  if (!Append(data)) {
    if (handler != nullptr) {
      handler->Message(kWarning,
                       "rule pack decision log: a write to %s/%s failed; "
                       "decisions of this day are not logged by this process",
                       dir_.c_str(), FileName(date_).c_str());
    }
    Close();
    return Result::kDropped;
  }
  if (seen_.size() >= kMaxDedupKeys) seen_.clear();
  seen_.insert(key);
  return Result::kWritten;
}

}  // namespace net_instaweb
