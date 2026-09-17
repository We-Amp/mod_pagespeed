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

#include "pagespeed/windows/iis_rewrite_driver_factory.h"

#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_shared_mem.h"
#include "pagespeed/system/curl_url_async_fetcher.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "pagespeed/system/system_thread_system.h"

namespace net_instaweb {

IisRewriteDriverFactory::IisRewriteDriverFactory(
    const ProcessContext& process_context, SystemThreadSystem* thread_system,
    StringPiece hostname, int port)
    : SystemRewriteDriverFactory(process_context, thread_system,
                                 new NullSharedMem(), hostname, port),
      message_handler_(new GoogleMessageHandler()) {}

IisRewriteDriverFactory::~IisRewriteDriverFactory() {}

void IisRewriteDriverFactory::NonStaticInitStats(Statistics* statistics) {
  SystemRewriteDriverFactory::InitStats(statistics);
}

SystemServerContext* IisRewriteDriverFactory::MakeNewServerContext() {
  auto* ctx = new SystemServerContext(this, hostname_identifier(), 80);
  return ctx;
}

UrlAsyncFetcher* IisRewriteDriverFactory::AllocateFetcher(
    SystemRewriteOptions* config) {
  // Use curl-based fetcher for resource fetching on Windows.
  // This is the same approach used by the Envoy filter.
  CurlUrlAsyncFetcher* fetcher = new CurlUrlAsyncFetcher(
      config->fetcher_proxy().c_str(), thread_system(), statistics(), timer(),
      config->blocking_fetch_timeout_ms(), message_handler_.get());

  fetcher->set_track_original_content_length(track_original_content_length());
  fetcher->set_fetch_with_gzip(config->fetch_with_gzip());
  fetcher->SetHttpsOptions(config->https_options());
  fetcher->SetSslCertificatesDir(config->ssl_cert_directory());
  fetcher->SetSslCertificatesFile(config->ssl_cert_file());

  LOG(INFO) << "IisRewriteDriverFactory: Using curl-based fetcher for "
            << "resource fetching";
  return fetcher;
}

MessageHandler* IisRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
  return message_handler_.get();
}

MessageHandler* IisRewriteDriverFactory::DefaultMessageHandler() {
  return message_handler_.get();
}

ServerContext* IisRewriteDriverFactory::NewDecodingServerContext() {
  ServerContext* sc = new SystemServerContext(this, hostname_identifier(), 80);
  InitStubDecodingServerContext(sc);
  return sc;
}

}  // namespace net_instaweb
