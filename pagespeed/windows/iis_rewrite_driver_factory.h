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

#ifndef PAGESPEED_WINDOWS_IIS_REWRITE_DRIVER_FACTORY_H_
#define PAGESPEED_WINDOWS_IIS_REWRITE_DRIVER_FACTORY_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include "pagespeed/system/system_rewrite_options.h"

namespace net_instaweb {

class MessageHandler;
class ProcessContext;
class ServerContext;
class SystemServerContext;
class SystemThreadSystem;
class UrlAsyncFetcher;

// RewriteDriverFactory for IIS on Windows.
//
// This is a minimal implementation that wires up the system-level factory
// with sensible defaults for a single-process IIS worker.  Resource fetching
// is not yet implemented (AllocateFetcher returns nullptr); a future
// WinHttpUrlAsyncFetcher will provide production-grade fetching.
class IisRewriteDriverFactory : public SystemRewriteDriverFactory {
 public:
  IisRewriteDriverFactory(const ProcessContext& process_context,
                          SystemThreadSystem* thread_system,
                          StringPiece hostname, int port);
  ~IisRewriteDriverFactory() override;

  // Initialises statistics variables owned by this factory and its parents.
  void NonStaticInitStats(Statistics* statistics) override;

  // Creates an IIS-appropriate ServerContext.
  SystemServerContext* MakeNewServerContext();

 protected:
  // Returns nullptr — resource fetching is not yet implemented.
  // TODO(windows): Return a WinHttpUrlAsyncFetcher.
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(IisRewriteDriverFactory);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_WINDOWS_IIS_REWRITE_DRIVER_FACTORY_H_
