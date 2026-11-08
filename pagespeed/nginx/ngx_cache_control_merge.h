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

//
// The Cache-Control the nginx port writes on rewritten HTML must never be
// more storable than what the operator (add_header, a header module) or the
// origin stated for the page.  The module replaces every Cache-Control line
// with its own single line, so it carries over the two directives that limit
// who may store the page at all: private and no-store.
//
// Deliberately nginx-free so it can be unit tested standalone
// (test/pagespeed/nginx).

#ifndef NGX_CACHE_CONTROL_MERGE_H_
#define NGX_CACHE_CONTROL_MERGE_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

struct NgxCacheControlLimits {
  bool is_private = false;
  bool no_store = false;
};

// Notes the private and no-store directives of one Cache-Control field value
// (one header line, itself a comma-separated list) in *limits.  Directive
// names compare case-insensitively; a qualified private="field" counts as
// private.  Call once per line; a directive found on any line stays noted.
void NgxNoteCacheControlLimits(StringPiece field_value,
                               NgxCacheControlLimits* limits);

// Returns module_value with ", private" and ", no-store" appended for each
// directive noted in limits that module_value does not already carry.  With
// nothing noted, returns module_value unchanged.
GoogleString NgxKeepCacheControlLimits(StringPiece module_value,
                                       const NgxCacheControlLimits& limits);

}  // namespace net_instaweb

#endif  // NGX_CACHE_CONTROL_MERGE_H_
