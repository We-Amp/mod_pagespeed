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

#pragma once

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace Envoy {
namespace Http {
class HttpPageSpeedDecoderFilter;
}
}  // namespace Envoy

namespace net_instaweb {

class EnvoyServerContext;

// AsyncFetch implementation for admin/statistics pages.
// Buffers the response body from AdminSite handlers and sends
// the complete response via Envoy's sendLocalReply().
class EnvoyAdminFetch : public AsyncFetch {
 public:
  EnvoyAdminFetch(EnvoyServerContext* server_context,
                  Envoy::Http::HttpPageSpeedDecoderFilter* decoder);
  ~EnvoyAdminFetch() override;

 protected:
  bool HandleWrite(const StringPiece& sp, MessageHandler* handler) override;
  bool HandleFlush(MessageHandler* handler) override;
  void HandleHeadersComplete() override;
  void HandleDone(bool success) override;

 private:
  EnvoyServerContext* server_context_;
  Envoy::Http::HttpPageSpeedDecoderFilter* decoder_;
  GoogleString buffer_;

  EnvoyAdminFetch(const EnvoyAdminFetch&) = delete;
  EnvoyAdminFetch& operator=(const EnvoyAdminFetch&) = delete;
};

}  // namespace net_instaweb
