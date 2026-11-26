// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// DaemonHtmlCache serve-variants on Apache: what the port gives the HTML
// cache engine (pagespeed/system/daemon_html_engine.h).
//
// STORE.  The HTML output filter records the origin's response at the
// rewriter's input; the store view is noted at AP_FTYPE_CONTENT_SET - 1
// (after mod_expires, before mod_headers) and the final view at
// AP_FTYPE_CONTENT_SET + 1 (mod_instaweb.cc).  The commit runs inline, on
// the request's own thread, after the response's last brigade went down the
// filter chain.
//
// SERVE.  The handler answers a request for the Markdown copy itself, ahead
// of in-place optimization and after httpd's access, authentication and
// authorization hooks have run (InstawebHandler::handle_as_resource).  The
// answer carries exactly the engine's header set:
//   * every request output filter below the protocol filters is removed, so
//     no compressor, body rewriter (SUBSTITUTE, INCLUDES, sed) or header
//     filter (mod_headers, mod_expires) runs on it;
//   * both header tables are cleared before the set is written, and
//     r->content_languages is cleared, since the core header filter writes
//     Content-Language from it;
//   * no-gzip and no-brotli are set in subprocess_env;
//   * a last filter, just ahead of the core header filter, applies the set
//     again, so a filter added after the handler cannot add a line either.
// A 304 is built the same way; httpd's core header filter then writes only
// its fixed list for a 304 (ETag, Cache-Control and Vary among it).
//
// Main requests only: never a subrequest (r->main) and never an internal
// redirect (r->prev; an ErrorDocument or a rewrite's internal redirect
// answers for another URL than the one it is keyed on).  A DirectoryIndex
// fast redirect stays the main request, and the client's conditionals stay
// readable through it (apache_conditional_stash.h).

#ifndef PAGESPEED_APACHE_APACHE_DAEMON_HTML_H_
#define PAGESPEED_APACHE_APACHE_DAEMON_HTML_H_

#include <condition_variable>
#include <mutex>

#include "pagespeed/apache/apache_httpd_includes.h"  // NOLINT
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "util_filter.h"  // NOLINT

namespace net_instaweb {

class ApacheServerContext;
class GoogleUrl;
class RewriteOptions;

// The output filter that applies the Markdown header set a last time.
extern const char kApacheMarkdownHeadersFilterName[];
// Its place: after every content filter, ahead of the core header filter.
constexpr int kApacheMarkdownHeadersFilterType = AP_FTYPE_PROTOCOL - 1;

// Counts the engine's work in flight on this server context, so the context
// can wait for it before the engine goes away.
class ApacheDaemonHtmlInFlight {
 public:
  void Enter();
  void Leave();
  // Returns once nothing is in flight.
  void WaitIdle();

 private:
  std::mutex mutex_;
  std::condition_variable idle_;
  int count_ = 0;
};

// The site id the optimizer's volume keys `server`'s entries under: the
// keyspace of the record (its own cache fragment, or its configuration's).
GoogleString ApacheDaemonHtmlSiteId(const server_rec* server,
                                    ApacheServerContext* context);

// Whether `r` may be stored or served at all: a main request, not an
// internal redirect.
bool ApacheDaemonHtmlMainRequest(const request_rec* r);

// The engine's view of one request.  `context` is the request's server
// context, `url` the module's reading of the request URL (scheme, host and
// port come from it), `options` the request's options (null leaves
// options_enabled_and_allowed false and the option context empty).  The
// path is the request target's path and query as received.
void FillApacheDaemonHtmlRequestFacts(request_rec* r,
                                      ApacheServerContext* context,
                                      const RewriteOptions* options,
                                      const GoogleUrl& url,
                                      DaemonHtmlRequestFacts* facts);

// What the engine asks of Apache for one request.  The store's blocking work
// runs inline (an Apache request thread may block); the Markdown answer is
// written as described above.  Lives as long as the request.
class ApacheDaemonHtmlPort : public DaemonHtmlPort {
 public:
  ApacheDaemonHtmlPort(request_rec* r, DaemonHtmlEngine* engine,
                       ApacheDaemonHtmlInFlight* in_flight);
  ~ApacheDaemonHtmlPort() override;

  bool ClearAndEmitAllowlisted(const DaemonHtmlServeResult& result,
                               bool head) override;
  void MarkNoCompress() override;
  // The post_read_request hook has already done it, before any handler.
  void RemoveClientConditionals() override {}
  void RunBlocking(DaemonHtmlCommit commit) override;

 private:
  void UndoNoCompress();

  request_rec* r_;
  DaemonHtmlEngine* engine_;
  ApacheDaemonHtmlInFlight* in_flight_;
  bool marked_no_compress_ = false;
  // subprocess_env's values before MarkNoCompress (null: unset).
  const char* prior_no_gzip_ = nullptr;
  const char* prior_no_brotli_ = nullptr;
};

// Removes every request output filter below AP_FTYPE_PROTOCOL from `r`'s
// chain except the Markdown header filter: whatever a handler's response
// would have gone through on its way to the protocol filters.
void ApacheRemoveContentFilters(request_rec* r);

// Makes `r`'s header tables carry exactly `lines`: clears every line of
// both tables and writes `lines` to headers_out (Content-Type and
// Content-Length through the request's own fields, as httpd writes them),
// keeping a Content-Encoding or Content-Range line that is there, since the
// body would otherwise not be readable; `*transport_conflict` says whether
// one was.  Also clears r->content_languages and r->content_encoding when
// nothing encoded the body.
void ApacheApplyMarkdownHeaders(request_rec* r,
                                const DaemonHtmlHeaderLines& lines,
                                bool* transport_conflict);

// The Markdown header filter (kApacheMarkdownHeadersFilterName).
apr_status_t ApacheMarkdownHeadersFilter(ap_filter_t* filter,
                                         apr_bucket_brigade* bb);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_DAEMON_HTML_H_
