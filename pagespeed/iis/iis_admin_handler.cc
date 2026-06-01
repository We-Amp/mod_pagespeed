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

#include "pagespeed/iis/iis_admin_handler.h"

#include <exception>

#include "base/logging.h"
#include "pagespeed/iis/iis_config.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#endif

namespace net_instaweb {

namespace {
// Simple JSON string escaping
GoogleString JsonEscape(const GoogleString& input) {
  GoogleString output;
  output.reserve(input.size() + 10);  // Reserve extra for escapes
  for (char c : input) {
    switch (c) {
      case '"':  output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Control character - encode as \u00XX
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          output += buf;
        } else {
          output += c;
        }
        break;
    }
  }
  return output;
}

// Get appropriate HTTP reason phrase for status code
const char* GetReasonPhrase(int status_code) {
  switch (status_code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "Unknown";
  }
}
}  // namespace

bool IisAdminHandler::HandleRequest(IHttpContext* context,
                                    IisServerContext* server_context,
                                    const GoogleString& path) {
  // Validate inputs
  if (context == nullptr || server_context == nullptr) {
    LOG(ERROR) << "PageSpeed Admin: Null context or server_context";
    return false;
  }

  try {
    if (!IsAdminPath(path) && !IsStatisticsPath(path)) {
      return false;
    }

    // Check authentication for admin paths (statistics path may be public)
    if (IsAdminPath(path) && !IsAuthorized(context, server_context)) {
      SendErrorResponse(context, 403, "Access denied. Admin authentication required.");
      return true;
    }

    // Optionally require authentication for statistics endpoint
    const IisConfig* config = server_context->config();
    if (IsStatisticsPath(path) && config != nullptr &&
        config->statistics_auth_required() && !IsAuthorized(context, server_context)) {
      SendErrorResponse(context, 403, "Access denied. Statistics authentication required.");
      return true;
    }

    GoogleString output;
    GoogleString content_type;

    // Handle POST requests for cache operations
    IHttpRequest* request = context->GetRequest();
    if (request != nullptr) {
      HTTP_REQUEST* raw_request = request->GetRawHttpRequest();
      if (raw_request != nullptr && raw_request->Verb == HttpVerbPOST) {
        GoogleString subpath = path.substr(strlen("/pagespeed_admin"));
        if (subpath == "/cache" || StringPiece(subpath).starts_with("/cache?")) {
          // Parse action from query string
          if (path.find("action=flush") != GoogleString::npos) {
            return HandleCacheFlush(context, server_context);
          } else if (path.find("action=purge") != GoogleString::npos) {
            // Extract URL to purge
            size_t url_pos = path.find("url=");
            if (url_pos != GoogleString::npos) {
              GoogleString url = path.substr(url_pos + 4);
              return HandleCachePurge(context, server_context, url);
            }
          }
        }
      }
    }

    if (IsStatisticsPath(path)) {
      GenerateStatisticsPage(server_context, &output, &content_type);
    } else {
      GenerateAdminPage(path, server_context, &output, &content_type);
    }

    SendResponse(context, 200, content_type, output);
    return true;
  } catch (const std::exception& e) {
    LOG(ERROR) << "PageSpeed Admin: Exception handling request: " << e.what();
    SendErrorResponse(context, 500, "Internal server error");
    return true;
  } catch (...) {
    LOG(ERROR) << "PageSpeed Admin: Unknown exception handling request";
    SendErrorResponse(context, 500, "Internal server error");
    return true;
  }
}

bool IisAdminHandler::IsAuthorized(IHttpContext* context,
                                   IisServerContext* server_context) {
  const IisConfig* config = server_context->config();
  if (config == nullptr || !config->admin_enabled()) {
    return false;
  }

  // If authentication is not required, allow access from localhost
  if (!config->admin_auth_required()) {
    GoogleString client_ip = GetClientIp(context, server_context);
    if (IsIpAllowed(client_ip, server_context)) {
      return true;
    }
  }

  // Check if request is from an allowed IP
  GoogleString client_ip = GetClientIp(context, server_context);
  if (IsIpAllowed(client_ip, server_context)) {
    return true;
  }

  // Check for authentication token
  return ValidateToken(context, server_context);
}

bool IisAdminHandler::IsAdminPath(const GoogleString& path) {
  // Strip query string if present before checking path
  GoogleString base_path = path;
  size_t query_pos = base_path.find('?');
  if (query_pos != GoogleString::npos) {
    base_path = base_path.substr(0, query_pos);
  }
  return base_path == "/pagespeed_admin" ||
         StringPiece(base_path).starts_with("/pagespeed_admin/");
}

bool IisAdminHandler::IsStatisticsPath(const GoogleString& path) {
  return path == "/pagespeed_statistics" ||
         path == "/mod_pagespeed_statistics";
}

void IisAdminHandler::GenerateAdminPage(const GoogleString& path,
                                        IisServerContext* server_context,
                                        GoogleString* output,
                                        GoogleString* content_type) {
  *content_type = "text/html; charset=utf-8";

  // Determine which admin page to generate
  GoogleString subpath = path.substr(strlen("/pagespeed_admin"));
  // Remove query string if present
  size_t query_pos = subpath.find('?');
  if (query_pos != GoogleString::npos) {
    subpath = subpath.substr(0, query_pos);
  }

  if (subpath.empty() || subpath == "/") {
    // Main admin page - generate simple dashboard
    *output = StrCat(
        "<!DOCTYPE html>\n"
        "<html><head><title>PageSpeed Admin</title>\n"
        "<style>\n"
        "body { font-family: sans-serif; margin: 20px; }\n"
        "h1 { color: #333; }\n"
        ".nav { background: #f5f5f5; padding: 15px; border-radius: 5px; }\n"
        ".nav a { margin-right: 20px; }\n"
        "</style>\n"
        "</head><body>\n"
        "<h1>PageSpeed Admin</h1>\n"
        "<div class=\"nav\">\n"
        "<a href=\"/pagespeed_admin/statistics\">Statistics</a>\n"
        "<a href=\"/pagespeed_admin/config\">Configuration</a>\n"
        "<a href=\"/pagespeed_admin/cache\">Cache Management</a>\n"
        "<a href=\"/pagespeed_admin/messages\">Messages</a>\n"
        "</div>\n"
        "<h2>Status</h2>\n"
        "<p>PageSpeed is running.</p>\n"
        "</body></html>\n");
  } else if (subpath == "/statistics" || subpath == "/stats") {
    GenerateStatisticsPage(server_context, output, content_type);
  } else if (subpath == "/config") {
    // Configuration page - show current configuration
    GenerateConfigPage(server_context, output, content_type);
  } else if (subpath == "/console") {
    // Console page - simplified version for IIS
    *output = "{\"status\": \"ok\", \"message\": \"IIS console endpoint\"}";
    *content_type = "application/json";
  } else if (subpath == "/cache") {
    // Cache management page
    GenerateCachePage(nullptr, server_context, output, content_type);
  } else if (subpath == "/messages") {
    // Recent messages page
    GenerateMessagesPage(server_context, output, content_type);
  } else {
    *output = StrCat(
        "<html><body><h1>PageSpeed Admin</h1>"
        "<p>Unknown admin path: ", subpath, "</p>"
        "<p><a href=\"/pagespeed_admin\">Back to admin</a></p>"
        "</body></html>");
  }
}

void IisAdminHandler::GenerateStatisticsPage(IisServerContext* server_context,
                                             GoogleString* output,
                                             GoogleString* content_type) {
  *content_type = "text/plain; charset=utf-8";

  // Generate statistics in text format
  Statistics* stats = server_context->statistics();
  if (stats == nullptr) {
    *output = "Statistics not available";
    return;
  }

  StringWriter writer(output);
  stats->Dump(&writer, server_context->message_handler());
}

void IisAdminHandler::GenerateConfigPage(IisServerContext* server_context,
                                         GoogleString* output,
                                         GoogleString* content_type) {
  *content_type = "text/html; charset=utf-8";

  const RewriteOptions* options = server_context->global_options();
  GoogleString options_str;
  if (options != nullptr) {
    options_str = options->OptionsToString();
  } else {
    options_str = "No options available";
  }

  // Escape HTML in options string
  GoogleString escaped_options;
  HtmlKeywords::Escape(options_str, &escaped_options);

  *output = StrCat(
      "<!DOCTYPE html>\n"
      "<html><head><title>PageSpeed Configuration</title>\n"
      "<style>\n"
      "body { font-family: sans-serif; margin: 20px; }\n"
      "pre { background: #f5f5f5; padding: 15px; border-radius: 5px; "
      "overflow-x: auto; }\n"
      "</style>\n"
      "</head><body>\n"
      "<h1>PageSpeed Configuration</h1>\n"
      "<p><a href=\"/pagespeed_admin\">&laquo; Back to Admin</a></p>\n"
      "<h2>Current Options</h2>\n"
      "<pre>", escaped_options, "</pre>\n"
      "</body></html>\n");
}

void IisAdminHandler::SendResponse(IHttpContext* context,
                                   int status_code,
                                   const GoogleString& content_type,
                                   const GoogleString& body) {
  IHttpResponse* response = context->GetResponse();

  // Clear any existing response
  response->Clear();

  // Set status
  response->SetStatus(static_cast<USHORT>(status_code), GetReasonPhrase(status_code));

  // Set content type
  response->SetHeader("Content-Type",
                      content_type.c_str(),
                      static_cast<USHORT>(content_type.size()),
                      TRUE);

  // Set content length
  char length_buf[32];
  snprintf(length_buf, sizeof(length_buf), "%zu", body.size());
  response->SetHeader("Content-Length",
                      length_buf,
                      static_cast<USHORT>(strlen(length_buf)),
                      TRUE);

  // Write body
  if (!body.empty()) {
    HTTP_DATA_CHUNK chunk;
    chunk.DataChunkType = HttpDataChunkFromMemory;
    chunk.FromMemory.pBuffer = const_cast<char*>(body.data());
    chunk.FromMemory.BufferLength = static_cast<ULONG>(body.size());

    DWORD bytes_sent = 0;
    BOOL completion_expected = FALSE;

    response->WriteEntityChunks(&chunk, 1, FALSE, FALSE,
                                &bytes_sent, &completion_expected);
  }
}

bool IisAdminHandler::HandleCacheFlush(IHttpContext* context,
                                       IisServerContext* server_context) {
  // Flush all caches
  server_context->FlushCacheIfNecessary();

  LOG(INFO) << "PageSpeed: Cache flushed via admin request";

  GoogleString response = "{\"status\": \"ok\", \"message\": \"Cache flushed\"}";
  SendResponse(context, 200, "application/json", response);
  return true;
}

bool IisAdminHandler::HandleCachePurge(IHttpContext* context,
                                       IisServerContext* server_context,
                                       const GoogleString& url) {
  // Purge specific URL from cache
  // This uses the invalidation mechanism
  bool purged = false;

  // TODO: Implement URL-specific cache purge
  // For now, we flush the entire cache
  server_context->FlushCacheIfNecessary();
  purged = true;

  LOG(INFO) << "PageSpeed: Cache purged for URL: " << url;

  GoogleString response;
  if (purged) {
    response = StrCat("{\"status\": \"ok\", \"message\": \"URL purged: ",
                      JsonEscape(url), "\"}");
    SendResponse(context, 200, "application/json", response);
  } else {
    response = StrCat("{\"status\": \"error\", \"message\": \"Failed to purge: ",
                      JsonEscape(url), "\"}");
    SendResponse(context, 500, "application/json", response);
  }
  return true;
}

void IisAdminHandler::GenerateCachePage(IHttpContext* context,
                                        IisServerContext* server_context,
                                        GoogleString* output,
                                        GoogleString* content_type) {
  *content_type = "text/html; charset=utf-8";

  GoogleString cache_stats;
  GetCacheStats(server_context, &cache_stats);

  *output = StrCat(
      "<!DOCTYPE html>\n"
      "<html><head><title>PageSpeed Cache Management</title>\n"
      "<style>\n"
      "body { font-family: sans-serif; margin: 20px; }\n"
      "h1 { color: #333; }\n"
      ".stats { background: #f5f5f5; padding: 15px; border-radius: 5px; }\n"
      ".actions { margin-top: 20px; }\n"
      "button { padding: 10px 20px; margin-right: 10px; cursor: pointer; }\n"
      ".success { color: green; }\n"
      ".error { color: red; }\n"
      "</style>\n"
      "</head><body>\n"
      "<h1>PageSpeed Cache Management</h1>\n"
      "<p><a href=\"/pagespeed_admin\">&laquo; Back to Admin</a></p>\n"
      "<div class=\"stats\">\n"
      "<h2>Cache Statistics</h2>\n"
      "<pre>", cache_stats, "</pre>\n"
      "</div>\n"
      "<div class=\"actions\">\n"
      "<h2>Cache Actions</h2>\n"
      "<form method=\"post\" action=\"/pagespeed_admin/cache?action=flush\" "
      "onsubmit=\"return confirm('Are you sure you want to flush the entire cache?');\">\n"
      "<button type=\"submit\">Flush All Caches</button>\n"
      "</form>\n"
      "</div>\n"
      "</body></html>\n");
}

void IisAdminHandler::GenerateMessagesPage(IisServerContext* server_context,
                                           GoogleString* output,
                                           GoogleString* content_type) {
  *content_type = "text/html; charset=utf-8";

  // Get recent messages from the IIS message handler
  IisMessageHandler* handler = dynamic_cast<IisMessageHandler*>(
      server_context->message_handler());

  GoogleString messages_html;
  if (handler != nullptr) {
    std::vector<GoogleString> messages;
    handler->GetRecentMessages(&messages);

    GoogleString escaped_buf;
    for (const auto& msg : messages) {
      StrAppend(&messages_html, "<div class=\"message\">",
                HtmlKeywords::Escape(msg, &escaped_buf), "</div>\n");
    }

    if (messages.empty()) {
      messages_html = "<p>No recent messages.</p>";
    }
  } else {
    messages_html = "<p>Message handler not available.</p>";
  }

  *output = StrCat(
      "<!DOCTYPE html>\n"
      "<html><head><title>PageSpeed Messages</title>\n"
      "<style>\n"
      "body { font-family: monospace; margin: 20px; }\n"
      ".message { padding: 5px; border-bottom: 1px solid #eee; }\n"
      "</style>\n"
      "</head><body>\n"
      "<h1>Recent PageSpeed Messages</h1>\n"
      "<p><a href=\"/pagespeed_admin\">&laquo; Back to Admin</a></p>\n"
      "<div class=\"messages\">\n",
      messages_html,
      "</div>\n"
      "</body></html>\n");
}

void IisAdminHandler::GetCacheStats(IisServerContext* server_context,
                                    GoogleString* output) {
  StringWriter writer(output);
  Statistics* stats = server_context->statistics();

  if (stats != nullptr) {
    // Get cache-related statistics
    writer.Write("File cache hits: ", server_context->message_handler());
    // Write specific cache stats
    stats->Dump(&writer, server_context->message_handler());
  } else {
    writer.Write("Statistics not available", server_context->message_handler());
  }
}

void IisAdminHandler::SendErrorResponse(IHttpContext* context,
                                        int status_code,
                                        const GoogleString& message) {
  GoogleString body = StrCat(
      "{\"status\": \"error\", \"code\": ", IntegerToString(status_code),
      ", \"message\": \"", JsonEscape(message), "\"}");
  SendResponse(context, status_code, "application/json", body);
}

GoogleString IisAdminHandler::GetClientIp(IHttpContext* context,
                                          IisServerContext* server_context) {
  IHttpRequest* request = context->GetRequest();
  if (request == nullptr) {
    return "";
  }

  // First, get the direct remote IP address from the socket connection
  GoogleString direct_ip;
  HTTP_REQUEST* raw_request = request->GetRawHttpRequest();
  if (raw_request != nullptr && raw_request->Address.pRemoteAddress != nullptr) {
    SOCKADDR* addr = raw_request->Address.pRemoteAddress;
    char ip_str[INET6_ADDRSTRLEN];

    if (addr->sa_family == AF_INET) {
      SOCKADDR_IN* addr_in = reinterpret_cast<SOCKADDR_IN*>(addr);
      inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
      direct_ip = ip_str;
    } else if (addr->sa_family == AF_INET6) {
      SOCKADDR_IN6* addr_in6 = reinterpret_cast<SOCKADDR_IN6*>(addr);
      inet_ntop(AF_INET6, &addr_in6->sin6_addr, ip_str, sizeof(ip_str));
      direct_ip = ip_str;
    }
  }

  // Only trust X-Forwarded-For header if the direct connection is from a
  // trusted proxy. This prevents IP spoofing attacks where an attacker sends
  // a forged X-Forwarded-For header to bypass IP-based access controls.
  const IisConfig* config = server_context != nullptr ?
      server_context->config() : nullptr;
  if (config != nullptr && config->IsTrustedProxy(direct_ip)) {
    USHORT forwarded_for_len = 0;
    PCSTR forwarded_for = request->GetHeader("X-Forwarded-For",
                                             &forwarded_for_len);
    if (forwarded_for != nullptr && forwarded_for_len > 0) {
      GoogleString ip(forwarded_for, forwarded_for_len);
      // Take the first IP if there are multiple (leftmost is original client)
      size_t comma = ip.find(',');
      if (comma != GoogleString::npos) {
        ip = ip.substr(0, comma);
      }
      GoogleString trimmed_ip;
      TrimWhitespace(ip, &trimmed_ip);
      return trimmed_ip;
    }
  }

  return direct_ip;
}

bool IisAdminHandler::IsIpAllowed(const GoogleString& client_ip,
                                  IisServerContext* server_context) {
  // Check against configured allowed IP list from IisConfig
  const IisConfig* config = server_context->config();
  if (config != nullptr) {
    return config->IsIpAllowed(client_ip);
  }

  // Default allowed IPs: localhost
  if (client_ip == "127.0.0.1" || client_ip == "::1" || client_ip == "localhost") {
    return true;
  }

  return false;
}

bool IisAdminHandler::ValidateToken(IHttpContext* context,
                                    IisServerContext* server_context) {
  IHttpRequest* request = context->GetRequest();
  if (request == nullptr) {
    return false;
  }

  // Get token from header
  USHORT token_len = 0;
  PCSTR token = request->GetHeader("X-PageSpeed-Admin-Token", &token_len);

  if (token == nullptr || token_len == 0) {
    return false;
  }

  // Validate against configured token from IisConfig
  const IisConfig* config = server_context->config();
  if (config != nullptr && !config->admin_token().empty()) {
    GoogleString provided_token(token, token_len);
    return ConstantTimeCompare(provided_token, config->admin_token());
  }

  return false;
}

}  // namespace net_instaweb
