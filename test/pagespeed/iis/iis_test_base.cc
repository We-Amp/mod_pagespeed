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

#include <memory>

#include "test/pagespeed/iis/iis_test_base.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "test/pagespeed/kernel/thread/mock_scheduler.h"
#include "pagespeed/kernel/util/platform.h"

namespace net_instaweb {

IisTestBase::IisTestBase() {
}

IisTestBase::~IisTestBase() {
}

void IisTestBase::SetUp() {
  // Initialize RewriteOptions properties before any test uses them
  RewriteOptions::Initialize();
  thread_system_.reset(Platform::CreateThreadSystem());
  message_handler_.reset(new MockMessageHandler(new NullMutex));
  timer_.reset(new MockTimer(thread_system_->NewMutex(), 0));
  scheduler_.reset(new MockScheduler(thread_system_.get(), timer_.get()));
  hasher_.reset(new MockHasher);
  statistics_.reset(new NullStatistics);
}

void IisTestBase::TearDown() {
  scheduler_.reset();
  timer_.reset();
  message_handler_.reset();
  thread_system_.reset();
  hasher_.reset();
  statistics_.reset();
}

std::unique_ptr<MockHttpContext> IisTestBase::CreateContext(
    const GoogleString& url) {
  return MockHttpContextBuilder()
      .SetUrl(url)
      .SetMethod("GET")
      .Build();
}

std::unique_ptr<MockHttpContext> IisTestBase::CreateContext(
    const GoogleString& url,
    const GoogleString& method,
    const std::map<GoogleString, GoogleString>& headers) {
  MockHttpContextBuilder builder;
  builder.SetUrl(url).SetMethod(method);
  for (const auto& header : headers) {
    builder.AddRequestHeader(header.first, header.second);
  }
  return builder.Build();
}

std::unique_ptr<MockAppHostElement> IisTestBase::CreateConfig() {
  return config_builder_.Build();
}

void IisTestBase::AdvanceTimeMs(int64 ms) {
  timer_->AdvanceMs(ms);
  scheduler_->AwaitQuiescence();
}

bool IisTestBase::IsAjaxHeaders(
    const std::map<GoogleString, GoogleString>& headers) {
  // Check X-Requested-With: XMLHttpRequest (case-insensitive value match).
  // Note: MockHttpRequest stores header names in lowercase.
  auto it = headers.find("x-requested-with");
  if (it != headers.end() && !it->second.empty()) {
    if (StringCaseEqual(it->second, "XMLHttpRequest")) {
      return true;
    }
  }

  // Check X-MicrosoftAjax (ASP.NET UpdatePanel/ScriptManager) - any non-empty value.
  it = headers.find("x-microsoftajax");
  if (it != headers.end() && !it->second.empty()) {
    return true;
  }

  // Check X-Prototype-Version (Prototype.js) - any non-empty value.
  it = headers.find("x-prototype-version");
  if (it != headers.end() && !it->second.empty()) {
    return true;
  }

  return false;
}

bool IisTestBase::IsRangeRequest(
    const std::map<GoogleString, GoogleString>& headers) {
  // Check Range header.
  auto it = headers.find("range");
  if (it != headers.end() && !it->second.empty()) {
    return true;
  }

  // Check If-Range header.
  it = headers.find("if-range");
  if (it != headers.end() && !it->second.empty()) {
    return true;
  }

  return false;
}

GoogleString IisTestBase::NormalizeUrlPort(const GoogleString& url) {
  if (StringCaseStartsWith(url, "http://")) {
    size_t host_start = 7;  // Length of "http://"
    size_t colon_pos = url.find(':', host_start);
    if (colon_pos != GoogleString::npos) {
      size_t port_end = url.find('/', colon_pos);
      if (port_end == GoogleString::npos) port_end = url.find('?', colon_pos);
      if (port_end == GoogleString::npos) port_end = url.size();
      GoogleString port = url.substr(colon_pos + 1, port_end - colon_pos - 1);
      if (port == "80") {
        return url.substr(0, colon_pos) + url.substr(port_end);
      }
    }
  } else if (StringCaseStartsWith(url, "https://")) {
    size_t host_start = 8;  // Length of "https://"
    size_t colon_pos = url.find(':', host_start);
    if (colon_pos != GoogleString::npos) {
      size_t port_end = url.find('/', colon_pos);
      if (port_end == GoogleString::npos) port_end = url.find('?', colon_pos);
      if (port_end == GoogleString::npos) port_end = url.size();
      GoogleString port = url.substr(colon_pos + 1, port_end - colon_pos - 1);
      if (port == "443") {
        return url.substr(0, colon_pos) + url.substr(port_end);
      }
    }
  }
  return url;
}

bool IisTestBase::IsPageSpeedUrl(const GoogleString& url) {
  return url.find(".pagespeed.") != GoogleString::npos;
}

GoogleString IisTestBase::CreatePageSpeedUrl(
    const GoogleString& original_url,
    const GoogleString& filter_id,
    const GoogleString& hash) {
  // Format: /path/to/file.FILTER_ID.HASH.EXT
  // Example: /styles/main.cf.ABC123.css
  size_t dot_pos = original_url.rfind('.');
  if (dot_pos == GoogleString::npos) {
    return StrCat(original_url, ".pagespeed.", filter_id, ".", hash);
  }

  GoogleString base = original_url.substr(0, dot_pos);
  GoogleString ext = original_url.substr(dot_pos);
  return StrCat(base, ".pagespeed.", filter_id, ".", hash, ext);
}

bool IisTestBase::DecodePageSpeedUrl(
    const GoogleString& pagespeed_url,
    GoogleString* original_url,
    GoogleString* filter_id,
    GoogleString* hash) {
  // Find .pagespeed. marker
  size_t marker_pos = pagespeed_url.find(".pagespeed.");
  if (marker_pos == GoogleString::npos) {
    return false;
  }

  // Get base path before .pagespeed.
  GoogleString base = pagespeed_url.substr(0, marker_pos);

  // Get the rest after .pagespeed.
  GoogleString rest = pagespeed_url.substr(marker_pos + 11);  // len(".pagespeed.")

  // Parse FILTER_ID.HASH.EXT
  std::vector<StringPiece> parts;
  SplitStringPieceToVector(rest, ".", &parts, false);

  if (parts.size() < 2) {
    return false;
  }

  *filter_id = parts[0].as_string();
  *hash = parts[1].as_string();

  // Reconstruct original URL
  if (parts.size() > 2) {
    // Has extension
    *original_url = StrCat(base, ".", parts[2].as_string());
  } else {
    *original_url = base;
  }

  return true;
}

const char* IisTestBase::GetReasonPhrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

void IisTestBase::InitStandardResponseHeaders(ResponseHeaders* headers) {
  headers->set_status_code(200);
  headers->set_major_version(1);
  headers->set_minor_version(1);
  headers->MergeContentType("text/plain");
}

void IisTestBase::OutputResponseHeaders(const ResponseHeaders& headers,
                                        MockHttpResponse* response) {
  response->SetStatus(headers.status_code(),
                      GetReasonPhrase(headers.status_code()));

  for (int i = 0; i < headers.NumAttributes(); ++i) {
    const GoogleString& name = headers.Name(i);
    const GoogleString& value = headers.Value(i);
    // Skip headers that IIS handles (hop-by-hop headers)
    if (name != HttpAttributes::kTransferEncoding &&
        name != HttpAttributes::kContentLength) {
      response->SetHeader(name, value);
    }
  }
}

bool IisTestBase::WriteToResponse(MockHttpResponse* response, StringPiece data) {
  response->WriteEntityChunks(data.data(), data.size());
  return true;
}

bool IisTestBase::FlushResponse(MockHttpResponse* response, bool final_flush) {
  response->Flush(final_flush);
  return true;
}

}  // namespace net_instaweb
