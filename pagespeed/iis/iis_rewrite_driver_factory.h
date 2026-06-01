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

#ifndef PAGESPEED_IIS_IIS_REWRITE_DRIVER_FACTORY_H_
#define PAGESPEED_IIS_IIS_REWRITE_DRIVER_FACTORY_H_

#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"

namespace net_instaweb {

class CacheInterface;
class GoogleMessageHandler;
class IisConfig;
class IisServerContext;
class MessageHandler;
class ProcessContext;
class RewriteOptions;
class ServerContext;
class Statistics;
class SystemRewriteOptions;
class SystemThreadSystem;
class UrlAsyncFetcher;

// RewriteDriverFactory for IIS on Windows.
//
// This factory creates RewriteDriver instances for IIS requests and manages:
// - CurlUrlAsyncFetcher for resource fetching
// - Cyclone cache for disk caching
// - IIS-specific server context
//
// This is the new IIS-specific factory that lives in pagespeed/iis/.
// It differs from pagespeed/windows/iis_rewrite_driver_factory.h by:
// - Using IisServerContext instead of SystemServerContext
// - Supporting IIS-specific configuration
// - Integrating with the IIS module lifecycle
class IisRewriteDriverFactory : public SystemRewriteDriverFactory {
 public:
  IisRewriteDriverFactory(const ProcessContext& process_context,
                          SystemThreadSystem* thread_system,
                          const StringPiece& hostname,
                          int port);
  ~IisRewriteDriverFactory() override;

  // Initialize statistics variables
  void NonStaticInitStats(Statistics* statistics) override;

  // Create an IIS-specific server context
  IisServerContext* MakeIisServerContext(std::unique_ptr<IisConfig> config);

  // Apply IIS configuration to default options (filters, etc.)
  void ApplyIisConfig(const IisConfig& config);

  // Apply system-level configuration from IisConfig (cache, fetcher settings)
  // These settings are in SystemRewriteOptions, not RewriteOptions
  void ApplySystemConfig(const IisConfig* config);

  // Shutdown cleanup
  void ShutDown();

  // Getters for hostname and port (needed by IisServerContext)
  const GoogleString& hostname() const { return hostname_; }
  int port() const { return port_; }

 protected:
  // Create CurlUrlAsyncFetcher for resource fetching
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override;

  // Pure virtual implementations
  MessageHandler* DefaultHtmlParseMessageHandler() override;
  MessageHandler* DefaultMessageHandler() override;
  ServerContext* NewDecodingServerContext() override;

  // Override to return SystemRewriteOptions (required for SystemServerContext)
  RewriteOptions* NewRewriteOptions() override;

 private:
  std::unique_ptr<GoogleMessageHandler> message_handler_;
  GoogleString hostname_;
  int port_;

  DISALLOW_COPY_AND_ASSIGN(IisRewriteDriverFactory);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_REWRITE_DRIVER_FACTORY_H_
