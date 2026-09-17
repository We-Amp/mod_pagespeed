/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef PAGESPEED_APACHE_IN_PLACE_NOT_MODIFIED_VARY_FETCH_H_
#define PAGESPEED_APACHE_IN_PLACE_NOT_MODIFIED_VARY_FETCH_H_

#include "net/instaweb/http/public/async_fetch.h"

namespace net_instaweb {

// Sits between RewriteDriver's in-place fetch and the unbuffered ApacheFetch
// on the classic (non-daemon) in-place serve path, and composes the `Vary`
// field the 304 leg of a response has to state.
//
// WHY A 304 NEEDS COMPOSING AT ALL.  This path composes no encoding axis
// itself: the 200's `Vary: Accept-Encoding` token is stamped by httpd's
// compressor, whose by-type filter sits in the chain the module's own bytes
// are written through.  The same compressor states nothing on a 304 -- a 304
// has no body, and the compressor's small-response shortcut (a whole
// response of at most 68 bytes is passed through before the line that
// states the axis) fires on zero length every time.  The 304 therefore went
// out stating a NARROWER `Vary` than the 200 it revalidates, and a shared
// cache that updates its stored header fields from the 304 (RFC 9111 4.3.4)
// was told the response varies on less than the set it had keyed on -- the
// direction that can serve one client's stored representation to a request
// with different encoding capabilities.  The daemon substrate arm closed
// the same defect class for its own serves by composing the field above the
// 200/304 split; this is the classic path's counterpart, and its
// composition lives at the Apache seam because the 200's emitter here is
// httpd's own filter rather than this module.
//
// THE TOKEN IS COMPOSED ONLY ON THE 304 LEG, against the URL-derived media
// type (mod_mime's answer for the URL) -- the one type this leg has in
// hand, the 304 branch having cleared the response headers before it runs.
// The daemon arm's counterpart prefers the type of the bytes it serves and
// falls back to the URL's; here the fallback is all there is.  On the 200
// leg the compressor is the emitter, and a second copy here would put two
// emitters on one field.
//
// WHAT THIS COMPOSITION CANNOT SEE OF THE COMPRESSOR'S DECISION, stated
// rather than left to be discovered.  The two legs are dispatched on
// DIFFERENT facts: the 200's compressor is selected against the SERVED
// media type, matched when the filter chain first runs against the type
// the handler set, while this composition reads the URL-derived type.
// Where the two straddle, the 304 can still state less than its 200 -- the
// defect this class closes, surviving in a corner:
//   * an extensionless URL whose origin serves a compressible type (a
//     clean-URL stylesheet or script): the 200 is stamped, and the 304
//     reads no type at all and states nothing;
//   * a media type the server's filter wiring names but this module's
//     compressor predicate does not -- a stock by-type list names
//     application/rss+xml, and the predicate does not.
// For the SUPPRESSOR paths -- a response whose compressor was selected and
// then declined it -- the divergence is the other way, and the safe one:
// the 304 states the encoding axis for a 200 that did not, which WIDENS a
// cache's key and costs a split, and cannot serve one client's
// representation to another:
//   * the length floor.  The compressor passes a whole response of at most
//     68 bytes through unstated; a 304 states nothing about length because
//     the leg this wraps reads no body -- answering the conditional
//     without the entry is the point of the branch it serves.  An optimized
//     output that small is the only case affected.
//   * a server that wires no compressor for this media type, a second
//     compressor, one disabled per request (`no-gzip`), an existing
//     content encoding, a `Content-Range`, or a subrequest.
// The client's own `Accept-Encoding` is deliberately NOT consulted: the
// compressor states the axis before it scans that header, so a 200 for a
// compressible type carries the token whatever the client offered.
class InPlaceNotModifiedVaryFetch : public SharedAsyncFetch {
 public:
  // `request_content_type` is the request's own media type and stays owned
  // by the caller; `request_is_head` is whether the request wants headers
  // only.  Both are read at construction and never re-read -- the Apache
  // record's fields change under a response being written, and the facts
  // this composes from belong to the request, not to whichever response is
  // in flight.
  InPlaceNotModifiedVaryFetch(const char* request_content_type,
                              bool request_is_head, AsyncFetch* base_fetch);

 protected:
  void HandleHeadersComplete() override;

 private:
  const char* request_content_type_;
  bool request_is_head_;

  InPlaceNotModifiedVaryFetch(const InPlaceNotModifiedVaryFetch&) = delete;
  InPlaceNotModifiedVaryFetch& operator=(const InPlaceNotModifiedVaryFetch&) =
      delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_IN_PLACE_NOT_MODIFIED_VARY_FETCH_H_
