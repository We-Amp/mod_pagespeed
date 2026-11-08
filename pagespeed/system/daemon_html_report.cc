// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_report.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/rule_pack_decision_log.h"

namespace net_instaweb {

namespace {

// The mode, the action values and the flag bits of the library's published
// policy API (PS_API 1.12), hand-copied for the reason daemon_abi.h gives.
constexpr int kPsHtmlCacheReportOnly = 1;
constexpr int kPsHtmlStoreActionWouldStore = 3;
constexpr int kPsHtmlStoreActionWouldStoreNoNotify = 4;
constexpr uint32_t kPsHtmlStoreFlagSidecarEvaluated = 16;
constexpr uint32_t kPsHtmlStoreFlagCcEvaluated = 32;
constexpr uint32_t kPsHtmlRequestHasAuthorization = 1;
constexpr uint32_t kPsHtmlRequestAuthenticatedUser = 2;
constexpr uint32_t kPsHtmlRequestHasCookie = 4;
constexpr uint32_t kPsHtmlRequestNoStore = 8;

constexpr int kSidecarEligible = 0;
constexpr int kSidecarFallThrough = 1;
constexpr int kSidecarNever = 2;

constexpr size_t kMaxLogPathBytes = 300;
constexpr size_t kMaxLogHostBytes = 255;
constexpr size_t kMaxLogVaryBytes = 1024;

// The library caps a detail at 128 bytes.
constexpr size_t kDetailBufferBytes = 160;

// Longer than any line the field limits above allow.
constexpr size_t kLineBufferBytes = 4096;

bool IsAsciiSpaceOrTab(char c) { return c == ' ' || c == '\t'; }

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

bool HasToken(std::string_view value, std::string_view token) {
  // A comma-separated list; a directive's own argument ends at '='.
  while (true) {
    const size_t comma = value.find(',');
    std::string_view item = value.substr(0, comma);
    const size_t equals = item.find('=');
    if (equals != std::string_view::npos) item = item.substr(0, equals);
    while (!item.empty() && IsAsciiSpaceOrTab(item.front())) {
      item.remove_prefix(1);
    }
    while (!item.empty() && IsAsciiSpaceOrTab(item.back())) {
      item.remove_suffix(1);
    }
    if (EqualsIgnoreCase(item, token)) return true;
    if (comma == std::string_view::npos) return false;
    value.remove_prefix(comma + 1);
  }
}

// The tokens of the Vary lines among `lines`, in order, each once (compared
// case-insensitively), without Accept-Encoding when `skip_accept_encoding`.
std::vector<std::string> VaryTokens(
    const std::vector<std::pair<std::string, std::string>>& lines,
    bool skip_accept_encoding) {
  std::vector<std::string> tokens;
  for (const auto& [name, value] : lines) {
    if (!EqualsIgnoreCase(name, "Vary")) continue;
    std::string_view rest(value);
    while (true) {
      const size_t comma = rest.find(',');
      std::string_view item = rest.substr(0, comma);
      while (!item.empty() && IsAsciiSpaceOrTab(item.front())) {
        item.remove_prefix(1);
      }
      while (!item.empty() && IsAsciiSpaceOrTab(item.back())) {
        item.remove_suffix(1);
      }
      const bool skip =
          item.empty() ||
          (skip_accept_encoding && EqualsIgnoreCase(item, "Accept-Encoding"));
      if (!skip && std::none_of(tokens.begin(), tokens.end(),
                                [item](const std::string& token) {
                                  return EqualsIgnoreCase(token, item);
                                })) {
        tokens.emplace_back(item);
      }
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  return tokens;
}

bool ContainsToken(const std::vector<std::string>& tokens,
                   std::string_view token) {
  return std::any_of(tokens.begin(), tokens.end(),
                     [token](const std::string& other) {
                       return EqualsIgnoreCase(other, token);
                     });
}

// An ETag without the -gzip suffix compression adds to it, inside or after
// the closing quote.
std::string EtagWithoutGzipSuffix(const std::string& value) {
  static constexpr std::string_view kQuoted = "-gzip\"";
  static constexpr std::string_view kBare = "-gzip";
  const std::string_view view(value);
  if (view.size() > kQuoted.size() &&
      view.substr(view.size() - kQuoted.size()) == kQuoted) {
    return std::string(view.substr(0, view.size() - kQuoted.size())) + '"';
  }
  if (view.size() > kBare.size() &&
      view.substr(view.size() - kBare.size()) == kBare) {
    return std::string(view.substr(0, view.size() - kBare.size()));
  }
  return value;
}

// The cache-key path as the log may show it: up to a query, a fragment, path
// parameters or a control byte, so a query string is never written anywhere.
// Percent-escapes stay, so /caf%C3%A9.html and /caf%C3%A8.html are two paths.
std::string LogPath(std::string_view path) {
  size_t end = 0;
  while (end < path.size() && end < kMaxLogPathBytes) {
    const char c = path[end];
    if (c == '?' || c == '#' || c == ';' ||
        static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      break;
    }
    ++end;
  }
  return std::string(path.substr(0, end));
}

bool IsLogHost(std::string_view host) {
  if (host.empty() || host.size() > kMaxLogHostBytes) return false;
  for (char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                    c == '_' || c == ':' || c == '[' || c == ']';
    if (!ok) return false;
  }
  return true;
}

bool IsTokenChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

// The origin's Vary lines as a token list, or empty when a line holds anything
// else: the value is the origin's to choose, so it is logged only when it is
// plainly a list of names.
std::string LogVary(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  std::string out;
  for (const auto& [name, value] : headers) {
    if (!EqualsIgnoreCase(name, "Vary")) continue;
    if (!out.empty()) out.append(",");
    out.append(value);
  }
  if (out.size() > kMaxLogVaryBytes) return std::string();
  bool in_token = false;
  for (char c : out) {
    if (c == ',' || IsAsciiSpaceOrTab(c)) {
      in_token = false;
    } else if (c != '\0' && IsTokenChar(c)) {
      in_token = true;
    } else {
      return std::string();
    }
  }
  if (!in_token) return std::string();
  return out;
}

std::string LogCc(uint16_t flags, uint32_t lifetime_flags) {
  std::string out;
  if ((lifetime_flags & kPsHtmlStoreFlagCcEvaluated) == 0 ||
      (flags & kPsCcOriginHeaderPresent) == 0) {
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
    if ((flags & entry.bit) == 0) continue;
    if (!out.empty()) out.push_back(',');
    out.append(entry.name);
  }
  return out;
}

}  // namespace

// What the writer thread has to say to a server's message handler, handed on
// from the request path: the thread never calls a handler, whose server may be
// gone by the time a queued line is written.
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

// The decision log of one LogDir in this process, shared by every reporter
// that logs there. Only the writer thread calls log.Write.
class DaemonHtmlDecisionLog {
 public:
  explicit DaemonHtmlDecisionLog(const GoogleString& dir)
      : log(dir, "DaemonHtmlCache decision log") {
    log.set_max_lines_per_second(DaemonHtmlReporter::kMaxLinesPerSecond);
  }

  RulePackDecisionLog log;
  DaemonHtmlPendingMessages messages;
  std::atomic<int64_t> dropped{0};
};

namespace {

// Set by FailLogWriterStartForTesting.
std::atomic<bool> fail_writer_start_for_testing{false};

// Lines waiting for the writer; past this a line is dropped and counted.
constexpr size_t kMaxQueuedLines = 1024;

// One per process: the logs by directory, and the thread that writes them.
// Opening a day's file, creating its directory, deleting expired files and
// every write happen on that thread, so a request (an nginx event loop
// included) only formats a line and queues it. The thread starts with the
// first line a process logs; the parent of a forking server never logs, so
// each child starts its own. Lines still queued when the process exits are
// lost.
class DecisionLogWriter {
 public:
  static DecisionLogWriter* Get() {
    // Never destroyed: the thread may still be waiting when the process exits.
    static DecisionLogWriter* const writer = new DecisionLogWriter;
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

  // False when the queue is full.
  bool Enqueue(std::shared_ptr<DaemonHtmlDecisionLog> log, GoogleString key,
               GoogleString line, int64_t now_ms) {
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
    int64_t now_ms;
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
      if (item.log->log.Write(item.key, item.line, item.now_ms,
                              &item.log->messages) ==
          RulePackDecisionLog::Result::kDropped) {
        item.log->dropped.fetch_add(1, std::memory_order_relaxed);
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

}  // namespace

DaemonHtmlCapture::DaemonHtmlCapture(DaemonHtmlReporter* reporter,
                                     const DaemonAbi* abi,
                                     int64_t max_body_bytes)
    : reporter_(reporter), abi_(abi), max_body_bytes_(max_body_bytes) {}

DaemonHtmlCapture::~DaemonHtmlCapture() {}

void DaemonHtmlCapture::SetRequest(StringPiece method, StringPiece url_path,
                                   StringPiece host) {
  method_.assign(method.data(), method.size());
  url_path_.assign(url_path.data(), url_path.size());
  host_.assign(host.data(), host.size());
}

void DaemonHtmlCapture::AddRequestCacheControl(StringPiece value) {
  if (HasToken(std::string_view(value.data(), value.size()), "no-store")) {
    request_no_store_ = true;
  }
}

void DaemonHtmlCapture::AddHeader(StringPiece name, StringPiece value) {
  headers_.emplace_back(std::string(name.data(), name.size()),
                        std::string(value.data(), value.size()));
}

void DaemonHtmlCapture::SetBaselineHeaders(HeaderLines lines) {
  baseline_ = std::move(lines);
  has_baseline_ = true;
}

void DaemonHtmlCapture::ApplyLateHeaders(
    const HeaderLines& lines, const std::vector<StringPiece>& ignored) {
  if (!has_baseline_) return;
  has_baseline_ = false;
  const auto is_ignored = [&ignored](const std::string& name) {
    for (const StringPiece& other : ignored) {
      if (EqualsIgnoreCase(name,
                           std::string_view(other.data(), other.size()))) {
        return true;
      }
    }
    return false;
  };
  // Compression adds Accept-Encoding to Vary and a -gzip suffix to an ETag,
  // neither of which the cache would store; any other change to them is the
  // server's and counts. Vary is compared as a set of tokens, apart from the
  // other lines.
  const bool vary_counts = !is_ignored("Vary");
  const auto skipped = [&](const std::string& name) {
    return is_ignored(name) || (vary_counts && EqualsIgnoreCase(name, "Vary"));
  };
  const auto value_of = [](const std::pair<std::string, std::string>& line) {
    return EqualsIgnoreCase(line.first, "ETag")
               ? EtagWithoutGzipSuffix(line.second)
               : line.second;
  };
  const auto same = [&value_of](const std::pair<std::string, std::string>& a,
                                const std::pair<std::string, std::string>& b) {
    return EqualsIgnoreCase(a.first, b.first) && value_of(a) == value_of(b);
  };
  // Lines are matched one to one, so a repeated line counts as often as it
  // appears.
  std::vector<bool> kept(baseline_.size(), false);
  HeaderLines added;
  for (const auto& line : lines) {
    if (skipped(line.first)) continue;
    bool found = false;
    for (size_t i = 0; i < baseline_.size(); ++i) {
      if (!kept[i] && same(baseline_[i], line)) {
        kept[i] = true;
        found = true;
        break;
      }
    }
    if (!found) added.emplace_back(line.first, value_of(line));
  }
  for (size_t i = 0; i < baseline_.size(); ++i) {
    if (kept[i] || skipped(baseline_[i].first)) continue;
    for (auto it = headers_.begin(); it != headers_.end(); ++it) {
      if (same(*it, baseline_[i])) {
        headers_.erase(it);
        break;
      }
    }
  }
  if (vary_counts) {
    const std::vector<std::string> before = VaryTokens(baseline_, true);
    const std::vector<std::string> after = VaryTokens(lines, true);
    std::vector<std::string> gone;
    for (const std::string& token : before) {
      if (!ContainsToken(after, token)) gone.push_back(token);
    }
    std::vector<std::string> fresh;
    for (const std::string& token : after) {
      if (!ContainsToken(before, token)) fresh.push_back(token);
    }
    if (!gone.empty() || !fresh.empty()) {
      // The captured Vary becomes one line: the origin's tokens less those
      // the server took away, then those it added.
      std::vector<std::string> tokens;
      for (std::string& token : VaryTokens(headers_, false)) {
        if (!ContainsToken(gone, token)) tokens.push_back(std::move(token));
      }
      for (std::string& token : fresh) {
        if (!ContainsToken(tokens, token)) tokens.push_back(std::move(token));
      }
      headers_.erase(
          std::remove_if(headers_.begin(), headers_.end(),
                         [](const std::pair<std::string, std::string>& line) {
                           return EqualsIgnoreCase(line.first, "Vary");
                         }),
          headers_.end());
      if (!tokens.empty()) {
        GoogleString value;
        for (const std::string& token : tokens) {
          if (!value.empty()) value.append(", ");
          value.append(token);
        }
        added.emplace_back("Vary", value);
      }
    }
  }
  for (auto& line : added) headers_.push_back(std::move(line));
  HeaderLines().swap(baseline_);
}

void DaemonHtmlCapture::AddBody(const char* data, size_t size) {
  body_bytes_ += size;
  if (body_prefix_.size() < kBodyWindowBytes) {
    body_prefix_.append(data,
                        std::min(size, kBodyWindowBytes - body_prefix_.size()));
  }
}

void DaemonHtmlCapture::Finish(bool aborted) {
  if (finished_) return;
  finished_ = true;
  reporter_->Report(*this, aborted);
}

DaemonHtmlReporter::DaemonHtmlReporter(Statistics* statistics, Timer* timer,
                                       StringPiece log_dir,
                                       MessageHandler* handler)
    : timer_(timer),
      handler_(handler),
      evaluated_(statistics->GetVariable("daemon_html_evaluated")),
      would_store_(statistics->GetVariable("daemon_html_would_store")),
      would_store_no_notify_(
          statistics->GetVariable("daemon_html_would_store_no_notify")),
      would_store_bytes_(
          statistics->GetVariable("daemon_html_would_store_bytes")),
      refused_(statistics->GetVariable("daemon_html_refused")),
      refused_other_(statistics->GetVariable("daemon_html_refused_other")),
      unavailable_(statistics->GetVariable("daemon_html_unavailable")),
      classify_failed_(statistics->GetVariable("daemon_html_classify_failed")),
      log_dropped_(statistics->GetVariable("daemon_html_log_dropped")) {
  for (const char* const* reason = RewriteStats::kDaemonHtmlRefusedReasons;
       *reason != nullptr; ++reason) {
    refused_by_reason_[*reason] =
        statistics->GetVariable(StrCat("daemon_html_refused_", *reason));
  }
  if (log_dir.empty()) {
    handler_->Message(kWarning,
                      "DaemonHtmlCache decision log is off: LogDir is not set");
  } else {
    log_ = DecisionLogWriter::Get()->Log(StrCat(log_dir, "/daemon-html"));
  }
}

DaemonHtmlReporter::~DaemonHtmlReporter() {}

void DaemonHtmlReporter::WaitForLogWritesForTesting() {
  DecisionLogWriter::Get()->WaitUntilIdle();
}

void DaemonHtmlReporter::FailLogWriterStartForTesting(bool fail) {
  fail_writer_start_for_testing.store(fail, std::memory_order_relaxed);
}

void DaemonHtmlReporter::TakeLogResults() {
  if (log_ == nullptr) return;
  const int64_t dropped = log_->dropped.exchange(0, std::memory_order_relaxed);
  if (dropped > 0) log_dropped_->Add(dropped);
  for (const GoogleString& message : log_->messages.Take()) {
    handler_->Message(kWarning, "%s", message.c_str());
  }
}

void DaemonHtmlReporter::CountUnavailable() { unavailable_->Add(1); }

void DaemonHtmlReporter::Report(const DaemonHtmlCapture& capture,
                                bool aborted) {
  evaluated_->Add(1);

  std::vector<const char*> names;
  std::vector<const char*> values;
  names.reserve(capture.headers_.size());
  values.reserve(capture.headers_.size());
  for (const auto& [name, value] : capture.headers_) {
    names.push_back(name.c_str());
    values.push_back(value.c_str());
  }
  uint32_t request_flags = 0;
  if (capture.has_authorization_) {
    request_flags |= kPsHtmlRequestHasAuthorization;
  }
  if (capture.authenticated_user_) {
    request_flags |= kPsHtmlRequestAuthenticatedUser;
  }
  if (capture.has_cookie_) request_flags |= kPsHtmlRequestHasCookie;
  if (capture.request_no_store_) request_flags |= kPsHtmlRequestNoStore;

  const uint64_t now_ms = static_cast<uint64_t>(timer_->NowMs());
  int reason = 0;
  uint32_t lifetime_s = 0;
  uint32_t flags = 0;
  uint16_t cc_flags = 0;
  int sidecar = 0;
  char detail[kDetailBufferBytes] = {0};
  size_t detail_len = 0;
  const int action = capture.abi_->HtmlStoreClassify(
      kPsHtmlCacheReportOnly, capture.method_.c_str(),
      capture.url_path_.c_str(), request_flags, capture.status_, names.data(),
      values.data(), names.size(), capture.body_bytes_,
      capture.body_ended_ && !aborted ? 1 : 0, capture.body_prefix_.data(),
      capture.body_prefix_.size(),
      static_cast<uint64_t>(std::max<int64_t>(capture.max_body_bytes_, 0)),
      static_cast<uint32_t>(now_ms / 1000), &reason, &lifetime_s, &flags,
      &cc_flags, &sidecar, detail, sizeof(detail), &detail_len);
  if (action < 0) {
    classify_failed_->Add(1);
    return;
  }

  const char* reason_name = capture.abi_->HtmlStoreReasonName(reason);
  if (action == kPsHtmlStoreActionWouldStore ||
      action == kPsHtmlStoreActionWouldStoreNoNotify) {
    would_store_->Add(1);
    would_store_bytes_->Add(static_cast<int64_t>(capture.body_bytes_));
    if (action == kPsHtmlStoreActionWouldStoreNoNotify) {
      would_store_no_notify_->Add(1);
    }
  } else {
    refused_->Add(1);
    const auto found = reason_name == nullptr
                           ? refused_by_reason_.end()
                           : refused_by_reason_.find(reason_name);
    (found == refused_by_reason_.end() ? refused_other_ : found->second)
        ->Add(1);
  }
  if (log_ != nullptr) {
    TakeLogResults();
    Log(capture, action, reason_name, detail, lifetime_s, flags, cc_flags,
        sidecar, now_ms);
  }
}

void DaemonHtmlReporter::Log(const DaemonHtmlCapture& capture, int action,
                             const char* reason_name, const char* detail,
                             uint32_t lifetime_s, uint32_t flags,
                             uint16_t cc_flags, int sidecar, uint64_t now_ms) {
  const bool would_store = action == kPsHtmlStoreActionWouldStore ||
                           action == kPsHtmlStoreActionWouldStoreNoNotify;
  const std::string bare_reason = reason_name == nullptr ? "" : reason_name;

  std::string reason_code;
  if (!would_store) {
    reason_code = bare_reason;
    if ((bare_reason == "headers" || bare_reason == "vary") &&
        detail[0] != '\0') {
      reason_code = StrCat(bare_reason, ":", detail);
    }
  }

  std::string sidecar_code;
  if ((flags & kPsHtmlStoreFlagSidecarEvaluated) != 0) {
    if (sidecar == kSidecarEligible) {
      sidecar_code = "eligible_block";
    } else if (sidecar == kSidecarFallThrough) {
      sidecar_code = bare_reason == "headers" && detail[0] != '\0'
                         ? StrCat("fall_through:", detail)
                         : "fall_through";
    } else if (sidecar == kSidecarNever) {
      if (bare_reason == "nonce_csp") sidecar_code = "never:nonce_csp";
      if (bare_reason == "nonce_link") sidecar_code = "never:nonce_link";
    }
  }

  bool etag = false;
  bool last_modified = false;
  for (const auto& [name, value] : capture.headers_) {
    if (value.empty()) continue;
    if (EqualsIgnoreCase(name, "ETag")) etag = true;
    if (EqualsIgnoreCase(name, "Last-Modified")) last_modified = true;
  }
  const std::string validators = etag && last_modified ? "etag,last_modified"
                                 : etag                ? "etag"
                                 : last_modified       ? "last_modified"
                                                       : "none";

  std::string request;
  if (capture.has_authorization_) request = "authorization";
  if (capture.has_cookie_) {
    if (!request.empty()) request.push_back(',');
    request.append("cookie");
  }
  if (request.empty()) request = "none";

  const std::string ts = Integer64ToString(static_cast<int64_t>(now_ms));
  const std::string status = IntegerToString(capture.status_);
  const std::string size =
      Integer64ToString(static_cast<int64_t>(capture.body_bytes_));
  const std::string lifetime = Integer64ToString(lifetime_s);
  const std::string path = LogPath(capture.url_path_);
  const std::string vary = LogVary(capture.headers_);
  const std::string cc = LogCc(cc_flags, flags);
  const char* action_name =
      action == kPsHtmlStoreActionWouldStore           ? "would_store"
      : action == kPsHtmlStoreActionWouldStoreNoNotify ? "would_store_no_notify"
                                                       : "refuse";

  std::vector<const char*> keys;
  std::vector<const char*> values;
  const auto add = [&keys, &values](const char* key, const std::string& value) {
    if (value.empty()) return;  // an absent fact is an absent key
    keys.push_back(key);
    values.push_back(value.c_str());
  };
  const std::string mode = "report-only";
  const std::string action_string = action_name;
  const std::string personalisation =
      bare_reason == "personalisation_signal" ? std::string(detail) : "";
  add("ts_ms", ts);
  if (IsLogHost(capture.host_)) add("host", capture.host_);
  add("path", path);
  add("mode", mode);
  add("action", action_string);
  add("reason", reason_code);
  add("status", status);
  add("size", size);
  add("lifetime_s", lifetime);
  add("validators", validators);
  add("cc", cc);
  add("vary", vary);
  add("sidecar", sidecar_code);
  add("request", request);
  add("personalisation", personalisation);

  char line[kLineBufferBytes];
  size_t line_len = 0;
  const int rc = capture.abi_->HtmlDecisionFormat(
      keys.data(), values.data(), keys.size(), line, sizeof(line), &line_len);
  if (rc != 0) {
    log_dropped_->Add(1);
    return;
  }
  const std::string dedup_key =
      StrCat(capture.host_, "\x1f", path, "\x1f", action_string, "\x1f",
             reason_code, "\x1f", sidecar_code);
  if (!DecisionLogWriter::Get()->Enqueue(log_, dedup_key,
                                         GoogleString(line, line_len),
                                         static_cast<int64_t>(now_ms))) {
    log_dropped_->Add(1);
  }
}

}  // namespace net_instaweb
