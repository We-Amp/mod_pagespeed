// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The nginx-free half of the HTML cache adapter (DaemonHtmlCache
// serve-variants on nginx): the pieces of ngx_pagespeed.cc's HTML cache glue
// that need no nginx structure, so they are unit tested standalone
// (//test/pagespeed/nginx:ngx_daemon_html_test).
//
// THREADING.  nginx's event thread may not block, and the engine's store
// path does volume I/O (DaemonHtmlPort::RunBlocking).  Every piece of that
// work -- a store's commit, a refusal's removal, a URL purge's removal, an
// epoch write, a tick -- is posted onto the record arm's one-worker sequence
// (the record-completion pool) through NgxDaemonHtmlRunner, which owns it
// until it has run or the pool cancelled it.  A commit owns the engine's byte
// budget reservation for its body, so a commit the pool drops releases it.
// The pool is shut down, and so joined, before the engines are destroyed and
// before the volume handles close (ps_exit_child_process); the posted and
// finished counters below say at any moment whether work is in flight.

#ifndef PAGESPEED_NGINX_NGX_DAEMON_HTML_H_
#define PAGESPEED_NGINX_NGX_DAEMON_HTML_H_

#include <utility>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/system/daemon_html_engine.h"

namespace net_instaweb {

class Statistics;
class Variable;
struct ClientConditionalStash;

// nginx-side counters of the work NgxDaemonHtmlRunner posts: every item
// posted, every item that left the pool (run or cancelled), and every item
// the queue caps or a missing pool turned away (a store that is simply not
// made; the next request stores again).  posted == finished means nothing is
// in flight.  Registered by ps_daemon_record_completion_init_stats.
extern const char kDaemonHtmlCommitsPosted[];
extern const char kDaemonHtmlCommitsFinished[];
extern const char kDaemonHtmlCommitsDropped[];
void NgxDaemonHtmlInitStats(Statistics* statistics);

// A test seam, read once per process: when the environment names a file in
// PAGESPEED_TEST_HTML_COMMIT_HOLD_FILE, every posted item waits on the pool
// thread while that file exists (up to 60 s) before it runs, so a system test
// can prove that a read made before the work finished sees nothing.  Unset,
// it costs nothing.
extern const char kDaemonHtmlCommitHoldEnv[];

// Runs the engine's blocking work on the record arm's sequence.
class NgxDaemonHtmlRunner : public DaemonHtmlBlockingRunner {
 public:
  // `bounded`: a store's commit or a refusal's removal, which the shared
  // queue caps may turn away (dropped, counted).  Not bounded: the admin
  // path's and the ticks' work, which is never turned away (an admin purge
  // answers only once it ran).  A shutdown cancels either unrun.
  // The variables may be null.
  NgxDaemonHtmlRunner(bool bounded, Variable* posted, Variable* finished,
                      Variable* dropped);

  // The record arm's sequence for this process, or null where there is no
  // pool; set once at worker start, before any request.
  void set_sequence(QueuedWorkerPool::Sequence* sequence) {
    sequence_ = sequence;
  }

  // Never runs `commit` on the calling thread when a sequence is set.
  // Without one, bounded work is dropped and unbounded work runs inline:
  // there is no pool only where no volume handle exists, so the engine
  // posts nothing that blocks.
  void RunBlocking(DaemonHtmlCommit commit) override;

 private:
  const bool bounded_;
  Variable* posted_;
  Variable* finished_;
  Variable* dropped_;
  QueuedWorkerPool::Sequence* sequence_ = nullptr;
};

// One request header line as the port reads it.
using NgxHeaderLine = std::pair<StringPiece, StringPiece>;

// The request-side facts that come from the request's own header lines, in
// order, every line counted: Authorization or Proxy-Authorization, Cookie or
// Cookie2, every Accept line, Cache-Control and Pragma joined with ", ",
// If-Match, If-Unmodified-Since, Range, If-Range and Origin.  If-None-Match
// and If-Modified-Since are never read here: they come from the client
// conditional stash (`stash`, null when the request carried neither), as
// they arrived, before the module's own entity tags were stripped.
void FillDaemonHtmlRequestHeaderFacts(const std::vector<NgxHeaderLine>& lines,
                                      const ClientConditionalStash* stash,
                                      DaemonHtmlRequestFacts* facts);

// The header lines that decide whether a response may be stored and that a
// stored page's header block is made from: refuse it (Set-Cookie,
// Cache-Control, Vary, Pragma).
bool IsDaemonHtmlRefusalClassHeader(StringPiece name);

// The origin's lines at the first header filter.  Without an upstream:
// `out_lines` (the response as the handler set it).  With one, the
// refusal-class lines come from the upstream's own lines as received
// (`upstream_lines`) instead of from `out_lines`, so proxy_hide_header and
// the like cannot hide a Set-Cookie, Cache-Control, Vary or Pragma from the
// store's verdict; every other line comes from `out_lines`.
DaemonHtmlHeaderLines DaemonHtmlOriginLines(
    const DaemonHtmlHeaderLines& out_lines, bool has_upstream,
    const DaemonHtmlHeaderLines& upstream_lines);

// What the Markdown answer's final enforcement does with one header line it
// finds on the response (after every other header stage has run).
enum class NgxMarkdownLine {
  // A line of the answer's own set (Content-Type, Content-Length,
  // Cache-Control, Vary, X-Robots-Tag, Age, ETag): cleared, and set again
  // from the answer exactly once.
  kOwned,
  // The server core's transport lines, kept: Date, Server, Connection,
  // Keep-Alive, Transfer-Encoding.
  kTransport,
  // Content-Encoding or Content-Range: a stage that did not honour the
  // no-compress mark changed the body.  Kept so the body stays parseable,
  // and counted (DaemonHtmlEngine::NoteTransportConflict).
  kConflict,
  // Anything else: removed.
  kRemove,
};
NgxMarkdownLine ClassifyMarkdownLine(StringPiece name);

// The value of the answer's Content-Length line, or -1 when it has none (a
// 304).
int64 DaemonHtmlAnswerContentLength(const DaemonHtmlServeResult& result);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_DAEMON_HTML_H_
