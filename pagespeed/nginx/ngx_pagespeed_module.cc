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

/*
 * nginx dynamic module entry point for ngx_pagespeed.
 *
 * This file exports the ngx_modules array that nginx expects to find
 * when loading a dynamic module via dlsym().
 *
 * nginx dynamic modules must export:
 * - ngx_modules: Array of pointers to ngx_module_t structures
 * - ngx_module_names: Array of module name strings (optional in newer nginx)
 * - ngx_module_order: Module load order (optional)
 */

extern "C" {

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

}  // extern "C"

// Forward declarations of the module structures defined in ngx_pagespeed.cc
extern ngx_module_t ngx_pagespeed;
extern ngx_module_t ngx_pagespeed_etag_filter;

// The ngx_modules array that nginx looks for when loading a dynamic module.
// This array contains pointers to all modules provided by this shared library.
// It must be terminated with NULL.
//
// IMPORTANT: This symbol must be visible (not hidden) for dlsym() to find it.
// The array is marked as extern "C" to ensure C linkage for compatibility.
extern "C" {

__attribute__((visibility("default"))) ngx_module_t* ngx_modules[] = {
    &ngx_pagespeed, &ngx_pagespeed_etag_filter, nullptr};

// Module names array (required by some nginx versions)
__attribute__((visibility("default"))) char* ngx_module_names[] = {
    (char*)"ngx_pagespeed", (char*)"ngx_pagespeed_etag_filter", nullptr};

// Where nginx's dynamic loader places this module's two filters relative to
// the other filter modules. ngx_add_module scans the names listed AFTER the
// module's own name and inserts it at the LOWEST index any of them already
// holds (not necessarily the first one named), and filters run in reverse
// list order. So: the rewriting filter runs immediately before the
// compression filters -- after postpone, SSI, charset, xslt, image, sub,
// addition, gunzip, userid and the headers filter (expires/add_header), and
// after the copy filter has read file buffers into memory -- and the etag
// filter runs immediately after compression, before the range header filter.
__attribute__((visibility("default"))) char* ngx_module_order[] = {
    (char*)"ngx_http_range_header_filter_module",
    (char*)"ngx_pagespeed_etag_filter",
    (char*)"ngx_http_gzip_filter_module",
    (char*)"ngx_http_brotli_filter_module",
    (char*)"ngx_pagespeed",
    (char*)"ngx_http_postpone_filter_module",
    (char*)"ngx_http_ssi_filter_module",
    (char*)"ngx_http_charset_filter_module",
    (char*)"ngx_http_xslt_filter_module",
    (char*)"ngx_http_image_filter_module",
    (char*)"ngx_http_sub_filter_module",
    (char*)"ngx_http_addition_filter_module",
    (char*)"ngx_http_gunzip_filter_module",
    (char*)"ngx_http_userid_filter_module",
    (char*)"ngx_http_headers_filter_module",
    nullptr};

}  // extern "C"
