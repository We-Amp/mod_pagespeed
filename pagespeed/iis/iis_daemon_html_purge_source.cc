// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_daemon_html_purge_source.h"

#include <algorithm>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

namespace {

// The purge context's own rule for a record's time.
constexpr size_t kMaxPurgeBytes = 1 << 20;

bool ValidTimestamp(StringPiece text, int64 now_ms, int64* out) {
  if (!StringToInt64(text, out)) return false;
  return *out == PurgeSet::kInitialTimestampMs ||
         (*out >= 0 && *out <= now_ms + PurgeSet::kClockSkewAllowanceMs);
}

}  // namespace

IisDaemonHtmlPurgeSource::IisDaemonHtmlPurgeSource(
    const DaemonHtmlPurgeSource* system, FileSystem* file_system, Timer* timer,
    StringPiece purge_file)
    : system_(system),
      file_system_(file_system),
      timer_(timer),
      purge_file_(purge_file.as_string()) {}

void IisDaemonHtmlPurgeSource::Refresh() const {
  NullMessageHandler handler;
  const int64 now_ms = timer_->NowMs();
  int64 mtime_s = -1;
  int64 size = -1;
  if (!file_system_->Mtime(purge_file_, &mtime_s, &handler)) mtime_s = -1;
  if (!file_system_->Size(purge_file_, &size, &handler)) size = -1;
  std::lock_guard<std::mutex> lock(mutex_);
  if (mtime_s == mtime_s_ && size == size_ && read_ms_ >= 0 &&
      now_ms - read_ms_ < Timer::kSecondMs) {
    return;
  }
  // As large as the file can be (the purge context bounds it).
  PurgeSet fresh(kMaxPurgeBytes);
  GoogleString buffer;
  bool parsed = false;
  if (mtime_s >= 0 &&
      file_system_->ReadFile(purge_file_.c_str(), &buffer, &handler)) {
    StringPieceVector lines;
    SplitStringPieceToVector(buffer, "\n", &lines, true);
    int64 timestamp_ms = 0;
    if (!lines.empty() && ValidTimestamp(lines[0], now_ms, &timestamp_ms)) {
      parsed = true;
      fresh.UpdateGlobalInvalidationTimestampMs(timestamp_ms);
      for (size_t i = 1; i < lines.size(); ++i) {
        const StringPiece line = lines[i];
        const size_t space = line.find(' ');
        if (space == StringPiece::npos ||
            !ValidTimestamp(line.substr(0, space), now_ms, &timestamp_ms)) {
          continue;
        }
        fresh.Put(line.substr(space + 1).as_string(), timestamp_ms);
      }
    }
  }
  if (!parsed) {
    // A missing, unreadable or torn file says nothing: the last view stays,
    // and the next decision reads again.
    return;
  }
  mtime_s_ = mtime_s;
  size_ = size;
  read_ms_ = now_ms;
  from_file_.Swap(&fresh);
}

bool IisDaemonHtmlPurgeSource::IsUrlValidIgnoringGlobal(const GoogleString& url,
                                                        int64 time_ms) const {
  if (!system_->IsUrlValidIgnoringGlobal(url, time_ms)) return false;
  Refresh();
  std::lock_guard<std::mutex> lock(mutex_);
  return from_file_.IsValidIgnoringGlobal(url, time_ms);
}

int64 IisDaemonHtmlPurgeSource::GlobalInvalidationTimestampMs() const {
  const int64 system = system_->GlobalInvalidationTimestampMs();
  Refresh();
  std::lock_guard<std::mutex> lock(mutex_);
  return std::max(system, from_file_.global_invalidation_timestamp_ms());
}

bool IisDaemonHtmlPurgeSource::FlushFileMtimeMs(int64* mtime_ms) const {
  return system_->FlushFileMtimeMs(mtime_ms);
}

}  // namespace net_instaweb
