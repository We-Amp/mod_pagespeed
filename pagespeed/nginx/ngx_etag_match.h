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
// Weak entity-tag comparison for If-None-Match (RFC 9110 section 13.1.2) for
// the nginx port. Headers of responses the module generates enter the filter
// chain at the module's own position, so nginx's not-modified filter -- which
// runs ahead of it -- never applies its conditional logic to them; the module
// answers If-None-Match for its own resources itself, with this as the
// comparison.
//
// Deliberately nginx-free so it can be unit tested standalone
// (test/pagespeed/nginx).

#ifndef NGX_ETAG_MATCH_H_
#define NGX_ETAG_MATCH_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Returns true when an If-None-Match field value matches the response's
// entity-tag under the WEAK comparison a GET calls for: "*" matches any
// current representation; otherwise the field is a comma-separated list of
// entity-tags, a leading "W/" is ignored on both sides, and the opaque tags
// are compared byte for byte, quotes included. An unquoted etag never
// matches (a server entity-tag is quoted by definition), and neither does an
// empty field.
bool NgxIfNoneMatchMatches(StringPiece field_value, StringPiece etag);

}  // namespace net_instaweb

#endif  // NGX_ETAG_MATCH_H_
