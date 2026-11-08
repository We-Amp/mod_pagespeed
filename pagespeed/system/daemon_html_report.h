// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_DAEMON_HTML_REPORT_H_
#define PAGESPEED_SYSTEM_DAEMON_HTML_REPORT_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class DaemonAbi;
class DaemonHtmlDecisionLog;
class DaemonHtmlReporter;
class MessageHandler;
class Statistics;
class Timer;
class Variable;

// DaemonHtmlCache report-only: whether the optimizer library's HTML cache
// policy would store a response, counted and logged, with no effect on the
// response.
//
// A server adapter creates one DaemonHtmlCapture for a response, only when the
// mode is report-only and the library publishes the policy, fills it from
// READ-ONLY views of what the response already carries, and calls Finish()
// once the response has ended. It never adds, removes or reorders a byte or a
// header of the response. Everything the capture holds is a copy, so it stays
// valid after the response has moved on, and its memory is the header set plus
// at most kBodyWindowBytes of the body.
class DaemonHtmlCapture {
 public:
  // What the policy reads of a body: it scans at most this prefix.
  static constexpr size_t kBodyWindowBytes = 64 * 1024;

  DaemonHtmlCapture(DaemonHtmlReporter* reporter, const DaemonAbi* abi,
                    int64_t max_body_bytes);
  ~DaemonHtmlCapture();

  DaemonHtmlCapture(const DaemonHtmlCapture&) = delete;
  DaemonHtmlCapture& operator=(const DaemonHtmlCapture&) = delete;

  void SetRequest(StringPiece method, StringPiece url_path, StringPiece host);
  // Which request headers were present, and the Cache-Control value (one call
  // per line). The values of the others are never read.
  void set_has_authorization() { has_authorization_ = true; }
  void set_authenticated_user() { authenticated_user_ = true; }
  void set_has_cookie() { has_cookie_ = true; }
  void AddRequestCacheControl(StringPiece value);

  using HeaderLines = std::vector<std::pair<std::string, std::string>>;

  void set_status(int status) { status_ = status; }
  void AddHeader(StringPiece name, StringPiece value);

  // Bounded: counts every byte, keeps the first kBodyWindowBytes.
  void AddBody(const char* data, size_t size);
  void set_body_ended() { body_ended_ = true; }
  bool body_ended() const { return body_ended_; }
  size_t body_prefix_size() const { return body_prefix_.size(); }

  // Classifies, counts and logs, once. `aborted` is the server's word that the
  // client went away; a body that never ended counts as incomplete either way.
  void Finish(bool aborted);
  bool finished() const { return finished_; }

  // For an adapter whose server changes headers after the capture: the lines
  // as they stand once the module has made its own changes (a later
  // SetBaselineHeaders, before ApplyLateHeaders, replaces them), and later the
  // lines as they stand when the response leaves the server's header stages.
  // ApplyLateHeaders adds to the capture the lines that appeared in between
  // and removes those that went away (a changed value is both), except for
  // the names in `ignored` (case-insensitive), whose changes the capture does
  // not take. Vary is compared as a set of tokens and an ETag without a -gzip
  // suffix, so what compression adds is not a change; a changed Vary leaves
  // one line in the capture. Only the first ApplyLateHeaders after
  // SetBaselineHeaders counts.
  void SetBaselineHeaders(HeaderLines lines);
  void ApplyLateHeaders(const HeaderLines& lines,
                        const std::vector<StringPiece>& ignored);
  bool needs_late_headers() const { return has_baseline_; }
  const HeaderLines& headers() const { return headers_; }

 private:
  friend class DaemonHtmlReporter;

  DaemonHtmlReporter* reporter_;
  const DaemonAbi* abi_;
  int64_t max_body_bytes_;
  std::string method_;
  std::string url_path_;
  std::string host_;
  bool has_authorization_ = false;
  bool authenticated_user_ = false;
  bool has_cookie_ = false;
  bool request_no_store_ = false;
  int status_ = 0;
  HeaderLines headers_;
  HeaderLines baseline_;
  bool has_baseline_ = false;
  std::string body_prefix_;
  uint64_t body_bytes_ = 0;
  bool body_ended_ = false;
  bool finished_ = false;
};

// The counters and the decision log of one server configuration. Thread-safe.
//
// The reporters of a process that share a LogDir share one decision log: one
// descriptor, one dedup set and one rate limit. The log is written by a thread
// of its own, so Report never waits on the file system; its file is opened,
// its directory created and its expired files deleted on that thread.
class DaemonHtmlReporter {
 public:
  // At most this many decision lines per second per process for each LogDir,
  // on top of the per-process dedup and the daily cap of the log itself.
  static constexpr int kMaxLinesPerSecond = 100;

  // The log goes to <log_dir>/daemon-html/; an empty log_dir leaves the
  // counters and logs one warning. All pointers must outlive this object.
  DaemonHtmlReporter(Statistics* statistics, Timer* timer, StringPiece log_dir,
                     MessageHandler* handler);
  ~DaemonHtmlReporter();

  DaemonHtmlReporter(const DaemonHtmlReporter&) = delete;
  DaemonHtmlReporter& operator=(const DaemonHtmlReporter&) = delete;

  // An HTML response under report-only that no installed library can classify:
  // no daemon configured, or a library older than the policy.
  void CountUnavailable();

  void Report(const DaemonHtmlCapture& capture, bool aborted);

  // Returns once every line queued so far in this process has been written
  // or dropped.
  static void WaitForLogWritesForTesting();

  // While set, the writer thread of a process that has not started it yet
  // fails to start, as if the system refused a thread.
  static void FailLogWriterStartForTesting(bool fail);

 private:
  // Counts the lines the writer dropped and passes on its messages.
  void TakeLogResults();
  void Log(const DaemonHtmlCapture& capture, int action,
           const char* reason_name, const char* detail, uint32_t lifetime_s,
           uint32_t flags, uint16_t cc_flags, int sidecar, uint64_t now_ms);

  Timer* timer_;
  MessageHandler* handler_;
  Variable* evaluated_;
  Variable* would_store_;
  Variable* would_store_no_notify_;
  Variable* would_store_bytes_;
  Variable* refused_;
  Variable* refused_other_;
  Variable* unavailable_;
  Variable* classify_failed_;
  Variable* log_dropped_;
  std::map<std::string, Variable*> refused_by_reason_;
  std::shared_ptr<DaemonHtmlDecisionLog> log_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_REPORT_H_
