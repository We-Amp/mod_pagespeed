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

#include "pagespeed/system/daemon_notify_params.h"

#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/system/site_cache_fragment.h"

namespace net_instaweb {

namespace {

AtomicInt32 g_notify_html_refused(0);

const char* CStrOrNull(const GoogleString& value) {
  return value.empty() ? nullptr : value.c_str();
}

}  // namespace

bool BuildModuleNotifyParams(const DaemonAbi& abi,
                             const ModuleNotifyInput& input,
                             ModuleNotifyParams* out) {
  // Whatever happens below, nothing from an earlier build is left to send.
  out->params_ = PsNotifyParams{};
  if (input.content_type == kPsContentHtml) {
    g_notify_html_refused.BarrierIncrement(1);
    return false;
  }
  if (!IsSiteId(input.site)) {
    return false;
  }
  input.url.CopyToString(&out->url_);
  input.hostname.CopyToString(&out->hostname_);
  input.scheme.CopyToString(&out->scheme_);
  input.site.CopyToString(&out->site_);
  input.option_context.CopyToString(&out->option_context_);
  input.option_signature.CopyToString(&out->option_signature_);

  PsNotifyParams* params = &out->params_;
  abi.NotifyParamsInit(params);
  params->url = out->url_.c_str();
  params->hostname = out->hostname_.c_str();
  params->scheme = out->scheme_.c_str();
  params->content_type = input.content_type;
  params->mask = input.mask;
  params->agent_request = 0;
  params->option_context = CStrOrNull(out->option_context_);
  params->option_context_length = out->option_context_.size();
  params->option_signature = CStrOrNull(out->option_signature_);
  params->site = out->site_.c_str();
  params->notify_flags = kPsNotifyFlagFromModule;
  return true;
}

int64 DaemonNotifyHtmlRefused() { return g_notify_html_refused.value(); }

}  // namespace net_instaweb
