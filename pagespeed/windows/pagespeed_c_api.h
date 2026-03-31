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

// C API for pagespeed_automatic.
//
// This provides a stable C ABI boundary so that the pagespeed library
// (built with any C++ compiler/runtime) can be consumed by an IIS native
// module (built with MSVC).  All strings are passed as pointer+length pairs
// to avoid std::string ABI issues.

#ifndef PAGESPEED_WINDOWS_PAGESPEED_C_API_H_
#define PAGESPEED_WINDOWS_PAGESPEED_C_API_H_

#include <stddef.h>

#ifdef _WIN32
#ifdef PAGESPEED_DLL_EXPORT
#define PAGESPEED_API __declspec(dllexport)
#else
#define PAGESPEED_API __declspec(dllimport)
#endif
#else
#define PAGESPEED_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles.
typedef struct pagespeed_instance* pagespeed_t;
typedef struct pagespeed_request* pagespeed_request_t;

// Return codes.
#define PAGESPEED_OK 0
#define PAGESPEED_ERROR (-1)

// ---------------------------------------------------------------------------
// Global lifecycle
// ---------------------------------------------------------------------------

// Must be called once before any other pagespeed_* function.
PAGESPEED_API int pagespeed_global_init(void);

// Must be called once at process shutdown.
PAGESPEED_API void pagespeed_global_shutdown(void);

// ---------------------------------------------------------------------------
// Instance lifecycle
// ---------------------------------------------------------------------------

// Create a pagespeed instance.
// |config_path| - directory containing pagespeed.conf (may be NULL for
//                 defaults).
// |cache_path|  - directory for the file/disk cache.
// Returns NULL on failure.
PAGESPEED_API pagespeed_t pagespeed_create(const char* config_path,
                                           const char* cache_path);

PAGESPEED_API void pagespeed_destroy(pagespeed_t ps);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Set a PageSpeed option (e.g., "EnableFilters", "rewrite_css,rewrite_images").
// Returns PAGESPEED_OK on success.
PAGESPEED_API int pagespeed_set_option(pagespeed_t ps, const char* name,
                                       const char* value);

// ---------------------------------------------------------------------------
// Per-request operations
// ---------------------------------------------------------------------------

// Create a request context for the given URL.
PAGESPEED_API pagespeed_request_t pagespeed_request_create(pagespeed_t ps,
                                                           const char* url);

// Add a request header (call before rewrite/optimize functions).
PAGESPEED_API void pagespeed_request_set_header(pagespeed_request_t req,
                                                const char* name,
                                                const char* value);

// Rewrite an HTML document.
// |html_in|/|html_in_len| - input HTML.
// |html_out|/|html_out_len| - output buffer allocated by pagespeed.
//   Caller must free with pagespeed_free_buffer().
// Returns PAGESPEED_OK on success.
PAGESPEED_API int pagespeed_rewrite_html(pagespeed_request_t req,
                                         const char* html_in,
                                         size_t html_in_len, char** html_out,
                                         size_t* html_out_len);

// Check if a resource URL should be optimized (IPRO lookup).
// Returns 1 if yes, 0 if no.
PAGESPEED_API int pagespeed_should_optimize_resource(pagespeed_request_t req,
                                                     const char* url);

// Optimize a resource (CSS, JS, image) in-place.
// |resource_in|/|resource_in_len| - input resource bytes.
// |content_type| - MIME type (e.g. "text/css").
// |resource_out|/|resource_out_len| - output buffer allocated by pagespeed.
//   Caller must free with pagespeed_free_buffer().
// Returns PAGESPEED_OK on success.
PAGESPEED_API int pagespeed_optimize_resource(
    pagespeed_request_t req, const char* resource_in, size_t resource_in_len,
    const char* content_type, char** resource_out, size_t* resource_out_len);

// Free a buffer returned by pagespeed_rewrite_html or
// pagespeed_optimize_resource.
PAGESPEED_API void pagespeed_free_buffer(char* buf);

// Destroy a request context.
PAGESPEED_API void pagespeed_request_destroy(pagespeed_request_t req);

#ifdef __cplusplus
}
#endif

#endif  // PAGESPEED_WINDOWS_PAGESPEED_C_API_H_
