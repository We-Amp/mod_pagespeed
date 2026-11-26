// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// DaemonHtmlCache on IIS: what the port records of an HTML response for the
// HTML cache engine (pagespeed/system/daemon_html_engine.h), and when.
//
// THE THREE HEADER VIEWS.  The engine classifies a response on two views,
// with the module's own lines taken out of both.  On IIS they come from
// three points of one response:
//
//   * the origin: the headers at entry to the main registration's first
//     send notification, before the module changes anything (what the
//     handler, StaticFileModule's clientCache included, produced);
//   * the module view: the headers right after the module set its own
//     (IisModuleBaseFetch::CollectHeaders), the baseline the engine
//     subtracts;
//   * the final view: the headers the final-view registration
//     (iis_registration.h) sees at its send, after ProtocolSupportModule has
//     written customHeaders and dynamic compression has run.  Neither is
//     visible from the main registration's own send.
//
// The store view is the origin.  The final view is the origin plus what
// changed from the module view to the final view; the engine ignores what
// compression changes.  Without a final view (the final-view entry is not in
// the site's module list), the report classifies on the store view alone,
// and Finish says so for the caller to log.
//
// THE BODY is the response as the handler produced it, captured in the main
// registration's send before it clears the response for the rewriter: a
// statically compressed page is recorded after the HTML path's own inflater,
// so it is evaluated on the bytes it decodes to.  The module's rewritten
// output is written after that send and never passes through it.
//
// Free of the IIS headers; one request, not thread-safe (IIS hands the
// response from one thread to the next, and each call here is made by the
// thread that holds it).

#ifndef PAGESPEED_IIS_IIS_DAEMON_HTML_CAPTURE_H_
#define PAGESPEED_IIS_IIS_DAEMON_HTML_CAPTURE_H_

#include <cstddef>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_engine.h"

namespace net_instaweb {

// The server variable the global GL_PRE_BEGIN_REQUEST notification sets to
// the moment the request arrived (Unix milliseconds, decimal), before any
// other module runs: the capture's start, so an entry's insert time is never
// later than the moment its origin was asked.  Not a name a request header
// maps to (those are HTTP_*), and written over whatever is there.
inline constexpr char kIisRequestStartVariable[] = "X-PAGESPEED-REQUEST-START";

// Reads that variable's value.  False for anything but a plain positive
// decimal of at most 18 digits.
bool IisParseRequestStartMs(const wchar_t* value, size_t length, int64* out_ms);

// The lines of a header table the views are compared on: every line but
// the ones this module owns (its signature, its local-only status header,
// the option headers an origin sends to it and it removes, and the digest
// a rewrite invalidates).
DaemonHtmlHeaderLines IisDaemonHtmlViewLines(const DaemonHtmlHeaderLines& raw);

class IisDaemonHtmlCapture {
 public:
  explicit IisDaemonHtmlCapture(std::unique_ptr<DaemonHtmlRecording> recording);
  ~IisDaemonHtmlCapture();
  IisDaemonHtmlCapture(const IisDaemonHtmlCapture&) = delete;
  IisDaemonHtmlCapture& operator=(const IisDaemonHtmlCapture&) = delete;

  // The origin's status and lines.  `body_inflated`: the module decodes the
  // body it records, so the origin's Content-Encoding and Content-Length do
  // not describe it and are left out.  Transfer-Encoding never is part of a
  // view (the module removes the chunking it records through).
  void SetOrigin(int status, const DaemonHtmlHeaderLines& lines,
                 bool body_inflated);
  // The body, in order, as the rewriter is given it.  Nothing is taken
  // after NoteBodyEnd.
  void AddBody(const char* data, size_t size);
  // The handler's last send: the body is whole.
  void NoteBodyEnd();
  // Part of the body could not be read or decoded: it is never whole.
  void NoteBodyBroken();
  // The response did not reach the client (a reset or a disconnect).
  void NoteAborted();

  // The module's own header edits are done.  Only the first call counts.
  void NoteModuleView(const DaemonHtmlHeaderLines& lines);
  // The final view, at the final-view registration's send.  Counts only
  // after the module view, and only the first time.  A status other than
  // the origin's means another module replaced the response (a custom
  // error page): what goes out is not what was recorded.
  void NoteFinalView(int status, const DaemonHtmlHeaderLines& lines);
  bool needs_final_view() const;

  // Hands the recording to the engine, once.  Returns true when the module
  // view was taken but no final view came: the report then read the store
  // view alone.
  bool Finish();
  bool finished() const { return finished_; }

 private:
  std::unique_ptr<DaemonHtmlRecording> recording_;
  int status_ = 0;
  bool module_view_taken_ = false;
  bool body_broken_ = false;
  bool body_ended_ = false;
  bool aborted_ = false;
  bool finished_ = false;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DAEMON_HTML_CAPTURE_H_
