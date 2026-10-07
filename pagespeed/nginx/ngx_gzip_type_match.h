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

#ifndef PAGESPEED_NGINX_NGX_GZIP_TYPE_MATCH_H_
#define PAGESPEED_NGINX_NGX_GZIP_TYPE_MATCH_H_

namespace net_instaweb {

// True when `media_type` is one a response is gzipped for, matched the way
// nginx matches a Content-Type against its gzip_types
// (ngx_http_test_content_type): the type/subtype up to the first ';' --
// parameters such as "; charset=utf-8" are ignored -- trailing blanks
// trimmed, compared CASE-INSENSITIVELY.  "text/html" is always true: nginx
// compresses it as the implied default and it cannot be unlisted.
//
// `types` is a null-terminated array of NUL-terminated type/subtype
// strings; a null or empty `media_type` is false.  Deliberately nginx-free
// so the matching rules are unit tested standalone
// (//test/pagespeed/nginx:ngx_gzip_type_match_test): the caller's
// Content-Type comes in verbatim, parameters and case included.
bool NgxGZipTypeListMatches(const char* const* types, const char* media_type);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_GZIP_TYPE_MATCH_H_
