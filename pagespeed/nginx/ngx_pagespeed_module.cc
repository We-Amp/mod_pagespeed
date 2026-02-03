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

__attribute__((visibility("default")))
ngx_module_t* ngx_modules[] = {
    &ngx_pagespeed,
    &ngx_pagespeed_etag_filter,
    NULL
};

// Module names array (required by some nginx versions)
__attribute__((visibility("default")))
char* ngx_module_names[] = {
    (char*)"ngx_pagespeed",
    (char*)"ngx_pagespeed_etag_filter",
    NULL
};

// Module order string - determines when our filter runs relative to others.
// This is used by nginx's dynamic module loader to insert the module
// at the correct position in the filter chain.
// Format: "module_name:position" where position is relative to other modules.
__attribute__((visibility("default")))
char* ngx_module_order[] = {
    (char*)"ngx_pagespeed",
    (char*)"ngx_pagespeed_etag_filter",
    NULL
};

}  // extern "C"
