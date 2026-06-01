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
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/iis/iis_config.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/admin_site.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#endif

namespace net_instaweb {

namespace {

// Simple JSON string escaping.
GoogleString JsonEscape(const GoogleString& input) {
  GoogleString output;
  output.reserve(input.size() + 10);
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

// Get appropriate HTTP reason phrase for status code.
const char* GetReasonPhrase(int status_code) {
  switch (status_code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "Unknown";
  }
}

// Constant-time comparison to prevent timing attacks on token validation.
bool ConstantTimeCompare(const GoogleString& a, const GoogleString& b) {
  if (a.size() != b.size()) {
    return false;
  }
  volatile unsigned char result = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    result |= static_cast<unsigned char>(a[i]) ^
              static_cast<unsigned char>(b[i]);
  }
  return result == 0;
}

// AsyncFetch adapter that buffers the response and signals completion.
// Used to bridge AdminSite's async output to IIS's synchronous request model.
//
// AdminSite methods call Write()/Done() on an AsyncFetch. This class buffers
// all output and signals a Windows event when Done() is called, allowing the
// IIS worker thread to wait for completion and then write the response.
class IisAdminFetch : public StringAsyncFetch {
 public:
  explicit IisAdminFetch(const RequestContextPtr& ctx)
      : StringAsyncFetch(ctx), success_(false) {
    done_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  }

  ~IisAdminFetch() override {
    if (done_event_ != nullptr) {
      CloseHandle(done_event_);
    }
  }

  void HandleDone(bool success) override {
    success_ = success;
    SetEvent(done_event_);
  }

  // Block until Done() is called. Returns success status.
  bool WaitForCompletion() {
    WaitForSingleObject(done_event_, INFINITE);
    return success_;
  }

 private:
  HANDLE done_event_;
  bool success_;
};

}  // namespace

bool IisAdminHandler::HandleRequest(IHttpContext* context,
                                    IisServerContext* server_context,
                                    const GoogleString& url,
                                    const GoogleString& path) {
  if (context == nullptr || server_context == nullptr) {
    LOG(ERROR) << "PageSpeed Admin: Null context or server_context";
    return false;
  }

  try {
    if (!IsAdminPath(path) && !IsGlobalAdminPath(path) &&
        !IsStatisticsPath(path)) {
      return false;
    }

    // Check authentication for admin paths.
    if ((IsAdminPath(path) || IsGlobalAdminPath(path)) &&
        !IsAuthorized(context, server_context)) {
      SendErrorResponse(context, 403,
                        "Access denied. Admin authentication required.");
      return true;
    }

    // Optionally require authentication for statistics endpoint.
    const IisConfig* config = server_context->config();
    if (IsStatisticsPath(path) && config != nullptr &&
        config->statistics_auth_required() &&
        !IsAuthorized(context, server_context)) {
      SendErrorResponse(context, 403,
                        "Access denied. Statistics authentication required.");
      return true;
    }

    // Handle POST requests for cache operations (IIS-specific).
    IHttpRequest* request = context->GetRequest();
    if (request != nullptr) {
      HTTP_REQUEST* raw_request = request->GetRawHttpRequest();
      if (raw_request != nullptr && raw_request->Verb == HttpVerbPOST) {
        size_t prefix_len = IsGlobalAdminPath(path)
            ? strlen("/pagespeed_global_admin")
            : strlen("/pagespeed_admin");
        GoogleString subpath = path.substr(prefix_len);
        if (subpath == "/cache" ||
            StringPiece(subpath).starts_with("/cache?")) {
          if (path.find("action=flush") != GoogleString::npos) {
            return HandleCacheFlush(context, server_context);
          } else if (path.find("action=purge") != GoogleString::npos) {
            size_t url_pos = path.find("url=");
            if (url_pos != GoogleString::npos) {
              GoogleString purge_url = path.substr(url_pos + 4);
              return HandleCachePurge(context, server_context, purge_url);
            }
          }
        }
      }
    }

    // Determine which admin prefix matched (used for subpath extraction).
    bool is_global = IsGlobalAdminPath(path);
    const char* admin_prefix = is_global
        ? "/pagespeed_global_admin" : "/pagespeed_admin";
    size_t admin_prefix_len = strlen(admin_prefix);

    // Handle health check (IIS-specific, not in shared AdminSite).
    if (IsAdminPath(path) || is_global) {
      GoogleString subpath = path.substr(admin_prefix_len);
      GoogleString query_string;
      size_t query_pos = subpath.find('?');
      if (query_pos != GoogleString::npos) {
        query_string = subpath.substr(query_pos + 1);
        subpath = subpath.substr(0, query_pos);
      }
      if (subpath == "/health" ||
          ((subpath.empty() || subpath == "/") && query_string == "health")) {
        GoogleString output;
        GoogleString content_type;
        GenerateHealthPage(server_context, &output, &content_type);
        SendResponse(context, 200, content_type, output);
        return true;
      }
    }

    // Normalize the URL for AdminSite dispatch.
    // AdminSite uses path-based routing (/pagespeed_admin/console) while the
    // old IIS handler used query-string routing (/pagespeed_admin?console).
    // For backwards compatibility, map query-string routes to path routes.
    GoogleString effective_url = url;
    if (IsAdminPath(path) || is_global) {
      GoogleString subpath = path.substr(admin_prefix_len);
      GoogleString query_string;
      size_t query_pos = subpath.find('?');
      if (query_pos != GoogleString::npos) {
        query_string = subpath.substr(query_pos + 1);
        subpath = subpath.substr(0, query_pos);
      }

      // Map query-string routes to path-based routes for AdminSite.
      if ((subpath.empty() || subpath == "/") && !query_string.empty()) {
        // Extract the first query param key (e.g., "console" from "console")
        GoogleString first_key = query_string;
        size_t amp = first_key.find('&');
        if (amp != GoogleString::npos) {
          first_key = first_key.substr(0, amp);
        }
        size_t eq = first_key.find('=');
        if (eq != GoogleString::npos) {
          first_key = first_key.substr(0, eq);
        }

        // Map known query-string routes to path equivalents.
        // These match what AdminSite::AdminPage() dispatches on.
        GoogleString path_suffix;
        if (first_key == "console") {
          path_suffix = "/console";
        } else if (first_key == "messages") {
          path_suffix = "/message_history";
        } else if (first_key == "cache" || first_key == "cache_flush") {
          path_suffix = "/cache";
        } else if (first_key == "filters") {
          path_suffix = "/config";  // AdminSite shows filters in config page
        } else if (first_key == "histograms") {
          path_suffix = "/histograms";
        } else if (first_key == "graphs") {
          path_suffix = "/graphs";
        }

        if (!path_suffix.empty()) {
          // Rewrite URL: replace /pagespeed_admin?X with /pagespeed_admin/X
          GoogleUrl original_gurl(url);
          if (original_gurl.IsWebValid()) {
            effective_url = StrCat(
                original_gurl.Origin(), admin_prefix, path_suffix);
          }
        }
      }

      // If the browser requested /pagespeed_admin without a trailing slash,
      // send a 301 redirect to /pagespeed_admin/ so that relative links in
      // the admin UI (e.g. <a href='statistics'>) resolve correctly.
      // Without this redirect the browser resolves them against / instead
      // of /pagespeed_admin/.
      if (subpath.empty() && query_string.empty()) {
        GoogleUrl original_gurl(url);
        if (original_gurl.IsWebValid()) {
          GoogleString redirect_url =
              StrCat(original_gurl.Origin(), admin_prefix, "/");
          IHttpResponse* response = context->GetResponse();
          response->Clear();
          response->SetStatus(301, "Moved Permanently");
          response->SetHeader(
              "Location", redirect_url.c_str(),
              static_cast<USHORT>(redirect_url.size()), TRUE);
          GoogleString ct("text/html");
          response->SetHeader(
              "Content-Type", ct.c_str(),
              static_cast<USHORT>(ct.size()), TRUE);
          GoogleString body = StrCat(
              "<html><body>Moved to <a href=\"", redirect_url, "\">",
              redirect_url, "</a></body></html>");
          char length_buf[32];
          snprintf(length_buf, sizeof(length_buf), "%zu", body.size());
          response->SetHeader(
              "Content-Length", length_buf,
              static_cast<USHORT>(strlen(length_buf)), TRUE);
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
          return true;
        }
      }
    }

    // Create a RequestContext for the admin fetch.
    RequestContextPtr request_ctx(new RequestContext(
        server_context->global_options()->ComputeHttpOptions(),
        server_context->thread_system()->NewMutex(),
        server_context->timer()));

    // Create the admin fetch adapter.
    IisAdminFetch admin_fetch(request_ctx);

    // Parse the (possibly rewritten) URL for AdminSite dispatch.
    GoogleUrl gurl(effective_url);

    if (IsStatisticsPath(path)) {
      // Delegate to SystemServerContext::StatisticsPage().
      QueryParams query_params;
      if (gurl.IsWebValid()) {
        query_params.ParseFromUrl(gurl);
      }
      server_context->StatisticsPage(
          is_global, query_params,
          server_context->global_options(), &admin_fetch);
    } else {
      // Delegate to SystemServerContext::AdminPage().
      QueryParams query_params;
      if (gurl.IsWebValid()) {
        query_params.ParseFromUrl(gurl);
      }
      // Read POST body for license API endpoints.
      GoogleString request_body;
      IHttpRequest* admin_request = context->GetRequest();
      if (admin_request != nullptr) {
        HTTP_REQUEST* raw_request = admin_request->GetRawHttpRequest();
        if (raw_request != nullptr && raw_request->Verb == HttpVerbPOST) {
          // Read entity body in chunks.
          static const DWORD kReadBufferSize = 4096;
          char read_buffer[kReadBufferSize];
          DWORD bytes_read = 0;
          HRESULT hr = S_OK;
          while (SUCCEEDED(hr)) {
            hr = admin_request->ReadEntityBody(read_buffer, kReadBufferSize,
                                               FALSE, &bytes_read, NULL);
            if (SUCCEEDED(hr) && bytes_read > 0) {
              request_body.append(read_buffer, bytes_read);
            }
            if (bytes_read == 0) break;
            // Safety limit
            if (request_body.size() > 8192) {
              SendErrorResponse(context, 413, "Request body too large");
              return true;
            }
          }
        }
      }
      server_context->AdminPage(
          is_global, gurl, query_params,
          server_context->global_options(), &admin_fetch, request_body);
    }

    // Wait for the admin page generation to complete.
    admin_fetch.WaitForCompletion();

    // Extract response headers set by AdminSite.
    const ResponseHeaders* resp_headers = admin_fetch.response_headers();
    int status_code = resp_headers->status_code();
    if (status_code == 0) {
      status_code = 200;
    }

    // Get content type from response headers, fall back to text/html.
    GoogleString content_type;
    const char* ct = resp_headers->Lookup1(HttpAttributes::kContentType);
    if (ct != nullptr) {
      content_type = ct;
    } else {
      content_type = "text/html; charset=utf-8";
    }

    // Check for redirect from AdminSite (e.g., other 301 cases).
    if (status_code == HttpStatus::kMovedPermanently) {
      const char* location =
          resp_headers->Lookup1(HttpAttributes::kLocation);
      if (location != nullptr) {
        IHttpResponse* response = context->GetResponse();
        response->Clear();
        response->SetStatus(301, "Moved Permanently");
        GoogleString loc_str(location);
        response->SetHeader("Location", loc_str.c_str(),
                            static_cast<USHORT>(loc_str.size()), TRUE);
        GoogleString ct_val("text/html");
        response->SetHeader("Content-Type", ct_val.c_str(),
                            static_cast<USHORT>(ct_val.size()), TRUE);
        const GoogleString& body = admin_fetch.buffer();
        char length_buf[32];
        snprintf(length_buf, sizeof(length_buf), "%zu", body.size());
        response->SetHeader("Content-Length", length_buf,
                            static_cast<USHORT>(strlen(length_buf)), TRUE);
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
        return true;
      }
    }

    SendResponse(context, status_code, content_type, admin_fetch.buffer());
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

  // If authentication is not required, allow access from localhost.
  if (!config->admin_auth_required()) {
    GoogleString client_ip = GetClientIp(context, server_context);
    if (IsIpAllowed(client_ip, server_context)) {
      return true;
    }
  }

  // Check if request is from an allowed IP.
  GoogleString client_ip = GetClientIp(context, server_context);
  if (IsIpAllowed(client_ip, server_context)) {
    return true;
  }

  // Check for authentication token.
  return ValidateToken(context, server_context);
}

bool IisAdminHandler::IsAdminPath(const GoogleString& path) {
  GoogleString base_path = path;
  size_t query_pos = base_path.find('?');
  if (query_pos != GoogleString::npos) {
    base_path = base_path.substr(0, query_pos);
  }
  return base_path == "/pagespeed_admin" ||
         StringPiece(base_path).starts_with("/pagespeed_admin/");
}

bool IisAdminHandler::IsGlobalAdminPath(const GoogleString& path) {
  GoogleString base_path = path;
  size_t query_pos = base_path.find('?');
  if (query_pos != GoogleString::npos) {
    base_path = base_path.substr(0, query_pos);
  }
  return base_path == "/pagespeed_global_admin" ||
         StringPiece(base_path).starts_with("/pagespeed_global_admin/");
}

bool IisAdminHandler::IsStatisticsPath(const GoogleString& path) {
  return path == "/pagespeed_statistics" ||
         path == "/mod_pagespeed_statistics";
}

void IisAdminHandler::GenerateHealthPage(IisServerContext* server_context,
                                         GoogleString* output,
                                         GoogleString* content_type) {
  *content_type = "text/plain; charset=utf-8";
  bool is_healthy = (server_context != nullptr &&
                     server_context->global_options() != nullptr);
  if (is_healthy) {
    *output = "healthy: ok";
  } else {
    *output = "error: server context not initialized";
  }
}

void IisAdminHandler::SendResponse(IHttpContext* context,
                                   int status_code,
                                   const GoogleString& content_type,
                                   const GoogleString& body) {
  IHttpResponse* response = context->GetResponse();

  response->Clear();
  response->SetStatus(static_cast<USHORT>(status_code),
                      GetReasonPhrase(status_code));

  response->SetHeader("Content-Type",
                      content_type.c_str(),
                      static_cast<USHORT>(content_type.size()),
                      TRUE);

  char length_buf[32];
  snprintf(length_buf, sizeof(length_buf), "%zu", body.size());
  response->SetHeader("Content-Length",
                      length_buf,
                      static_cast<USHORT>(strlen(length_buf)),
                      TRUE);

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
  server_context->FlushCacheIfNecessary();
  LOG(INFO) << "PageSpeed: Cache flushed via admin request";
  GoogleString response = "{\"status\": \"ok\", \"message\": \"Cache flushed\"}";
  SendResponse(context, 200, "application/json", response);
  return true;
}

bool IisAdminHandler::HandleCachePurge(IHttpContext* context,
                                       IisServerContext* server_context,
                                       const GoogleString& url) {
  server_context->FlushCacheIfNecessary();
  LOG(INFO) << "PageSpeed: Cache purged for URL: " << url;
  GoogleString response = StrCat(
      "{\"status\": \"ok\", \"message\": \"URL purged: ",
      JsonEscape(url), "\"}");
  SendResponse(context, 200, "application/json", response);
  return true;
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

  const IisConfig* config = server_context != nullptr ?
      server_context->config() : nullptr;
  if (config != nullptr && config->IsTrustedProxy(direct_ip)) {
    USHORT forwarded_for_len = 0;
    PCSTR forwarded_for = request->GetHeader("X-Forwarded-For",
                                             &forwarded_for_len);
    if (forwarded_for != nullptr && forwarded_for_len > 0) {
      GoogleString ip(forwarded_for, forwarded_for_len);
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
  const IisConfig* config = server_context->config();
  if (config != nullptr) {
    return config->IsIpAllowed(client_ip);
  }

  if (client_ip == "127.0.0.1" || client_ip == "::1" ||
      client_ip == "localhost") {
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

  USHORT token_len = 0;
  PCSTR token = request->GetHeader("X-PageSpeed-Admin-Token", &token_len);

  if (token == nullptr || token_len == 0) {
    return false;
  }

  const IisConfig* config = server_context->config();
  if (config != nullptr && !config->admin_token().empty()) {
    GoogleString provided_token(token, token_len);
    return ConstantTimeCompare(provided_token, config->admin_token());
  }

  return false;
}

}  // namespace net_instaweb
