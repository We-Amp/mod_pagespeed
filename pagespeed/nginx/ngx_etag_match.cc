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

#include "pagespeed/nginx/ngx_etag_match.h"

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

bool NgxIfNoneMatchMatches(StringPiece field_value, StringPiece etag) {
  // "*" matches any current representation -- but only as the WHOLE field,
  // exactly one byte, the way nginx's ngx_http_test_if_match treats it;
  // a "*" inside a list is just a candidate that cannot match a quoted tag.
  if (field_value == "*") {
    return true;
  }
  if (etag.starts_with("W/")) {
    etag.remove_prefix(2);
  }
  // A server entity-tag is quoted by definition, opening AND closing; an
  // unquoted or unterminated one is not a tag and never matches.
  if (etag.size() < 2 || etag[0] != '"' || etag[etag.size() - 1] != '"') {
    return false;
  }
  // Weak comparison over the comma-separated list: trim each candidate,
  // ignore a leading W/ on it too, and compare the opaque tags byte for
  // byte, quotes included. The split is quote-aware: a comma inside a
  // quoted tag does not end the candidate (RFC 9110's etagc includes ",").
  size_t pos = 0;
  const size_t n = field_value.size();
  while (pos < n) {
    bool in_quotes = false;
    size_t end = pos;
    while (end < n) {
      const char c = field_value[end];
      if (c == '"') {
        in_quotes = !in_quotes;
      } else if (c == ',' && !in_quotes) {
        break;
      }
      ++end;
    }
    StringPiece candidate = field_value.substr(pos, end - pos);
    TrimWhitespace(&candidate);
    if (candidate.starts_with("W/")) {
      candidate.remove_prefix(2);
    }
    if (candidate == etag) {
      return true;
    }
    pos = end + 1;  // skip the comma, or walk off the end
  }
  return false;
}

}  // namespace net_instaweb
