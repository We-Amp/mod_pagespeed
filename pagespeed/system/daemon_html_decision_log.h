// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_DAEMON_HTML_DECISION_LOG_H_
#define PAGESPEED_SYSTEM_DAEMON_HTML_DECISION_LOG_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/rule_pack_decision_log.h"

namespace net_instaweb {

class DaemonAbi;
class DaemonHtmlPendingMessages;

// The decision log of the HTML cache: one line per decision the engine took
// or, in report-only, would have taken, in <LogDir>/daemon-html/.
//
// The engines of a process that share a LogDir share one log: one
// descriptor, one dedup set and one rate limit, whatever the line says (a
// report, a store, a Markdown answer, a purge).  The log is written by a
// thread of its own, so a request never waits on the file system: opening a
// day's file, creating its directory, deleting expired files and every write
// happen there, and a request (an nginx event loop included) only formats a
// line and queues it.  The thread starts with the first line a process logs;
// the parent of a forking server never logs, so each child starts its own.
// Lines still queued when the process exits are lost.  Thread-safe.
class DaemonHtmlDecisionLog
    : public std::enable_shared_from_this<DaemonHtmlDecisionLog> {
 public:
  // At most this many lines per second per process for each LogDir, on top
  // of the per-process dedup and the daily cap of the log itself.
  static constexpr int kMaxLinesPerSecond = 100;

  // One key and value of a line; the library's formatter fixes the order.
  using Fields = std::vector<std::pair<const char*, GoogleString>>;

  // The log of `log_dir` in this process.  `log_dir` must not be empty.
  static std::shared_ptr<DaemonHtmlDecisionLog> ForLogDir(StringPiece log_dir);

  explicit DaemonHtmlDecisionLog(const GoogleString& dir);
  ~DaemonHtmlDecisionLog();

  DaemonHtmlDecisionLog(const DaemonHtmlDecisionLog&) = delete;
  DaemonHtmlDecisionLog& operator=(const DaemonHtmlDecisionLog&) = delete;

  // Formats the fields through the library (fields with an empty value are
  // left out: an absent fact is an absent key) and queues the line under
  // `dedup_key`.  False when the line was dropped: the formatter refused it,
  // the queue was full, or the writer thread could not be started (the next
  // line tries again).
  bool Queue(const DaemonAbi& abi, const Fields& fields,
             const GoogleString& dedup_key, int64 now_ms);

  // The number of lines the writer dropped since the last call.
  int64 TakeDropped();
  // What the writer has to say to a server's message handler, handed on:
  // the thread never calls a handler, whose server may be gone by the time a
  // queued line is written.
  std::vector<GoogleString> TakeMessages();

  // Returns once every line queued so far in this process has been written
  // or dropped.
  static void WaitForWritesForTesting();

  // While set, the writer thread of a process that has not started it yet
  // fails to start, as if the system refused a thread.
  static void FailWriterStartForTesting(bool fail);

 private:
  friend class DaemonHtmlDecisionLogWriter;

  RulePackDecisionLog log_;
  std::unique_ptr<DaemonHtmlPendingMessages> messages_;
  std::atomic<int64> dropped_{0};
};

using DaemonHtmlLogHeaderLines =
    std::vector<std::pair<GoogleString, GoogleString>>;

// The values a decision line may carry, derived from what a response holds.
// Each returns an empty string for a fact the line leaves out.

// The cache-key path as the log may show it: up to a query, a fragment, path
// parameters or a control byte, so a query string is never written anywhere.
// Percent-escapes stay, so /caf%C3%A9.html and /caf%C3%A8.html are two paths.
GoogleString DaemonHtmlLogPath(StringPiece path);

// Whether `host` is made of host name characters only (and so may be logged).
bool DaemonHtmlIsLogHost(StringPiece host);

// The Vary lines as a token list, or empty when a line holds anything else:
// the value is the origin's to choose, so it is logged only when it is
// plainly a list of names.
GoogleString DaemonHtmlLogVary(const DaemonHtmlLogHeaderLines& headers);

// The Cache-Control directives the library found, by name, when it evaluated
// them (`store_flags` carries its cc-evaluated bit).
GoogleString DaemonHtmlLogCc(uint16 cc_flags, uint32 store_flags);

// "etag", "last_modified", both in that order, or "none".
GoogleString DaemonHtmlLogValidators(const DaemonHtmlLogHeaderLines& headers);

// The names of the lines `final_view` carries beyond `store_view` (the
// header set at the store snapshot): what the server's own configuration
// added after it.  Lower-case, each once, in order, comma-separated; names
// only, never a value.  Empty when nothing was added, or when the list would
// not fit the line.
GoogleString DaemonHtmlVhostAdded(const DaemonHtmlLogHeaderLines& store_view,
                                  const DaemonHtmlLogHeaderLines& final_view);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_DECISION_LOG_H_
