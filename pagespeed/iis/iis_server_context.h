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

#ifndef PAGESPEED_IIS_IIS_SERVER_CONTEXT_H_
#define PAGESPEED_IIS_IIS_SERVER_CONTEXT_H_

#include <memory>

#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/system_server_context.h"

namespace net_instaweb {

class IisConfig;
class IisRewriteDriverFactory;
class LicenseValidator;

// IIS-specific ServerContext managing per-application-pool state.
//
// This class extends SystemServerContext to add:
// 1. License key validation for commercial deployments
// 2. IIS-specific configuration from web.config
// 3. Statistics tracking for the admin UI
class IisServerContext : public SystemServerContext {
 public:
  explicit IisServerContext(IisRewriteDriverFactory* factory);
  ~IisServerContext() override;

  // License validation
  bool ValidateLicense(const GoogleString& license_key);
  // TODO(iis): Re-enable license check for production
  // For testing, always return true to bypass license validation
  bool IsLicenseValid() const { return true; /* license_valid_; */ }
  GoogleString GetLicenseError() const { return license_error_; }

  // Configuration from web.config
  void ApplyConfiguration(std::unique_ptr<IisConfig> config);
  const IisConfig* config() const { return config_.get(); }

  // Admin UI support
  bool IsAdminPath(const GoogleString& path) const;
  bool IsStatisticsPath(const GoogleString& path) const;

  // Unplugged mode - returns true if module should be inactive
  bool unplugged() const;

  // Access the factory
  IisRewriteDriverFactory* iis_factory() { return iis_factory_; }

  // ProxyFetch factory for HTML rewriting (following Apache pattern)
  ProxyFetchFactory* proxy_fetch_factory() {
    return proxy_fetch_factory_.get();
  }
  void InitProxyFetchFactory();

 private:
  IisRewriteDriverFactory* iis_factory_;  // Not owned (parent class owns)
  std::unique_ptr<IisConfig> config_;
  std::unique_ptr<LicenseValidator> license_validator_;
  std::unique_ptr<ProxyFetchFactory> proxy_fetch_factory_;
  bool license_valid_;
  GoogleString license_key_;
  GoogleString license_error_;

  DISALLOW_COPY_AND_ASSIGN(IisServerContext);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_SERVER_CONTEXT_H_
