// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// DaemonHtmlCache serve-variants on IIS: the rules of the port that are free
// of the IIS headers, so they are unit-tested on every platform.
//
// THE ONE SPELLING OF A PATH.  IIS reaches one file under many spellings:
// it matches paths without regard to case, decodes percent-escapes
// (an encoded slash included) and merges slashes before it routes, and the
// file system adds its own aliases (8.3 short names, a trailing dot or
// space, "::$DATA" streams).  The engine keys a page on the request target
// as sent, and a URL purge names one spelling, so a page stored or served
// under another one could outlive a purge of the page.  The port therefore
// stores and serves only the one spelling IisHtmlCachePathIsCanonical
// accepts; every other one is url_disagree.  For a site whose URLs carry
// capitals that means no Markdown copy for them on IIS.
//
// FINALISING THE MARKDOWN ANSWER.  IIS writes the site's customHeaders and
// runs dynamic compression after the main registration's send, so the
// Markdown header set can only be enforced from the final-view
// registration, whose send runs after those stages.  The main registration
// therefore never sends the Markdown answer itself: it keeps it in its own
// module context and leaves an empty error response
// (kIisMarkdownPendingStatus) in its place.  The final-view registration
// replaces that response with the answer, and only while it is still that
// untouched error response (PlanIisMarkdownFinal).  When the final-view
// entry does not run, or another module replaced the response in between
// (a custom error page), the client gets an error, never a Markdown answer
// with the site's own headers on it.

#ifndef PAGESPEED_IIS_IIS_DAEMON_HTML_SERVE_H_
#define PAGESPEED_IIS_IIS_DAEMON_HTML_SERVE_H_

#include <cstddef>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_engine.h"

namespace net_instaweb {

// Whether `target` (a request target's path, with its query when there is
// one) is the one spelling of its path: absolute, lowercase, no
// percent-escape but an uppercase-hex escape of a non-ASCII byte, no empty,
// "." or ".." segment, no segment ending in ".", and none of
// \ ; : ~ * " < > | space or control bytes.  The query is not read.
bool IisHtmlCachePathIsCanonical(StringPiece target);

// The Vary value of an HTML response whose page can be answered with its
// Markdown copy: `existing` with the Accept token added, unless it is there
// already (or the value is "*").
GoogleString IisVaryWithAccept(StringPiece existing);

// The response the main registration leaves for a Markdown answer it has
// kept for the final-view registration: an empty 500 with this sub-status,
// which IIS does not use.
inline constexpr int kIisMarkdownPendingStatus = 500;
inline constexpr int kIisMarkdownPendingSubStatus = 61;

// The Markdown answer, as the engine composed it (DaemonHtmlServeResult).
struct IisMarkdownAnswer {
  int status = 0;  // 200 or 304
  DaemonHtmlHeaderLines headers;
  GoogleString body;  // Empty for a HEAD and a 304.
};

struct IisMarkdownFinalPlan {
  // The response is still the pending one: replace it with the answer.
  bool finalize = false;
  // The names to delete before the answer's own lines are set, each once.
  std::vector<GoogleString> remove;
  // A Content-Encoding or Content-Range line is present and kept, so the
  // body stays readable: counted as a transport conflict.
  bool transport_conflict = false;
};

// What the final-view registration does with the response it holds when a
// Markdown answer is pending: `status`, `substatus` and `entity_chunks` are
// the response's, `current` every header line it carries.
IisMarkdownFinalPlan PlanIisMarkdownFinal(int status, int substatus,
                                          size_t entity_chunks,
                                          const DaemonHtmlHeaderLines& current,
                                          const IisMarkdownAnswer& answer);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DAEMON_HTML_SERVE_H_
