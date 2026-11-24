// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_SERVED_SAVINGS_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_SERVED_SAVINGS_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

class RequestHeaders;
class RewriteStats;

// Counts the sizes of optimized responses as they are sent: what each was
// made from, and what went out.  Every rule about what counts lives in
// Record(), so the places that send a response only describe it.
//
// Sizes are of uncompressed bodies on both sides.  The statistics measure
// what this server sent, each time it sent it; a browser or proxy cache in
// front of it saves more than they show.
class ServedSavings {
 public:
  enum Outcome {
    // Nothing was added to any variable.
    kNotCounted,
    // The response's sizes were added to its type's pair of variables.
    kCounted,
  };

  // One response with a body, as the place that sends it sees it.  Everything
  // is borrowed for the duration of the call.
  struct Response {
    // The driver serving it is a nested one: the fetch is the module's own,
    // for an input of another rewrite.
    bool nested_driver = false;
    // The request being answered.
    const RequestHeaders* request_headers = nullptr;
    // The status of the response.
    int status_code = 0;
    // The size record of what is served, or null when it has none.
    const ResponseHeaders::SizeRecord* size_record = nullptr;
  };

  // Decides whether 'response' counts and adds to 'stats' accordingly.
  //
  // Not counted: a nested driver's response; a request that is not a GET; a
  // response that is not a 200; a request with a Range header (the whole
  // body is written and cut down afterwards); a request that carries the
  // module's own marker in its User-Agent (the module fetching from its own
  // server).
  //
  // With a size record, the type is that of the filter that wrote the served
  // bytes: image recompression, CSS rewriting and JavaScript minification,
  // whose sizes are added as they are, so a response that is larger than
  // what it was made from lowers the saving.  The CSS and JavaScript
  // combiners count only when the combination is smaller than what it was
  // made from, a saving carried in by inputs that were optimized first;
  // combining itself saves requests, not bytes, and a plain combination,
  // which is the same size or slightly larger, is not counted.  Other
  // filters are not counted.
  //
  // Without a record nothing is counted.  The cache generation was raised
  // when the record was introduced, so no entry from before it is read; an
  // output without one was written without knowing the size of what it was
  // made from (it has no inputs, say).
  static Outcome Record(const Response& response, RewriteStats* stats);

  // What kind of resource a response from the optimizer's cache is.
  enum CachedType {
    kCachedOther,
    kCachedImage,
    kCachedCss,
    kCachedJavascript,
  };

  // One optimized response answered from the optimizer's cache, as the place
  // that sends it sees it.  That path has no driver and no parsed request,
  // so the request is described by plain values; nothing here is built for
  // the call.
  struct OptimizerCacheResponse {
    // The request is a GET.
    bool get_request = false;
    // The whole body went out with a 200: false for a 304 and for any answer
    // without a body.
    bool body_sent = false;
    // The request has a Range header.
    bool range_request = false;
    // The request's User-Agent; empty when it has none.
    StringPiece user_agent;
    CachedType type = kCachedOther;
    // What the body was made from, and the body sent, measured the same way
    // (both uncompressed, or both in the one compression the body went out
    // in).  The caller decides that; see RecordDaemonServeCounters.
    int64 original_size = 0;
    int64 optimized_size = 0;
  };

  // The same decision as Record() for a response answered from the
  // optimizer's cache, added to that path's own variables
  // (ipro_daemon_served_{image,css,js}_{original,optimized}_bytes).
  //
  // Not counted: a request that is not a GET; an answer whose whole body did
  // not go out with a 200; a request with a Range header; a request that
  // carries the module's own marker in its User-Agent; a type other than
  // image, stylesheet or script.  A response larger than what it was made
  // from is counted as it is and lowers the saving.
  static Outcome RecordOptimizerCacheServe(
      const OptimizerCacheResponse& response, RewriteStats* stats);

  // True for the id of a filter whose output is an optimized image,
  // stylesheet or script by itself: image recompression, CSS rewriting,
  // JavaScript minification.  Every output of these that has inputs is
  // stored with a size record.
  static bool IsOptimizingFilter(StringPiece filter_id);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_SERVED_SAVINGS_H_
