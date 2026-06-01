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

#include "test/pagespeed/iis/mock_iis.h"

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// MockHttpRequest implementation
MockHttpRequest::MockHttpRequest()
    : method_("GET"),
      remote_address_("127.0.0.1") {
}

MockHttpRequest::~MockHttpRequest() {
}

void MockHttpRequest::SetUrl(const GoogleString& url) {
  url_ = url;
  // Extract query string if present
  size_t pos = url.find('?');
  if (pos != GoogleString::npos) {
    query_string_ = url.substr(pos + 1);
  }
}

void MockHttpRequest::SetMethod(const GoogleString& method) {
  method_ = method;
}

void MockHttpRequest::SetHeader(const GoogleString& name,
                                 const GoogleString& value) {
  headers_[LowerString(name)] = value;
}

void MockHttpRequest::SetQueryString(const GoogleString& query) {
  query_string_ = query;
}

void MockHttpRequest::SetRemoteAddress(const GoogleString& addr) {
  remote_address_ = addr;
}

GoogleString MockHttpRequest::GetHeader(const GoogleString& name) const {
  auto it = headers_.find(LowerString(name));
  if (it != headers_.end()) {
    return it->second;
  }
  return "";
}

// MockHttpResponse implementation
MockHttpResponse::MockHttpResponse()
    : status_(200),
      status_reason_("OK"),
      completed_(false) {
}

MockHttpResponse::~MockHttpResponse() {
}

void MockHttpResponse::SetStatus(USHORT status, const GoogleString& reason) {
  status_ = status;
  status_reason_ = reason;
}

void MockHttpResponse::SetHeader(const GoogleString& name,
                                  const GoogleString& value) {
  headers_[name] = value;
  response_headers_.Replace(name, value);
}

void MockHttpResponse::AppendBody(const StringPiece& data) {
  data.AppendToString(&body_);
}

void MockHttpResponse::ClearBody() {
  body_.clear();
}

GoogleString MockHttpResponse::GetHeader(const GoogleString& name) const {
  auto it = headers_.find(name);
  if (it != headers_.end()) {
    return it->second;
  }
  return "";
}

HRESULT MockHttpResponse::WriteEntityChunks(const char* data, size_t length) {
  body_.append(data, length);
  return S_OK;
}

// MockHttpContext implementation
MockHttpContext::MockHttpContext()
    : site_id_(1),
      site_name_("Default Web Site"),
      app_path_("/"),
      physical_path_("C:\\inetpub\\wwwroot"),
      async_pending_(false),
      async_result_(RQ_NOTIFICATION_CONTINUE) {
}

MockHttpContext::~MockHttpContext() {
}

void MockHttpContext::CompleteAsync(REQUEST_NOTIFICATION_STATUS status) {
  async_pending_ = false;
  async_result_ = status;
}

// MockAppHostElement implementation
MockAppHostElement::MockAppHostElement() {
}

MockAppHostElement::MockAppHostElement(const GoogleString& name)
    : name_(name) {
}

MockAppHostElement::~MockAppHostElement() {
  for (auto* child : children_) {
    delete child;
  }
}

void MockAppHostElement::SetAttribute(const GoogleString& name,
                                       const GoogleString& value) {
  attributes_[name] = value;
}

GoogleString MockAppHostElement::GetAttribute(const GoogleString& name) const {
  auto it = attributes_.find(name);
  if (it != attributes_.end()) {
    return it->second;
  }
  return "";
}

bool MockAppHostElement::HasAttribute(const GoogleString& name) const {
  return attributes_.find(name) != attributes_.end();
}

void MockAppHostElement::AddChildElement(MockAppHostElement* child) {
  children_.push_back(child);
}

MockAppHostElement* MockAppHostElement::GetChildElement(
    const GoogleString& name) const {
  for (auto* child : children_) {
    if (child->GetName() == name) {
      return child;
    }
  }
  return nullptr;
}

// MockAppHostAdminManager implementation
MockAppHostAdminManager::MockAppHostAdminManager() {
}

MockAppHostAdminManager::~MockAppHostAdminManager() {
  for (auto& pair : sections_) {
    delete pair.second;
  }
}

void MockAppHostAdminManager::AddSection(const GoogleString& section_path,
                                          MockAppHostElement* element) {
  sections_[section_path] = element;
}

MockAppHostElement* MockAppHostAdminManager::GetSection(
    const GoogleString& section_path) const {
  auto it = sections_.find(section_path);
  if (it != sections_.end()) {
    return it->second;
  }
  return nullptr;
}

HRESULT MockAppHostAdminManager::GetAdminSection(
    const GoogleString& section_name,
    const GoogleString& config_path,
    MockAppHostElement** element) {
  GoogleString full_path = StrCat(config_path, "/", section_name);
  *element = GetSection(full_path);
  if (*element != nullptr) {
    return S_OK;
  }
  // Try just section name
  *element = GetSection(section_name);
  return (*element != nullptr) ? S_OK : E_FAIL;
}

// MockHttpContextBuilder implementation
MockHttpContextBuilder::MockHttpContextBuilder()
    : context_(new MockHttpContext()) {
}

MockHttpContextBuilder& MockHttpContextBuilder::SetUrl(const GoogleString& url) {
  context_->request()->SetUrl(url);
  return *this;
}

MockHttpContextBuilder& MockHttpContextBuilder::SetMethod(
    const GoogleString& method) {
  context_->request()->SetMethod(method);
  return *this;
}

MockHttpContextBuilder& MockHttpContextBuilder::AddRequestHeader(
    const GoogleString& name, const GoogleString& value) {
  context_->request()->SetHeader(name, value);
  return *this;
}

MockHttpContextBuilder& MockHttpContextBuilder::SetSiteId(DWORD site_id) {
  context_->SetSiteId(site_id);
  return *this;
}

MockHttpContextBuilder& MockHttpContextBuilder::SetSiteName(
    const GoogleString& name) {
  context_->SetSiteName(name);
  return *this;
}

MockHttpContextBuilder& MockHttpContextBuilder::SetPhysicalPath(
    const GoogleString& path) {
  context_->SetPhysicalPath(path);
  return *this;
}

std::unique_ptr<MockHttpContext> MockHttpContextBuilder::Build() {
  return std::move(context_);
}

// MockConfigBuilder implementation
MockConfigBuilder::MockConfigBuilder()
    : root_(new MockAppHostElement("pagespeed")) {
  // Set up default structure
  root_->AddChildElement(new MockAppHostElement("settings"));
}

MockConfigBuilder& MockConfigBuilder::SetEnabled(bool enabled) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    settings->SetAttribute("enabled", enabled ? "true" : "false");
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetFileCachePath(const GoogleString& path) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    settings->SetAttribute("fileCachePath", path);
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetLruCacheSize(int size_bytes) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    auto* cache = settings->GetChildElement("cache");
    if (!cache) {
      cache = new MockAppHostElement("cache");
      settings->AddChildElement(cache);
    }
    cache->SetAttribute("lruCacheSizeBytes", IntegerToString(size_bytes));
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::AddEnabledFilter(
    const GoogleString& filter) {
  enabled_filters_.push_back(filter);
  return *this;
}

MockConfigBuilder& MockConfigBuilder::AddDisabledFilter(
    const GoogleString& filter) {
  disabled_filters_.push_back(filter);
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetImageQuality(int quality) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    auto* images = settings->GetChildElement("images");
    if (!images) {
      images = new MockAppHostElement("images");
      settings->AddChildElement(images);
    }
    images->SetAttribute("recompressQuality", IntegerToString(quality));
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetWebpQuality(int quality) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    auto* images = settings->GetChildElement("images");
    if (!images) {
      images = new MockAppHostElement("images");
      settings->AddChildElement(images);
    }
    images->SetAttribute("webpQuality", IntegerToString(quality));
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetAdminEnabled(bool enabled) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    auto* admin = settings->GetChildElement("admin");
    if (!admin) {
      admin = new MockAppHostElement("admin");
      settings->AddChildElement(admin);
    }
    admin->SetAttribute("enabled", enabled ? "true" : "false");
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetAdminPath(const GoogleString& path) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    auto* admin = settings->GetChildElement("admin");
    if (!admin) {
      admin = new MockAppHostElement("admin");
      settings->AddChildElement(admin);
    }
    admin->SetAttribute("path", path);
  }
  return *this;
}

MockConfigBuilder& MockConfigBuilder::SetLicenseKey(const GoogleString& key) {
  auto* settings = root_->GetChildElement("settings");
  if (settings) {
    settings->SetAttribute("licenseKey", key);
  }
  return *this;
}

std::unique_ptr<MockAppHostElement> MockConfigBuilder::Build() {
  // Apply enabled/disabled filters
  auto* settings = root_->GetChildElement("settings");
  if (settings && (!enabled_filters_.empty() || !disabled_filters_.empty())) {
    auto* filters = settings->GetChildElement("filters");
    if (!filters) {
      filters = new MockAppHostElement("filters");
      settings->AddChildElement(filters);
    }
    if (!enabled_filters_.empty()) {
      filters->SetAttribute("enabledFilters", JoinString(enabled_filters_, ","));
    }
    if (!disabled_filters_.empty()) {
      filters->SetAttribute("disabledFilters", JoinString(disabled_filters_, ","));
    }
  }
  return std::move(root_);
}

}  // namespace net_instaweb
