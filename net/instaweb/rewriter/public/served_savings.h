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
    // An optimized response whose cache entry has no sizes with it: the
    // unmeasured-responses variable was incremented instead.
    kUnmeasured,
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

    // The rest describes a response answered from a cache entry that has no
    // record, and is not read otherwise.
    //
    // The filter id in the URL that was asked for; empty when the response is
    // not such an answer.
    StringPiece url_filter_id;
    // The entry's X-Original-Content-Length, or -1 when it has none.
    int64 header_original_size = -1;
    // The length of the entry's body when it is stored uncompressed, else -1.
    int64 uncompressed_body_size = -1;
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
  // bytes: image recompression, CSS rewriting, JavaScript minification, and
  // the CSS and JavaScript combiners when the sizes differ (a combination of
  // inputs that were minified first; a combination that changed no size
  // saves requests, not bytes, and is not counted).  Other filters are not
  // counted.  The record's sizes are added as they are, so a response that
  // is larger than what it was made from lowers the saving.
  //
  // Without a record, which is how entries stored before the record existed
  // read: an image is counted from the entry's X-Original-Content-Length and
  // the length of its body, which is approximate where the origin compresses
  // images; a stylesheet or script, or an image without that header, is an
  // unmeasured response; a combination or anything else is not counted.
  static Outcome Record(const Response& response, RewriteStats* stats);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_SERVED_SAVINGS_H_
