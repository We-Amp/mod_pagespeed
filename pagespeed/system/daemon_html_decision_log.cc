// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_decision_log.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/system/daemon_abi.h"

namespace net_instaweb {

namespace {

constexpr size_t kMaxLogPathBytes = 300;
constexpr size_t kMaxLogHostBytes = 255;
// The formatter's bound on a vary or vhost_added value.
constexpr size_t kMaxLogListBytes = 1024;

// Longer than any line the field limits above allow.
constexpr size_t kLineBufferBytes = 4096;

// Lines waiting for the writer; past this a line is dropped and counted.
constexpr size_t kMaxQueuedLines = 1024;

// ps_html_store_classify's cc-evaluated out flag (PS_API 1.12), hand-copied
// for the reason daemon_abi.h gives.
constexpr uint32_t kPsHtmlStoreFlagCcEvaluated = 32;

bool IsAsciiSpaceOrTab(char c) { return c == ' ' || c == '\t'; }

bool IsTokenChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

bool IsToken(StringPiece s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c == '\0' || !IsTokenChar(c)) return false;
  }
  return true;
}

// Set by FailWriterStartForTesting.
std::atomic<bool> fail_writer_start_for_testing{false};

}  // namespace

// What the writer thread has to say to a server's message handler, held
// until a request path takes it.
class DaemonHtmlPendingMessages : public MessageHandler {
 public:
  std::vector<GoogleString> Take() {
    std::vector<GoogleString> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(messages_);
    return out;
  }

 protected:
  void MessageSImpl(MessageType /*type*/,
                    const GoogleString& message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    // The log reports a problem once per day; this bound is a backstop.
    if (messages_.size() < 16) messages_.push_back(message);
  }
  void FileMessageSImpl(MessageType type, const char* /*filename*/,
                        int /*line*/, const GoogleString& message) override {
    MessageSImpl(type, message);
  }

 private:
  std::mutex mutex_;
  std::vector<GoogleString> messages_;
};

// One per process: the logs by directory, and the thread that writes them.
class DaemonHtmlDecisionLogWriter {
 public:
  static DaemonHtmlDecisionLogWriter* Get() {
    // Never destroyed: the thread may still be waiting when the process exits.
    static DaemonHtmlDecisionLogWriter* const writer =
        new DaemonHtmlDecisionLogWriter;
    return writer;
  }

  std::shared_ptr<DaemonHtmlDecisionLog> Log(const GoogleString& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = logs_.begin(); it != logs_.end();) {
      it = it->second.expired() ? logs_.erase(it) : std::next(it);
    }
    std::weak_ptr<DaemonHtmlDecisionLog>& slot = logs_[dir];
    std::shared_ptr<DaemonHtmlDecisionLog> log = slot.lock();
    if (log == nullptr) {
      log = std::make_shared<DaemonHtmlDecisionLog>(dir);
      slot = log;
    }
    return log;
  }

  // False when the queue is full or the thread could not be started.
  bool Enqueue(std::shared_ptr<DaemonHtmlDecisionLog> log, GoogleString key,
               GoogleString line, int64 now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueuedLines) return false;
    if (!started_) {
      // Start the thread first, so a failed start leaves the next line to try
      // again; this line is dropped.
      if (!StartThread()) return false;
      started_ = true;
    }
    queue_.push_back(
        Item{std::move(log), std::move(key), std::move(line), now_ms});
    wake_.notify_one();
    return true;
  }

  void WaitUntilIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !writing_; });
  }

 private:
  struct Item {
    std::shared_ptr<DaemonHtmlDecisionLog> log;
    GoogleString key;
    GoogleString line;
    int64 now_ms;
  };

  bool StartThread() {
    if (fail_writer_start_for_testing.load(std::memory_order_relaxed)) {
      return false;
    }
    try {
      std::thread([this] { Run(); }).detach();
    } catch (const std::exception&) {
      return false;
    }
    return true;
  }

  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      wake_.wait(lock, [this] { return !queue_.empty(); });
      Item item = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
      lock.unlock();
      if (item.log->log_.Write(item.key, item.line, item.now_ms,
                               item.log->messages_.get()) ==
          RulePackDecisionLog::Result::kDropped) {
        item.log->dropped_.fetch_add(1, std::memory_order_relaxed);
      }
      // The last reference closes the file, outside the lock.
      item.log.reset();
      lock.lock();
      writing_ = false;
      if (queue_.empty()) idle_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::deque<Item> queue_;
  bool started_ = false;
  bool writing_ = false;
  std::map<GoogleString, std::weak_ptr<DaemonHtmlDecisionLog>> logs_;
};

DaemonHtmlDecisionLog::DaemonHtmlDecisionLog(const GoogleString& dir)
    : log_(dir, "DaemonHtmlCache decision log"),
      messages_(new DaemonHtmlPendingMessages) {
  log_.set_max_lines_per_second(kMaxLinesPerSecond);
}

DaemonHtmlDecisionLog::~DaemonHtmlDecisionLog() {}

std::shared_ptr<DaemonHtmlDecisionLog> DaemonHtmlDecisionLog::ForLogDir(
    StringPiece log_dir) {
  return DaemonHtmlDecisionLogWriter::Get()->Log(
      StrCat(log_dir, "/daemon-html"));
}

bool DaemonHtmlDecisionLog::Queue(const DaemonAbi& abi, const Fields& fields,
                                  const GoogleString& dedup_key,
                                  int64 now_ms) {
  std::vector<const char*> keys;
  std::vector<const char*> values;
  keys.reserve(fields.size());
  values.reserve(fields.size());
  for (const auto& [key, value] : fields) {
    if (value.empty()) continue;  // an absent fact is an absent key
    keys.push_back(key);
    values.push_back(value.c_str());
  }
  char line[kLineBufferBytes];
  size_t line_len = 0;
  if (abi.HtmlDecisionFormat(keys.data(), values.data(), keys.size(), line,
                             sizeof(line), &line_len) != 0) {
    return false;
  }
  return DaemonHtmlDecisionLogWriter::Get()->Enqueue(
      shared_from_this(), dedup_key, GoogleString(line, line_len), now_ms);
}

int64 DaemonHtmlDecisionLog::TakeDropped() {
  return dropped_.exchange(0, std::memory_order_relaxed);
}

std::vector<GoogleString> DaemonHtmlDecisionLog::TakeMessages() {
  return messages_->Take();
}

void DaemonHtmlDecisionLog::WaitForWritesForTesting() {
  DaemonHtmlDecisionLogWriter::Get()->WaitUntilIdle();
}

void DaemonHtmlDecisionLog::FailWriterStartForTesting(bool fail) {
  fail_writer_start_for_testing.store(fail, std::memory_order_relaxed);
}

GoogleString DaemonHtmlLogPath(StringPiece path) {
  size_t end = 0;
  while (end < path.size() && end < kMaxLogPathBytes) {
    const char c = path[end];
    if (c == '?' || c == '#' || c == ';' ||
        static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      break;
    }
    ++end;
  }
  return GoogleString(path.substr(0, end));
}

bool DaemonHtmlIsLogHost(StringPiece host) {
  if (host.empty() || host.size() > kMaxLogHostBytes) return false;
  for (char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                    c == '_' || c == ':' || c == '[' || c == ']';
    if (!ok) return false;
  }
  return true;
}

GoogleString DaemonHtmlLogVary(const DaemonHtmlLogHeaderLines& headers) {
  GoogleString out;
  for (const auto& [name, value] : headers) {
    if (!StringCaseEqual(name, "Vary")) continue;
    if (!out.empty()) out.append(",");
    out.append(value);
  }
  if (out.size() > kMaxLogListBytes) return GoogleString();
  bool in_token = false;
  for (char c : out) {
    if (c == ',' || IsAsciiSpaceOrTab(c)) {
      in_token = false;
    } else if (c != '\0' && IsTokenChar(c)) {
      in_token = true;
    } else {
      return GoogleString();
    }
  }
  if (!in_token) return GoogleString();
  return out;
}

GoogleString DaemonHtmlLogCc(uint16 cc_flags, uint32 store_flags) {
  GoogleString out;
  if ((store_flags & kPsHtmlStoreFlagCcEvaluated) == 0 ||
      (cc_flags & kPsCcOriginHeaderPresent) == 0) {
    return out;
  }
  static constexpr struct {
    uint16_t bit;
    const char* name;
  } kNames[] = {
      {0x0001, "no-cache"},
      {0x0002, "must-revalidate"},
      {kPsCcOriginNoStore, "no-store"},
      {0x0008, "private"},
      {0x0010, "public"},
      {0x0020, "immutable"},
      {0x0080, "proxy-revalidate"},
      {kPsCcOriginNoTransform, "no-transform"},
  };
  for (const auto& entry : kNames) {
    if ((cc_flags & entry.bit) == 0) continue;
    if (!out.empty()) out.push_back(',');
    out.append(entry.name);
  }
  return out;
}

GoogleString DaemonHtmlLogValidators(const DaemonHtmlLogHeaderLines& headers) {
  bool etag = false;
  bool last_modified = false;
  for (const auto& [name, value] : headers) {
    if (value.empty()) continue;
    if (StringCaseEqual(name, "ETag")) etag = true;
    if (StringCaseEqual(name, "Last-Modified")) last_modified = true;
  }
  return etag && last_modified ? "etag,last_modified"
         : etag                ? "etag"
         : last_modified       ? "last_modified"
                               : "none";
}

GoogleString DaemonHtmlVhostAdded(const DaemonHtmlLogHeaderLines& store_view,
                                  const DaemonHtmlLogHeaderLines& final_view) {
  // The final view's lines that the store view does not hold, one for one,
  // so a second copy of a line counts as added.
  std::vector<bool> used(store_view.size(), false);
  std::vector<GoogleString> names;
  for (const auto& line : final_view) {
    bool matched = false;
    for (size_t i = 0; i < store_view.size(); ++i) {
      if (!used[i] && StringCaseEqual(store_view[i].first, line.first) &&
          store_view[i].second == line.second) {
        used[i] = true;
        matched = true;
        break;
      }
    }
    if (matched || !IsToken(line.first)) continue;
    GoogleString name(line.first);
    LowerString(&name);
    bool seen = false;
    for (const GoogleString& n : names) seen = seen || n == name;
    if (!seen) names.push_back(name);
  }
  GoogleString out = JoinCollection(names, ",");
  if (out.size() > kMaxLogListBytes) return GoogleString();
  return out;
}

}  // namespace net_instaweb
