// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_RULE_PACK_DECISION_LOG_H_
#define PAGESPEED_SYSTEM_RULE_PACK_DECISION_LOG_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace net_instaweb {

class MessageHandler;

// The decision log of one rule pack in one process: JSON lines appended to
// <dir>/decisions-YYYY-MM-DD.jsonl (UTC date), where <dir> is
// <LogDir>/packs/<pack-id>.
//
// Every process of a server (Apache children, nginx workers, IIS worker
// processes) appends to the same file. Each line goes out in one write on a
// descriptor opened for appending (O_APPEND; FILE_APPEND_DATA on Windows),
// and lines are short, so lines from different processes do not interleave
// on a local file system. A new file is opened when the UTC date changes;
// files are never renamed, so there is no cross-process rotation race.
//
// Per process, a line is written at most once per dedup key (the caller
// builds the key from host, path, rule, pack hash, mode, action and reason).
// The key set is bounded; when it is full it is cleared, so the contract is
// "at most once per key per process lifetime, give or take a full set". N
// processes may write the same line N times.
//
// A day's file is capped at about kMaxBytesPerDay across all processes: each
// process checks the size when it opens the file and every kStatEveryLines
// lines it writes, and stops writing to that file once the cap is reached.
// The process that creates a day's file deletes the files older than
// kRetentionDays days in the same directory.
//
// Thread-safe.
class RulePackDecisionLog {
 public:
  static constexpr int64_t kMaxBytesPerDay = int64_t{50} * 1024 * 1024;
  static constexpr int kStatEveryLines = 256;
  static constexpr size_t kMaxDedupKeys = 65536;
  static constexpr int kRetentionDays = 14;

  enum class Result {
    kWritten,    // the line was appended
    kDuplicate,  // this process already wrote a line with the same key
    kDropped,    // not written: the day's cap, or the file cannot be written
  };

  explicit RulePackDecisionLog(std::string dir);
  ~RulePackDecisionLog();

  RulePackDecisionLog(const RulePackDecisionLog&) = delete;
  RulePackDecisionLog& operator=(const RulePackDecisionLog&) = delete;

  // Appends `line` and a newline to the file of the UTC day of `now_ms`,
  // unless a line with `dedup_key` was written by this process already.
  // Problems with the directory or the file are reported to `handler` once
  // per day.
  Result Write(std::string_view dedup_key, std::string_view line,
               int64_t now_ms, MessageHandler* handler);

  const std::string& dir() const { return dir_; }

  // "YYYY-MM-DD" of the UTC day `ms` (milliseconds since the epoch) falls in.
  static std::string UtcDate(int64_t ms);
  // The file name of a day: "decisions-YYYY-MM-DD.jsonl".
  static std::string FileName(std::string_view date);

  void set_max_bytes_per_day_for_testing(int64_t bytes) {
    max_bytes_per_day_ = bytes;
  }

 private:
  void Open(const std::string& date, int64_t now_ms, MessageHandler* handler);
  void Close();
  // Deletes the day files of this directory older than kRetentionDays days
  // before `now_ms`.
  void RemoveExpired(int64_t now_ms);
  // Current size of the open file, or -1.
  int64_t FileSize() const;
  bool Append(std::string_view data);

  const std::string dir_;
  int64_t max_bytes_per_day_ = kMaxBytesPerDay;

  std::mutex mutex_;
  std::string date_;  // the day of the open file (or of the failed open)
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
  bool capped_ = false;
  int lines_since_stat_ = 0;
  std::unordered_set<size_t> seen_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_RULE_PACK_DECISION_LOG_H_
