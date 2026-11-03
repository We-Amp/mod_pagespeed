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

#include "pagespeed/nginx/ngx_cache_control_merge.h"

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// Calls fn with the trimmed name of each directive in a Cache-Control field
// value: the part of each comma-separated element before any "=".
template <class Fn>
void ForEachDirectiveName(StringPiece field_value, Fn fn) {
  StringPieceVector elements;
  SplitStringPieceToVector(field_value, ",", &elements,
                           true /* omit empty strings */);
  for (StringPiece element : elements) {
    StringPiece::size_type eq = element.find('=');
    StringPiece name =
        eq == StringPiece::npos ? element : element.substr(0, eq);
    TrimWhitespace(&name);
    fn(name);
  }
}

}  // namespace

void NgxNoteCacheControlLimits(StringPiece field_value,
                               NgxCacheControlLimits* limits) {
  ForEachDirectiveName(field_value, [limits](StringPiece name) {
    if (StringCaseEqual(name, "private")) {
      limits->is_private = true;
    } else if (StringCaseEqual(name, "no-store")) {
      limits->no_store = true;
    }
  });
}

GoogleString NgxKeepCacheControlLimits(StringPiece module_value,
                                       const NgxCacheControlLimits& limits) {
  NgxCacheControlLimits present;
  NgxNoteCacheControlLimits(module_value, &present);
  GoogleString result(module_value.data(), module_value.size());
  if (limits.is_private && !present.is_private) {
    StrAppend(&result, result.empty() ? "" : ", ", "private");
  }
  if (limits.no_store && !present.no_store) {
    StrAppend(&result, result.empty() ? "" : ", ", "no-store");
  }
  return result;
}

}  // namespace net_instaweb
