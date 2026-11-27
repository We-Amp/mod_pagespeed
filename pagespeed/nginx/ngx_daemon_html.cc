// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_daemon_html.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/webbotauth/classifier.h"
#include "pagespeed/nginx/ngx_daemon_record_completion.h"
#include "pagespeed/system/module_entity_tags.h"

namespace net_instaweb {

const char kDaemonHtmlCommitsPosted[] = "daemon_html_commits_posted";
const char kDaemonHtmlCommitsFinished[] = "daemon_html_commits_finished";
const char kDaemonHtmlCommitsDropped[] = "daemon_html_commits_dropped";
const char kDaemonHtmlCommitHoldEnv[] = "PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE";

void NgxDaemonHtmlInitStats(Statistics* statistics) {
  if (statistics == nullptr) return;
  statistics->AddVariable(kDaemonHtmlCommitsPosted);
  statistics->AddVariable(kDaemonHtmlCommitsFinished);
  statistics->AddVariable(kDaemonHtmlCommitsDropped);
}

const GoogleString& DaemonHtmlCommitHoldFile() {
  static const GoogleString* file = [] {
    const char* value = getenv(kDaemonHtmlCommitHoldEnv);
    return new GoogleString(value == nullptr ? "" : value);
  }();
  return *file;
}

namespace {

void WaitWhileHeld() {
  const GoogleString& file = DaemonHtmlCommitHoldFile();
  if (file.empty()) return;
  constexpr int kStepMs = 20;
  constexpr int kMaxMs = 60 * 1000;
  struct stat info;
  for (int waited = 0; waited < kMaxMs && stat(file.c_str(), &info) == 0;
       waited += kStepMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
  }
}

void Add(Variable* v) {
  if (v != nullptr) v->Add(1);
}

// One posted item.  Owns the engine's work (and with it whatever the work
// owns, the byte budget reservation of a store included) until it runs or
// the pool cancels it; the queue reservation of bounded work is released by
// the destructor either way.
class DaemonHtmlTask : public Function {
 public:
  DaemonHtmlTask(DaemonHtmlCommit commit, bool bounded, Variable* finished)
      : commit_(std::move(commit)), bounded_(bounded), finished_(finished) {}
  ~DaemonHtmlTask() override {
    // Released before the count, so a caller that sees the counts agree
    // also sees the budget back.
    DaemonHtmlCommit drop = std::move(commit_);
    (void)drop;
    if (bounded_) DaemonCompletionExited(0);
    Add(finished_);
  }

  void Run() override {
    WaitWhileHeld();
    commit_.Run();
  }
  // A shutdown: the work is dropped unrun, whatever it is.  The worker is
  // exiting after its event loop stopped, so an admin purge cancelled here
  // has no client left to answer, and running it would complete a fetch the
  // port has already torn down.
  void Cancel() override {}

 private:
  DaemonHtmlCommit commit_;
  const bool bounded_;
  Variable* finished_;
};

}  // namespace

NgxDaemonHtmlRunner::NgxDaemonHtmlRunner(bool bounded, Variable* posted,
                                         Variable* finished, Variable* dropped)
    : bounded_(bounded),
      posted_(posted),
      finished_(finished),
      dropped_(dropped) {}

void NgxDaemonHtmlRunner::RunBlocking(DaemonHtmlCommit commit) {
  if (sequence_ == nullptr) {
    if (bounded_) {
      Add(dropped_);
      return;  // `commit` and what it owns go here.
    }
    commit.Run();
    return;
  }
  if (bounded_ && !TryQueueDaemonCompletion(0, dropped_)) {
    return;
  }
  Add(posted_);
  sequence_->Add(new DaemonHtmlTask(std::move(commit), bounded_, finished_));
}

DaemonHtmlAgentSignature NgxDaemonHtmlAgentSignature(
    bool enabled, const StringPiece* verdict, StringPiece verified_bot_suffix) {
  if (!enabled) return DaemonHtmlAgentSignature::kNone;
  if (verdict == nullptr) return DaemonHtmlAgentSignature::kNotReady;
  using webbotauth::Verdict;
  using webbotauth::VerdictToken;
  if (*verdict == VerdictToken(Verdict::kSignedAgent) ||
      (!verified_bot_suffix.empty() &&
       verdict->ends_with(verified_bot_suffix))) {
    return DaemonHtmlAgentSignature::kVerified;
  }
  if (*verdict == VerdictToken(Verdict::kUnknown)) {
    return DaemonHtmlAgentSignature::kFailed;
  }
  return DaemonHtmlAgentSignature::kNone;
}

void FillDaemonHtmlRequestHeaderFacts(const std::vector<NgxHeaderLine>& lines,
                                      const ClientConditionalStash* stash,
                                      DaemonHtmlRequestFacts* facts) {
  for (const auto& [name, value] : lines) {
    if (facts->ajax_marker.empty()) {
      const char* marker = DaemonHtmlAjaxMarker(name, value);
      if (marker != nullptr) facts->ajax_marker = marker;
    }
    if (StringCaseEqual(name, "Accept")) {
      facts->accept_lines.push_back(value.as_string());
    } else if (StringCaseEqual(name, "Cache-Control")) {
      if (facts->has_cache_control) facts->cache_control.append(", ");
      facts->has_cache_control = true;
      StrAppend(&facts->cache_control, value);
    } else if (StringCaseEqual(name, "Pragma")) {
      if (facts->has_pragma) facts->pragma.append(", ");
      facts->has_pragma = true;
      StrAppend(&facts->pragma, value);
    } else if (StringCaseEqual(name, "Authorization") ||
               StringCaseEqual(name, "Proxy-Authorization")) {
      facts->has_authorization = true;
    } else if (StringCaseEqual(name, "Cookie") ||
               StringCaseEqual(name, "Cookie2")) {
      facts->has_cookie = true;
    } else if (StringCaseEqual(name, "If-Match")) {
      facts->has_if_match = true;
    } else if (StringCaseEqual(name, "If-Unmodified-Since")) {
      facts->has_if_unmodified_since = true;
    } else if (StringCaseEqual(name, "Range")) {
      facts->has_range = true;
    } else if (StringCaseEqual(name, "If-Range")) {
      facts->has_if_range = true;
    } else if (StringCaseEqual(name, "Origin")) {
      facts->has_origin = true;
    }
  }
  if (stash != nullptr) CopyClientConditionals(*stash, facts);
}

bool IsDaemonHtmlRefusalClassHeader(StringPiece name) {
  return StringCaseEqual(name, "Set-Cookie") ||
         StringCaseEqual(name, "Cache-Control") ||
         StringCaseEqual(name, "Vary") || StringCaseEqual(name, "Pragma");
}

DaemonHtmlHeaderLines DaemonHtmlOriginLines(
    const DaemonHtmlHeaderLines& out_lines, bool has_upstream,
    const DaemonHtmlHeaderLines& upstream_lines) {
  if (!has_upstream) return out_lines;
  DaemonHtmlHeaderLines origin;
  for (const auto& line : out_lines) {
    if (!IsDaemonHtmlRefusalClassHeader(line.first)) origin.push_back(line);
  }
  for (const auto& line : upstream_lines) {
    if (IsDaemonHtmlRefusalClassHeader(line.first)) origin.push_back(line);
  }
  return origin;
}

NgxMarkdownLine ClassifyMarkdownLine(StringPiece name) {
  static const char* const kOwned[] = {"Content-Type",  "Content-Length",
                                       "Cache-Control", "Vary",
                                       "X-Robots-Tag",  "Age",
                                       "ETag"};
  static const char* const kTransport[] = {"Date", "Server", "Connection",
                                           "Keep-Alive", "Transfer-Encoding"};
  for (const char* n : kOwned) {
    if (StringCaseEqual(name, n)) return NgxMarkdownLine::kOwned;
  }
  for (const char* n : kTransport) {
    if (StringCaseEqual(name, n)) return NgxMarkdownLine::kTransport;
  }
  if (StringCaseEqual(name, "Content-Encoding") ||
      StringCaseEqual(name, "Content-Range")) {
    return NgxMarkdownLine::kConflict;
  }
  return NgxMarkdownLine::kRemove;
}

bool DaemonHtmlRequestHost(StringPiece server, const StringPiece* host_header,
                           GoogleString* host) {
  server.CopyToString(host);
  if (host_header == nullptr) return true;
  StringPiece name = *host_header;
  // The host part: up to the port's colon, after an IPv6 literal's bracket.
  const size_t start = name.empty() || name[0] != '[' ? 0 : name.find(']');
  const size_t colon =
      start == StringPiece::npos ? StringPiece::npos : name.find(':', start);
  if (colon != StringPiece::npos) name = name.substr(0, colon);
  // nginx drops one trailing dot of the name it selects by.
  if (!name.empty() && name[name.size() - 1] == '.') {
    name.remove_suffix(1);
  }
  if (!StringCaseEqual(name, server)) return false;
  host_header->CopyToString(host);
  return true;
}

int64 DaemonHtmlAnswerContentLength(const DaemonHtmlServeResult& result) {
  for (const auto& [name, value] : result.headers) {
    if (!StringCaseEqual(name, "Content-Length")) continue;
    int64 length = 0;
    if (value.empty() ||
        value.find_first_not_of("0123456789") != GoogleString::npos ||
        !StringToInt64(value, &length)) {
      return -1;
    }
    return length;
  }
  return -1;
}

}  // namespace net_instaweb
