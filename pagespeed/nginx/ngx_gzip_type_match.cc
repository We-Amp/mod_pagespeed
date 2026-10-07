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

#include "pagespeed/nginx/ngx_gzip_type_match.h"

#include <cstring>

namespace net_instaweb {

bool NgxGZipTypeListMatches(const char* const* types, const char* media_type) {
  if (media_type == nullptr) {
    return false;
  }
  // The part nginx compares: everything before the first ';', trailing
  // blanks trimmed.  An empty remainder is not a type.
  size_t len = strcspn(media_type, ";");
  while (len > 0 &&
         (media_type[len - 1] == ' ' || media_type[len - 1] == '\t')) {
    --len;
  }
  if (len == 0) {
    return false;
  }
  if (len == strlen("text/html") &&
      strncasecmp(media_type, "text/html", len) == 0) {
    // The implied default: compressed whether or not it is listed.
    return true;
  }
  for (const char* const* type = types; type != nullptr && *type != nullptr;
       ++type) {
    if (strlen(*type) == len && strncasecmp(media_type, *type, len) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace net_instaweb
