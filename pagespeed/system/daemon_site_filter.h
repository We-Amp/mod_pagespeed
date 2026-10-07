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

// What a per-host admin console may see of the optimizer's answers.  The
// module decides, because it knows the console's scope; the optimizer's
// answers are untrusted.  Every function reads an answer of at most the
// proxy's JSON cap strictly -- no comments, no trailing data, no repeated
// key, bounded nesting -- and fails closed.  `own_host` is the host the
// console's own site records its serves under (VouchedServeHost); "" means
// the site has none, and then nothing matches it.

#ifndef PAGESPEED_SYSTEM_DAEMON_SITE_FILTER_H_
#define PAGESPEED_SYSTEM_DAEMON_SITE_FILTER_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// GET /v1/stats: keeps at most ONE row of serve_savings_by_host -- the row
// named exactly `own_host` (repeated rows of it summed into one) -- and
// adds every other row's hits and bytes to "other", so the rows and "other"
// still add up to the totals; "limit" is kept; and marks the block with
// "site": `own_host`, "" when it is empty -- the console's sign that the
// block was narrowed for it (an upstream "site" is never kept).  A block
// that is malformed in any way -- a wrong type, a counter that is negative,
// fractional, written as text or outside 64 bits, a sum that would pass
// 2^64-1 -- is removed whole (and so carries no marker), and keys inside
// the block that are not part of it are dropped.  Every other top-level key
// is kept with its value.  The document is written compactly with sorted
// keys, as the optimizer writes it, and in ASCII only (non-ASCII text as
// \uXXXX escapes), so only a fractional number's or a non-ASCII string's
// spelling can differ (never its value).
// Returns false, with `out` empty, when the answer is not exactly one JSON
// object, or anything fails while it is rebuilt: the caller must then send
// none of it.
bool FilterStatsForSite(StringPiece upstream_body, StringPiece own_host,
                        GoogleString* out);

// GET /v1/cache/cooldowns: keeps only the entries whose "hostname" is
// `own_host` under the optimizer's host-name rule (NormalizeServeHostName)
// -- each kept entry with only its known fields (url, hostname, scheme and
// reason as text; remaining_seconds and duration_seconds as whole seconds)
// -- drops every other entry, malformed ones, ones without a url and ones
// with a known field of another type included, and sets "count" to the number kept.  A
// "cooldowns" value that is missing or not a list becomes an empty list.
// Of the other top-level keys only "enabled" is kept, and only as a
// boolean.  Returns false, with `out` empty, when the answer is not exactly
// one JSON object or anything fails while it is rebuilt.
bool FilterCooldownsForSite(StringPiece upstream_body, StringPiece own_host,
                            GoogleString* out);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_SITE_FILTER_H_
