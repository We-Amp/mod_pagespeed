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

// The ONE place this module builds a notification to the optimizer worker.
//
// Every notification the in-place arms send -- the record arm's ask after a
// stored original, and the serve arm's fallback re-notify, origin-refreshed
// sentinel and lost-copy ask -- goes through BuildModuleNotifyParams, so the
// rules below hold for all of them by construction rather than by four
// copies agreeing:
//
//   * it names the site of the server block, virtual host or IIS site that
//     admitted the request (a site id, see IsSiteId), and is not built
//     without one;
//   * its flags are exactly kPsNotifyFlagFromModule: this module never
//     vouches for a host name, so nothing it sends can start work that
//     fetches from a host;
//   * it is never an agent request;
//   * it never carries HTML, with ONE exception: the HTML cache's own
//     notification for a page its store wrote (`html_stored` below).  Any
//     other HTML request builds nothing, moves DaemonNotifyHtmlRefused(), and
//     the caller sends nothing.
//
// The exception is narrow by construction.  An HTML notification is built
// only with `html_stored` set, and then carries exactly kPsNotifyFlagFromModule
// | kPsNotifyFlagHostVouched | kPsNotifyFlagHtmlStored: the caller states that
// the request's host is one of the site's exact configured names and that its
// own store wrote the page in this volume generation.  Only such a
// notification may be an agent request.  `html_stored` on anything but HTML
// builds nothing, so the vouched bit can never ride on a resource.

#ifndef PAGESPEED_SYSTEM_DAEMON_NOTIFY_PARAMS_H_
#define PAGESPEED_SYSTEM_DAEMON_NOTIFY_PARAMS_H_

#include <cstdint>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"

namespace net_instaweb {

// What a notification says.  Every StringPiece is borrowed for the duration
// of the BuildModuleNotifyParams call only; the built object keeps copies.
struct ModuleNotifyInput {
  StringPiece url;
  StringPiece hostname;
  StringPiece scheme;
  StringPiece site;
  int content_type = kPsContentOther;
  uint32_t mask = 0;
  // Both or neither; empty is sent as absent.
  StringPiece option_context;
  StringPiece option_signature;
  // The HTML cache's notification for a page its store wrote, for a host the
  // caller vouches for (HostExactlyConfigured).  Legal only with HTML.
  bool html_stored = false;
  // With html_stored only: the requesting client asked for the Markdown copy.
  bool agent_request = false;
};

// A built notification: the parameter struct and the storage its pointers
// name.  Not copyable, because the struct points into this object.
class ModuleNotifyParams {
 public:
  ModuleNotifyParams() = default;
  ModuleNotifyParams(const ModuleNotifyParams&) = delete;
  ModuleNotifyParams& operator=(const ModuleNotifyParams&) = delete;

  const PsNotifyParams& params() const { return params_; }

 private:
  friend bool BuildModuleNotifyParams(const DaemonAbi& abi,
                                      const ModuleNotifyInput& input,
                                      ModuleNotifyParams* out);

  GoogleString url_;
  GoogleString hostname_;
  GoogleString scheme_;
  GoogleString site_;
  GoogleString option_context_;
  GoogleString option_signature_;
  PsNotifyParams params_ = {};
};

// Builds the notification for `input` into `*out`.  Returns false, and `*out`
// holds nothing to send, for HTML content without html_stored
// (DaemonNotifyHtmlRefused() moves), for html_stored on anything but HTML,
// and for a request without a site id; the caller then does not call
// NotifyWorker.
bool BuildModuleNotifyParams(const DaemonAbi& abi,
                             const ModuleNotifyInput& input,
                             ModuleNotifyParams* out);

// Number of notifications this process declined to build because they named
// HTML content (daemon_notify_html_refused).  Monotonic; never reset.
int64 DaemonNotifyHtmlRefused();

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_NOTIFY_PARAMS_H_
