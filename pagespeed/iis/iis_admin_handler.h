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

#ifndef PAGESPEED_IIS_IIS_ADMIN_HANDLER_H_
#define PAGESPEED_IIS_IIS_ADMIN_HANDLER_H_

// Windows headers - winsock2.h must come before windows.h and httpserv.h
// to avoid redefinition errors with winsock.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

// IIS Native Module API headers
#include <httpserv.h>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

class IisServerContext;

// Handler for PageSpeed admin UI and statistics endpoints.
//
// This class delegates admin page generation to the shared AdminSite
// infrastructure (via SystemServerContext), producing the same admin UI
// as Apache/nginx/Envoy with tabs, graphs, histograms, and interactive
// console.
//
// Handled paths:
// - /pagespeed_admin[/...] - Admin dashboard (delegated to AdminSite)
// - /pagespeed_global_admin[/...] - Global admin (delegated to AdminSite)
// - /pagespeed_statistics  - Statistics (delegated to AdminSite)
// - /mod_pagespeed_statistics - Statistics (legacy path)
// - /pagespeed_admin?health - Health check (IIS-specific)
//
// Authentication:
// - When authentication is required, admin endpoints check either:
//   1. Request from allowed IP addresses (127.0.0.1, ::1 by default)
//   2. Valid authentication token in X-PageSpeed-Admin-Token header
class IisAdminHandler {
 public:
  // Handle an admin request.
  // |url| is the full request URL (e.g., "http://host/pagespeed_admin/cache").
  // |path| is the URL path (e.g., "/pagespeed_admin/cache").
  // Returns true if the request was handled (caller should finish request).
  // Returns false if this is not an admin request.
  static bool HandleRequest(
      IHttpContext* context,
      IisServerContext* server_context,
      const GoogleString& url,
      const GoogleString& path);

  // Check if a request is authorized for admin access.
  static bool IsAuthorized(
      IHttpContext* context,
      IisServerContext* server_context);

  // Check if path is an admin path (public for testability).
  static bool IsAdminPath(const GoogleString& path);

  // Check if path is a global admin path (public for testability).
  static bool IsGlobalAdminPath(const GoogleString& path);

  // Check if path is statistics path (public for testability).
  static bool IsStatisticsPath(const GoogleString& path);

 private:
  // Generate health check response (IIS-specific, not in AdminSite).
  static void GenerateHealthPage(
      IisServerContext* server_context,
      GoogleString* output,
      GoogleString* content_type);

  // Send response to IIS.
  static void SendResponse(
      IHttpContext* context,
      int status_code,
      const GoogleString& content_type,
      const GoogleString& body);

  // Send error response.
  static void SendErrorResponse(
      IHttpContext* context,
      int status_code,
      const GoogleString& message);

  // Handle cache flush request (POST /pagespeed_admin/cache?action=flush).
  static bool HandleCacheFlush(
      IHttpContext* context,
      IisServerContext* server_context);

  // Handle cache purge request (POST /pagespeed_admin/cache?action=purge&url=...).
  static bool HandleCachePurge(
      IHttpContext* context,
      IisServerContext* server_context,
      const GoogleString& url);

  // Get client IP address from request.
  static GoogleString GetClientIp(IHttpContext* context,
                                  IisServerContext* server_context);

  // Check if IP is in allowed list.
  static bool IsIpAllowed(
      const GoogleString& client_ip,
      IisServerContext* server_context);

  // Validate authentication token.
  static bool ValidateToken(
      IHttpContext* context,
      IisServerContext* server_context);

  IisAdminHandler(const IisAdminHandler&) = delete;
  IisAdminHandler& operator=(const IisAdminHandler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_ADMIN_HANDLER_H_
