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

#ifndef PAGESPEED_IIS_IIS_UTILS_H_
#define PAGESPEED_IIS_IIS_UTILS_H_

#ifdef _WIN32

// Windows headers - winsock2.h must come before windows.h and httpserv.h
// to avoid redefinition errors with winsock.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <httpserv.h>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Utility functions for common IIS operations.
// These functions consolidate duplicate code across the IIS module.
namespace iis_utils {

// =============================================================================
// URL and Path Extraction
// =============================================================================

// Extracts the full URL (scheme + host + path + query) from an IIS context.
// Returns an empty string if the URL cannot be extracted.
// Example: "http://example.com/path/file.html?query=value"
GoogleString ExtractUrl(IHttpContext* context);

// Extracts just the path portion of the URL from an IIS context.
// Returns an empty string if the path cannot be extracted.
// Example: "/path/file.html" (without query string)
GoogleString ExtractPath(IHttpContext* context);

// Extracts the path including query string from an IIS context.
// Returns an empty string if the path cannot be extracted.
// Example: "/path/file.html?query=value"
GoogleString ExtractPathWithQuery(IHttpContext* context);

// =============================================================================
// Response Helpers
// =============================================================================

// Sends a simple HTTP response with the given status, reason phrase, and body.
// Clears any existing response content before writing.
// Sets Content-Type to "text/html; charset=utf-8" by default.
// Returns true if the response was sent successfully.
bool SendSimpleResponse(IHttpContext* context,
                        int status,
                        const char* reason,
                        const GoogleString& body);

// Sends a simple HTTP response with a custom content type.
// Clears any existing response content before writing.
// Returns true if the response was sent successfully.
bool SendSimpleResponse(IHttpContext* context,
                        int status,
                        const char* reason,
                        const GoogleString& content_type,
                        const GoogleString& body);

// Sends a JSON response with the given status code and JSON body.
// Automatically sets Content-Type to "application/json".
// Returns true if the response was sent successfully.
bool SendJsonResponse(IHttpContext* context,
                      int status,
                      const GoogleString& json_body);

// =============================================================================
// Header Utilities
// =============================================================================

// Sets a response header, handling the conversion to IIS types.
// If replace is true, replaces any existing header with the same name.
// Returns true if the header was set successfully.
bool SetResponseHeader(IHttpResponse* response,
                       const char* name,
                       const GoogleString& value,
                       bool replace = true);

// Gets a request header value by name.
// Returns an empty string if the header is not present.
GoogleString GetRequestHeader(IHttpRequest* request, const char* name);

// Gets a response header value by name.
// Returns an empty string if the header is not present.
GoogleString GetResponseHeader(IHttpResponse* response, const char* name);

// =============================================================================
// Reason Phrase Utilities
// =============================================================================

// Returns the standard HTTP reason phrase for a status code.
// Returns "Unknown" for unrecognized status codes.
const char* GetReasonPhrase(int status_code);

// =============================================================================
// Memory Allocation Helpers
// =============================================================================

// Allocates memory from the IIS request memory pool.
// Memory is automatically freed when the request completes.
// Returns nullptr on failure.
void* AllocateRequestMemory(IHttpContext* context, size_t size);

// Writes a body to the response using request-allocated memory.
// The memory is allocated from the IIS request pool to ensure proper lifetime.
// Returns true if successful.
bool WriteResponseBody(IHttpContext* context,
                       IHttpResponse* response,
                       const GoogleString& body);

}  // namespace iis_utils

}  // namespace net_instaweb

#endif  // _WIN32

#endif  // PAGESPEED_IIS_IIS_UTILS_H_
