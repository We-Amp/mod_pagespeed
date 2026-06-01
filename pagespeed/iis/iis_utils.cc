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

#include "pagespeed/iis/iis_utils.h"

#ifdef _WIN32

#include <cstring>

namespace net_instaweb {
namespace iis_utils {

// =============================================================================
// URL and Path Extraction
// =============================================================================

GoogleString ExtractUrl(IHttpContext* context) {
  if (context == nullptr) {
    return GoogleString();
  }

  IHttpRequest* request = context->GetRequest();
  if (request == nullptr) {
    return GoogleString();
  }

  HTTP_REQUEST* raw = request->GetRawHttpRequest();
  if (raw == nullptr || raw->CookedUrl.pFullUrl == nullptr) {
    return GoogleString();
  }

  // Convert wide string to UTF-8
  int len = WideCharToMultiByte(CP_UTF8, 0,
                                raw->CookedUrl.pFullUrl,
                                raw->CookedUrl.FullUrlLength / sizeof(WCHAR),
                                nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return GoogleString();
  }

  GoogleString url(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0,
                      raw->CookedUrl.pFullUrl,
                      raw->CookedUrl.FullUrlLength / sizeof(WCHAR),
                      &url[0], len, nullptr, nullptr);
  return url;
}

GoogleString ExtractPath(IHttpContext* context) {
  if (context == nullptr) {
    return GoogleString();
  }

  IHttpRequest* request = context->GetRequest();
  if (request == nullptr) {
    return GoogleString();
  }

  HTTP_REQUEST* raw = request->GetRawHttpRequest();
  if (raw == nullptr || raw->CookedUrl.pAbsPath == nullptr) {
    return GoogleString();
  }

  // Convert wide string to UTF-8
  int len = WideCharToMultiByte(CP_UTF8, 0,
                                raw->CookedUrl.pAbsPath,
                                raw->CookedUrl.AbsPathLength / sizeof(WCHAR),
                                nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return GoogleString();
  }

  GoogleString path(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0,
                      raw->CookedUrl.pAbsPath,
                      raw->CookedUrl.AbsPathLength / sizeof(WCHAR),
                      &path[0], len, nullptr, nullptr);
  return path;
}

GoogleString ExtractPathWithQuery(IHttpContext* context) {
  if (context == nullptr) {
    return GoogleString();
  }

  IHttpRequest* request = context->GetRequest();
  if (request == nullptr) {
    return GoogleString();
  }

  HTTP_REQUEST* raw = request->GetRawHttpRequest();
  if (raw == nullptr) {
    return GoogleString();
  }

  // Use pAbsPath for the path and pQueryString for the query
  GoogleString result;

  if (raw->CookedUrl.pAbsPath != nullptr) {
    int path_len = WideCharToMultiByte(CP_UTF8, 0,
                                       raw->CookedUrl.pAbsPath,
                                       raw->CookedUrl.AbsPathLength / sizeof(WCHAR),
                                       nullptr, 0, nullptr, nullptr);
    if (path_len > 0) {
      result.resize(path_len);
      WideCharToMultiByte(CP_UTF8, 0,
                          raw->CookedUrl.pAbsPath,
                          raw->CookedUrl.AbsPathLength / sizeof(WCHAR),
                          &result[0], path_len, nullptr, nullptr);
    }
  }

  if (raw->CookedUrl.pQueryString != nullptr &&
      raw->CookedUrl.QueryStringLength > 0) {
    int query_len = WideCharToMultiByte(CP_UTF8, 0,
                                        raw->CookedUrl.pQueryString,
                                        raw->CookedUrl.QueryStringLength / sizeof(WCHAR),
                                        nullptr, 0, nullptr, nullptr);
    if (query_len > 0) {
      size_t current_len = result.size();
      result.resize(current_len + query_len);
      WideCharToMultiByte(CP_UTF8, 0,
                          raw->CookedUrl.pQueryString,
                          raw->CookedUrl.QueryStringLength / sizeof(WCHAR),
                          &result[current_len], query_len, nullptr, nullptr);
    }
  }

  return result;
}

// =============================================================================
// Response Helpers
// =============================================================================

bool SendSimpleResponse(IHttpContext* context,
                        int status,
                        const char* reason,
                        const GoogleString& body) {
  return SendSimpleResponse(context, status, reason,
                            "text/html; charset=utf-8", body);
}

bool SendSimpleResponse(IHttpContext* context,
                        int status,
                        const char* reason,
                        const GoogleString& content_type,
                        const GoogleString& body) {
  if (context == nullptr) {
    return false;
  }

  IHttpResponse* response = context->GetResponse();
  if (response == nullptr) {
    return false;
  }

  // Clear any existing response
  response->Clear();

  // Set status
  response->SetStatus(static_cast<USHORT>(status), reason);

  // Set Content-Type
  if (!SetResponseHeader(response, "Content-Type", content_type)) {
    return false;
  }

  // Set Content-Length
  char length_buf[32];
  snprintf(length_buf, sizeof(length_buf), "%zu", body.size());
  if (!SetResponseHeader(response, "Content-Length", length_buf)) {
    return false;
  }

  // Write body
  if (!body.empty()) {
    return WriteResponseBody(context, response, body);
  }

  return true;
}

bool SendJsonResponse(IHttpContext* context,
                      int status,
                      const GoogleString& json_body) {
  return SendSimpleResponse(context, status, GetReasonPhrase(status),
                            "application/json", json_body);
}

// =============================================================================
// Header Utilities
// =============================================================================

bool SetResponseHeader(IHttpResponse* response,
                       const char* name,
                       const GoogleString& value,
                       bool replace) {
  if (response == nullptr || name == nullptr) {
    return false;
  }

  HRESULT hr = response->SetHeader(name,
                                   value.c_str(),
                                   static_cast<USHORT>(value.size()),
                                   replace ? TRUE : FALSE);
  return SUCCEEDED(hr);
}

GoogleString GetRequestHeader(IHttpRequest* request, const char* name) {
  if (request == nullptr || name == nullptr) {
    return GoogleString();
  }

  USHORT len = 0;
  PCSTR value = request->GetHeader(name, &len);

  if (value == nullptr || len == 0) {
    return GoogleString();
  }

  return GoogleString(value, len);
}

GoogleString GetResponseHeader(IHttpResponse* response, const char* name) {
  if (response == nullptr || name == nullptr) {
    return GoogleString();
  }

  USHORT len = 0;
  PCSTR value = response->GetHeader(name, &len);

  if (value == nullptr || len == 0) {
    return GoogleString();
  }

  return GoogleString(value, len);
}

// =============================================================================
// Reason Phrase Utilities
// =============================================================================

const char* GetReasonPhrase(int status_code) {
  switch (status_code) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Unknown";
  }
}

// =============================================================================
// Memory Allocation Helpers
// =============================================================================

void* AllocateRequestMemory(IHttpContext* context, size_t size) {
  if (context == nullptr || size == 0) {
    return nullptr;
  }

  return context->AllocateRequestMemory(static_cast<DWORD>(size));
}

bool WriteResponseBody(IHttpContext* context,
                       IHttpResponse* response,
                       const GoogleString& body) {
  if (context == nullptr || response == nullptr || body.empty()) {
    return body.empty();  // Empty body is considered success
  }

  // Allocate buffer from IIS memory pool to ensure lifetime
  // matches request lifetime (survives async completion)
  void* response_buffer = AllocateRequestMemory(context, body.size());
  if (response_buffer == nullptr) {
    return false;
  }
  memcpy(response_buffer, body.data(), body.size());

  HTTP_DATA_CHUNK chunk;
  chunk.DataChunkType = HttpDataChunkFromMemory;
  chunk.FromMemory.pBuffer = response_buffer;
  chunk.FromMemory.BufferLength = static_cast<ULONG>(body.size());

  DWORD bytes_sent = 0;
  BOOL completion_expected = FALSE;
  HRESULT hr = response->WriteEntityChunks(&chunk, 1, FALSE, FALSE,
                                           &bytes_sent, &completion_expected);
  return SUCCEEDED(hr);
}

}  // namespace iis_utils
}  // namespace net_instaweb

#endif  // _WIN32
