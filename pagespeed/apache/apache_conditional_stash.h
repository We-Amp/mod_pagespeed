// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The module entity tag rule on Apache (see
// pagespeed/system/module_entity_tags.h): a post_read_request hook keeps the
// client's conditionals in per-request state, then removes If-None-Match and
// If-Modified-Since from headers_in when If-None-Match carries a tag this
// module mints.  post_read_request runs before mod_proxy, mod_cache's quick
// handler and the default handler read headers_in, and again on every
// internal redirect, whose headers_in is the original request's table.

#ifndef PAGESPEED_APACHE_APACHE_CONDITIONAL_STASH_H_
#define PAGESPEED_APACHE_APACHE_CONDITIONAL_STASH_H_

#include "pagespeed/apache/apache_httpd_includes.h"  // NOLINT
#include "pagespeed/system/module_entity_tags.h"

namespace net_instaweb {

// Applies the rule to `r`.  The stash is kept once, for the request as the
// client sent it: on an internal redirect (which shares the request pool)
// the first stash stays, so it still holds the fields as they arrived.
void ApacheStashAndStripClientConditionals(request_rec* r);

// The stash of the request `r` belongs to, found through internal redirects
// (r->prev) and subrequests (r->main), so a DirectoryIndex or ErrorDocument
// request reads the client's original fields.  Null when the hook did not
// run for it.
const ClientConditionalStash* ApacheClientConditionalStash(
    const request_rec* r);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_CONDITIONAL_STASH_H_
