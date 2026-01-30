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

#include "pagespeed/windows/pagespeed_c_api.h"

#include <cstring>
#include <memory>
#include <string>

#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/opt/http/request_context.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "pagespeed/system/system_thread_system.h"
#include "pagespeed/windows/iis_rewrite_driver_factory.h"

namespace {

// Internal process context — kept alive for the lifetime of the process.
net_instaweb::ProcessContext* g_process_context = nullptr;

}  // namespace

struct pagespeed_instance {
  std::unique_ptr<net_instaweb::SystemRewriteDriverFactory> factory;
  net_instaweb::ServerContext* server_context;  // Owned by factory.
  net_instaweb::GoogleMessageHandler handler;
};

struct pagespeed_request {
  pagespeed_instance* instance;
  GoogleString url;
  net_instaweb::RequestHeaders request_headers;
};

extern "C" {

int pagespeed_global_init(void) {
  if (g_process_context != nullptr) {
    return PAGESPEED_OK;  // Already initialized.
  }
  g_process_context = new net_instaweb::ProcessContext();
  return PAGESPEED_OK;
}

void pagespeed_global_shutdown(void) {
  delete g_process_context;
  g_process_context = nullptr;
}

pagespeed_t pagespeed_create(const char* config_path,
                             const char* cache_path) {
  if (g_process_context == nullptr) {
    return nullptr;
  }

  auto* ps = new pagespeed_instance;
  auto* thread_system = new net_instaweb::SystemThreadSystem();
  auto* factory = new net_instaweb::IisRewriteDriverFactory(
      *g_process_context, thread_system, "localhost", 80);
  ps->factory.reset(factory);
  factory->Init();

  // TODO(windows): Parse config_path/pagespeed.conf and apply options.
  // TODO(windows): Set cache_path on options so caches use it.
  (void)config_path;
  (void)cache_path;

  net_instaweb::SystemServerContext* server_context =
      factory->MakeNewServerContext();
  ps->server_context = server_context;

  factory->RootInit();
  factory->ChildInit();

  return ps;
}

void pagespeed_destroy(pagespeed_t ps) {
  delete ps;
}

int pagespeed_set_option(pagespeed_t ps, const char* name, const char* value) {
  if (ps == nullptr || ps->server_context == nullptr) {
    return PAGESPEED_ERROR;
  }
  net_instaweb::RewriteOptions* options =
      ps->server_context->global_options()->Clone();
  GoogleString msg;
  net_instaweb::RewriteOptions::OptionSettingResult result =
      options->ParseAndSetOptionFromName1(name, value, &msg,
                                          &ps->handler);
  if (result != net_instaweb::RewriteOptions::kOptionOk) {
    delete options;
    return PAGESPEED_ERROR;
  }
  ps->server_context->global_options()->Merge(*options);
  delete options;
  return PAGESPEED_OK;
}

pagespeed_request_t pagespeed_request_create(pagespeed_t ps, const char* url) {
  if (ps == nullptr) {
    return nullptr;
  }
  auto* req = new pagespeed_request;
  req->instance = ps;
  req->url = url;
  return req;
}

void pagespeed_request_set_header(pagespeed_request_t req, const char* name,
                                  const char* value) {
  if (req != nullptr) {
    req->request_headers.Add(name, value);
  }
}

int pagespeed_rewrite_html(pagespeed_request_t req, const char* html_in,
                           size_t html_in_len, char** html_out,
                           size_t* html_out_len) {
  if (req == nullptr || req->instance == nullptr ||
      req->instance->server_context == nullptr) {
    return PAGESPEED_ERROR;
  }

  GoogleString output;
  net_instaweb::StringWriter writer(&output);

  // Create a RequestContext for this request.
  net_instaweb::RequestContextPtr request_context(
      new net_instaweb::RequestContext(
          net_instaweb::kDefaultHttpOptionsForTests,
          new net_instaweb::NullMutex(),
          req->instance->factory->timer()));

  net_instaweb::RewriteDriver* driver =
      req->instance->server_context->NewRewriteDriver(request_context);
  driver->SetRequestHeaders(req->request_headers);

  net_instaweb::GoogleUrl gurl(req->url);
  if (!gurl.IsWebValid()) {
    driver->Cleanup();
    return PAGESPEED_ERROR;
  }

  driver->SetWriter(&writer);
  driver->StartParse(gurl.Spec());
  driver->ParseText(StringPiece(html_in, html_in_len));
  driver->FinishParse();

  // Copy output to a C-allocated buffer.
  *html_out_len = output.size();
  *html_out = static_cast<char*>(malloc(output.size()));
  if (*html_out == nullptr) {
    return PAGESPEED_ERROR;
  }
  memcpy(*html_out, output.data(), output.size());
  return PAGESPEED_OK;
}

int pagespeed_should_optimize_resource(pagespeed_request_t /*req*/,
                                       const char* /*url*/) {
  // TODO(windows): Implement IPRO lookup via RewriteDriver::DecodeUrl.
  return 0;
}

int pagespeed_optimize_resource(pagespeed_request_t /*req*/,
                                const char* /*resource_in*/,
                                size_t /*resource_in_len*/,
                                const char* /*content_type*/,
                                char** /*resource_out*/,
                                size_t* /*resource_out_len*/) {
  // TODO(windows): Implement IPRO resource optimization.
  return PAGESPEED_ERROR;
}

void pagespeed_free_buffer(char* buf) { free(buf); }

void pagespeed_request_destroy(pagespeed_request_t req) { delete req; }

}  // extern "C"
