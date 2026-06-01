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

#ifndef PAGESPEED_IIS_MOCK_IIS_H_
#define PAGESPEED_IIS_MOCK_IIS_H_

// Mock IIS infrastructure for unit testing.
// This provides mock implementations of IIS interfaces that can be used
// to test IIS module code without requiring actual IIS.

#include <map>
#include <string>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"

#ifdef _WIN32
#include <httpserv.h>
#else
// Define minimal IIS types for cross-platform compilation of tests
// These allow the test code to compile on Linux for development

typedef unsigned short USHORT;
typedef unsigned long ULONG;
typedef unsigned long DWORD;
typedef long HRESULT;
typedef int BOOL;
typedef const char* PCSTR;
typedef const wchar_t* PCWSTR;
typedef wchar_t* BSTR;

#define S_OK 0
#define E_FAIL -1
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)
#define TRUE 1
#define FALSE 0

// Minimal HTTP structures
struct HTTP_REQUEST {
  USHORT Method;
  PCSTR pRawUrl;
  USHORT RawUrlLength;
};

struct HTTP_RESPONSE {
  USHORT StatusCode;
  PCSTR pReason;
  USHORT ReasonLength;
};

struct HTTP_DATA_CHUNK {
  enum { HttpDataChunkFromMemory } DataChunkType;
  struct {
    void* pBuffer;
    ULONG BufferLength;
  } FromMemory;
};

// Forward declarations for mock implementations
class IHttpRequest;
class IHttpResponse;
class IHttpContext;
class IAppHostElement;
class IAppHostAdminManager;

// Request notification status
enum REQUEST_NOTIFICATION_STATUS {
  RQ_NOTIFICATION_CONTINUE = 0,
  RQ_NOTIFICATION_PENDING = 1,
  RQ_NOTIFICATION_FINISH_REQUEST = 2
};

#endif  // _WIN32

namespace net_instaweb {

// Mock HTTP Request for testing
class MockHttpRequest {
 public:
  MockHttpRequest();
  ~MockHttpRequest();

  // Set request properties
  void SetUrl(const GoogleString& url);
  void SetMethod(const GoogleString& method);
  void SetHeader(const GoogleString& name, const GoogleString& value);
  void SetQueryString(const GoogleString& query);
  void SetRemoteAddress(const GoogleString& addr);

  // IIS-like interface methods
  PCSTR GetRawHttpRequestUrl() const { return url_.c_str(); }
  USHORT GetRawHttpRequestUrlLength() const { return url_.length(); }
  const GoogleString& GetMethod() const { return method_; }

  // Get header value (returns empty string if not found)
  GoogleString GetHeader(const GoogleString& name) const;

  // Get all headers
  const std::map<GoogleString, GoogleString>& headers() const { return headers_; }

  // Query string
  const GoogleString& query_string() const { return query_string_; }

  // Remote address
  const GoogleString& remote_address() const { return remote_address_; }

 private:
  GoogleString url_;
  GoogleString method_;
  GoogleString query_string_;
  GoogleString remote_address_;
  std::map<GoogleString, GoogleString> headers_;

  DISALLOW_COPY_AND_ASSIGN(MockHttpRequest);
};

// Mock HTTP Response for testing
class MockHttpResponse {
 public:
  MockHttpResponse();
  ~MockHttpResponse();

  // Set response properties
  void SetStatus(USHORT status, const GoogleString& reason);
  void SetHeader(const GoogleString& name, const GoogleString& value);
  void AppendBody(const StringPiece& data);
  void ClearBody();

  // IIS-like interface methods
  USHORT GetStatus() const { return status_; }
  const GoogleString& GetStatusReason() const { return status_reason_; }

  // Get header value
  GoogleString GetHeader(const GoogleString& name) const;

  // Get all headers
  const std::map<GoogleString, GoogleString>& headers() const { return headers_; }

  // Get accumulated response body
  const GoogleString& body() const { return body_; }

  // Response headers object
  ResponseHeaders* mutable_response_headers() { return &response_headers_; }
  const ResponseHeaders& response_headers() const { return response_headers_; }

  // Check if response has been completed
  bool completed() const { return completed_; }
  void set_completed(bool completed) { completed_ = completed; }

  // Simulate IIS WriteEntityChunks
  HRESULT WriteEntityChunks(const char* data, size_t length);

 private:
  USHORT status_;
  GoogleString status_reason_;
  std::map<GoogleString, GoogleString> headers_;
  GoogleString body_;
  ResponseHeaders response_headers_;
  bool completed_;

  DISALLOW_COPY_AND_ASSIGN(MockHttpResponse);
};

// Mock HTTP Context for testing
class MockHttpContext {
 public:
  MockHttpContext();
  ~MockHttpContext();

  // Access request and response
  MockHttpRequest* request() { return &request_; }
  MockHttpResponse* response() { return &response_; }
  const MockHttpRequest* request() const { return &request_; }
  const MockHttpResponse* response() const { return &response_; }

  // Site and application info
  void SetSiteId(DWORD site_id) { site_id_ = site_id; }
  DWORD GetSiteId() const { return site_id_; }

  void SetSiteName(const GoogleString& name) { site_name_ = name; }
  const GoogleString& GetSiteName() const { return site_name_; }

  void SetApplicationPath(const GoogleString& path) { app_path_ = path; }
  const GoogleString& GetApplicationPath() const { return app_path_; }

  void SetPhysicalPath(const GoogleString& path) { physical_path_ = path; }
  const GoogleString& GetPhysicalPath() const { return physical_path_; }

  // User context
  void SetAuthenticatedUser(const GoogleString& user) { auth_user_ = user; }
  const GoogleString& GetAuthenticatedUser() const { return auth_user_; }
  bool IsAuthenticated() const { return !auth_user_.empty(); }

  // Simulate async completion
  void SetAsyncPending(bool pending) { async_pending_ = pending; }
  bool IsAsyncPending() const { return async_pending_; }

  void CompleteAsync(REQUEST_NOTIFICATION_STATUS status);
  REQUEST_NOTIFICATION_STATUS GetAsyncResult() const { return async_result_; }

 private:
  MockHttpRequest request_;
  MockHttpResponse response_;
  DWORD site_id_;
  GoogleString site_name_;
  GoogleString app_path_;
  GoogleString physical_path_;
  GoogleString auth_user_;
  bool async_pending_;
  REQUEST_NOTIFICATION_STATUS async_result_;

  DISALLOW_COPY_AND_ASSIGN(MockHttpContext);
};

// Mock App Host Element for config testing
class MockAppHostElement {
 public:
  MockAppHostElement();
  explicit MockAppHostElement(const GoogleString& name);
  ~MockAppHostElement();

  // Set element properties
  void SetName(const GoogleString& name) { name_ = name; }
  const GoogleString& GetName() const { return name_; }

  // Attributes
  void SetAttribute(const GoogleString& name, const GoogleString& value);
  GoogleString GetAttribute(const GoogleString& name) const;
  bool HasAttribute(const GoogleString& name) const;

  // Child elements
  void AddChildElement(MockAppHostElement* child);
  MockAppHostElement* GetChildElement(const GoogleString& name) const;
  const std::vector<MockAppHostElement*>& GetChildElements() const { return children_; }

 private:
  GoogleString name_;
  std::map<GoogleString, GoogleString> attributes_;
  std::vector<MockAppHostElement*> children_;

  DISALLOW_COPY_AND_ASSIGN(MockAppHostElement);
};

// Mock App Host Admin Manager for config testing
class MockAppHostAdminManager {
 public:
  MockAppHostAdminManager();
  ~MockAppHostAdminManager();

  // Add a configuration section
  void AddSection(const GoogleString& section_path, MockAppHostElement* element);

  // Get a configuration section (returns nullptr if not found)
  MockAppHostElement* GetSection(const GoogleString& section_path) const;

  // Simulate IIS GetAdminSection
  HRESULT GetAdminSection(const GoogleString& section_name,
                          const GoogleString& config_path,
                          MockAppHostElement** element);

 private:
  std::map<GoogleString, MockAppHostElement*> sections_;

  DISALLOW_COPY_AND_ASSIGN(MockAppHostAdminManager);
};

// Helper to create a mock context with common settings
class MockHttpContextBuilder {
 public:
  MockHttpContextBuilder();

  MockHttpContextBuilder& SetUrl(const GoogleString& url);
  MockHttpContextBuilder& SetMethod(const GoogleString& method);
  MockHttpContextBuilder& AddRequestHeader(const GoogleString& name,
                                           const GoogleString& value);
  MockHttpContextBuilder& SetSiteId(DWORD site_id);
  MockHttpContextBuilder& SetSiteName(const GoogleString& name);
  MockHttpContextBuilder& SetPhysicalPath(const GoogleString& path);

  std::unique_ptr<MockHttpContext> Build();

 private:
  std::unique_ptr<MockHttpContext> context_;
};

// Helper to create mock config elements
class MockConfigBuilder {
 public:
  MockConfigBuilder();

  // Build a pagespeed config section
  MockConfigBuilder& SetEnabled(bool enabled);
  MockConfigBuilder& SetFileCachePath(const GoogleString& path);
  MockConfigBuilder& SetLruCacheSize(int size_bytes);
  MockConfigBuilder& AddEnabledFilter(const GoogleString& filter);
  MockConfigBuilder& AddDisabledFilter(const GoogleString& filter);
  MockConfigBuilder& SetImageQuality(int quality);
  MockConfigBuilder& SetWebpQuality(int quality);
  MockConfigBuilder& SetAdminEnabled(bool enabled);
  MockConfigBuilder& SetAdminPath(const GoogleString& path);
  MockConfigBuilder& SetLicenseKey(const GoogleString& key);

  std::unique_ptr<MockAppHostElement> Build();

 private:
  std::unique_ptr<MockAppHostElement> root_;
  std::vector<GoogleString> enabled_filters_;
  std::vector<GoogleString> disabled_filters_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_MOCK_IIS_H_
