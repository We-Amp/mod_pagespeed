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

#ifndef PAGESPEED_SYSTEM_ADMIN_SITE_H_
#define PAGESPEED_SYSTEM_ADMIN_SITE_H_

#include <atomic>
#include <cstddef>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class AdminDaemonHandler;
class AsyncFetch;
class CacheInterface;
class DaemonReader;
class GoogleUrl;
class HTTPCache;
class MessageHandler;
class PropertyCache;
class QueryParams;
class RewriteOptions;
class ServerContext;
class Statistics;
class SystemCachePath;
class SystemCaches;
class SystemRewriteOptions;
class Timer;

// Identifies which admin handler family a request hit, so the exposure
// warning can fire once per family per process lifetime instead of once
// globally (operators may have hardened one path but not another).
enum class AdminHandlerFamily {
  kAdmin = 0,         // /pagespeed_admin/*
  kGlobalAdmin,       // /pagespeed_global_admin/*
  kStatistics,        // /*_pagespeed_statistics
  kGlobalStatistics,  // /*_pagespeed_global_statistics
  kConsole,           // /pagespeed_console
  kMessages,          // /*_pagespeed_message
  kNumFamilies,       // Sentinel; keep last.
};

// Defense-in-depth log signal: if an admin/statistics handler is invoked
// for a non-loopback client, emit a single warning per handler family per
// process lifetime. This does NOT block the request — admins may have
// legitimately widened access (CDN admin IP, ops VLAN). The warning is a
// hint that the web-server-layer ACL the deployment relies on may not be
// effective; the warning text carries a link to the hardening docs. Safe to
// call from any thread; idempotent via atomic flag.
//
// `client_ip` is a textual IP (IPv4 dotted-quad or IPv6); empty string is
// treated as non-loopback (caller couldn't determine — flag it loudly).
// `handler_name` is the URL handler string used in the warning text (e.g.
// "pagespeed_admin"). Pass nullptr handler to skip logging (used to test
// the loopback predicate directly).
void WarnIfNonLoopbackAdminAccess(StringPiece client_ip,
                                  AdminHandlerFamily family,
                                  StringPiece handler_name,
                                  MessageHandler* message_handler);

// Returns true if `client_ip` represents an IPv4 or IPv6 loopback address
// (127.0.0.0/8, ::1, or the IPv4-mapped/compatible variants ::ffff:127.x.y.z
// and ::127.x.y.z). Also accepts the literal "localhost". Empty input is
// NOT considered loopback. Exposed for testing.
bool IsLoopbackClientIp(StringPiece client_ip);

// Resets the per-family warning flags. Test-only.
void ResetAdminExposureWarningsForTesting();

// Message templates.  A message-history line is "[<time>] [<Level>] [<pid>] "
// (optionally followed by "[<file>:<line>] ") and the message; further lines
// of a multi-line message carry no header.  The admin console reduces lines to
// templates to group repeats, and implements exactly these rules again in
// TypeScript (console/src/lib/utils/message-template.ts); both are tested
// against test/pagespeed/system/testdata/message_templates.tsv.

// The message text of a line with its header removed.  A line without a
// "[<time>] [<Level>] " header (a continuation line) is returned unchanged, so
// a line has a header exactly when the result is shorter than the line.
StringPiece MessageLineBody(StringPiece line);

// The time in a line's header, in epoch milliseconds; -1 when the line has no
// header or its time does not parse.
int64 MessageLineTimeMs(StringPiece line);

// The template of a message body: every http:// or https:// URL becomes
// "URL", every hexadecimal identifier "ID" and every run of digits "N".
GoogleString MessageTemplate(StringPiece body);

// Upper bound on a grouped message-history answer (message_history?grouped=1).
constexpr size_t kMaxGroupedMessageBytes = 1 << 20;

// Largest window_s a grouped message-history answer accepts (one day).
constexpr int64 kMaxMessageWindowSeconds = 86400;

// Implements the /pagespeed_admin pages.
// All handlers return JSON responses; the SPA console is served as a
// single embedded HTML page generated by data2c from the Vite build output.
class AdminSite {
 public:
  // Identifies whether the user arrived at an admin page from a
  // /pagespeed_admin handler or a /*_pagespeed_statistics handler.
  // The main difference between these is that the _admin site might in the
  // future grant more privileges than the statistics site did, such as flushing
  // cache.  But it also affects the syntax of the links created to sub-pages
  // in the top navigation bar.
  enum AdminSource { kPageSpeedAdmin, kStatistics, kOther };

  // Takes ownership of |daemon_reader| (may be null: ports without a daemon
  // transport get a /v1/daemon/* proxy that reports the daemon as absent).
  AdminSite(Timer* timer, MessageHandler* message_handler,
            DaemonReader* daemon_reader);

  ~AdminSite();

  // Serves the embedded SPA console HTML.
  void ServeSpaConsole(AsyncFetch* fetch);

  // Handler which serves PSOL console.
  // If ?json query param is present, returns JSON via ConsoleJsonHandler.
  // Otherwise, serves the SPA console.
  void ConsoleHandler(const SystemRewriteOptions& global_options,
                      const RewriteOptions& options, AdminSource source,
                      const QueryParams& query_params, AsyncFetch* fetch,
                      Statistics* statistics);

  // Displays recent Info/Warning/Error messages as JSON, scoped to this
  // process and carrying a `next` write-count cursor. `query_params` may
  // carry `since=<n>`: only messages written after write-count `n` are
  // returned (all retained messages if `since` predates what's retained,
  // none if `since` already equals `next`).
  void MessageHistoryHandler(const RewriteOptions& options, AdminSource source,
                             const QueryParams& query_params,
                             AsyncFetch* fetch);

  // Handle a request for /pagespeed_admin/*, which is a launching
  // point for all the administrator pages including stats,
  // message-histogram, console, etc.
  // `own_serve_host` is the host this request's own site records its serves
  // under (VouchedServeHost, supplied by the port; "" when it has none): a
  // per-virtual-host console's daemon answers are narrowed to it.
  void AdminPage(bool is_global, const GoogleUrl& stripped_gurl,
                 const QueryParams& query_params, const RewriteOptions* options,
                 SystemCachePath* cache_path, AsyncFetch* fetch,
                 SystemCaches* system_caches,
                 CacheInterface* filesystem_metadata_cache,
                 HTTPCache* http_cache, CacheInterface* metadata_cache,
                 PropertyCache* page_property_cache,
                 ServerContext* server_context, Statistics* statistics,
                 Statistics* stats,
                 SystemRewriteOptions* global_system_rewrite_options,
                 StringPiece request_body = StringPiece(),
                 StringPiece own_serve_host = StringPiece());

  // Handle a request for the legacy /*_pagespeed_statistics page, which also
  // serves as a launching point for a subset of the admin pages.
  void StatisticsPage(bool is_global, const QueryParams& query_params,
                      const RewriteOptions* options, AsyncFetch* fetch,
                      SystemCaches* system_caches,
                      CacheInterface* filesystem_metadata_cache,
                      HTTPCache* http_cache, CacheInterface* metadata_cache,
                      PropertyCache* page_property_cache,
                      ServerContext* server_context, Statistics* statistics,
                      Statistics* stats,
                      SystemRewriteOptions* global_system_rewrite_options);

  // Returns JSON used by the PageSpeed Console JavaScript.
  void ConsoleJsonHandler(const QueryParams& params, AsyncFetch* fetch,
                          Statistics* statistics);

  // Handler for /mod_pagespeed_statistics and
  // /ngx_pagespeed_statistics, as well as
  // /...pagespeed__global_statistics.  Returns JSON via DumpJson.
  void StatisticsHandler(const RewriteOptions& options, AdminSource source,
                         AsyncFetch* fetch, Statistics* stats, bool is_global,
                         StringPiece host);

  // Responds to 'fetch' with data used on statistics page and graphs page
  // in JSON format.
  void StatisticsJsonHandler(AsyncFetch* fetch, Statistics* stats,
                             bool is_global, StringPiece host);

  // Display various charts on graphs page.
  // If ?json query param is present, returns JSON via ConsoleJsonHandler.
  // Otherwise, serves the SPA console.
  void GraphsHandler(const RewriteOptions& options, AdminSource source,
                     const QueryParams& query_params, AsyncFetch* fetch,
                     Statistics* stats);

  // Print details for configuration as JSON: the server-wide configuration
  // (`server_config`), the options actually in effect for this request/vhost
  // (`effective_options`), which scope this view is showing ("global" for
  // /pagespeed_global_admin/*, "vhost" otherwise), and the host this
  // instance identifies as.
  void PrintConfig(AdminSource source, AsyncFetch* fetch,
                   const RewriteOptions& server_config,
                   const RewriteOptions& effective_options, bool is_global,
                   StringPiece host);

  // Print statistics about the caches as JSON. Cache-entry deletion (via
  // url=...&Delete) and cache purge (via purge=...) are state-changing
  // actions: they require a same-origin POST (see GateCacheAction in
  // admin_site.cc) and, when `is_global` is false, are scoped to the vhost
  // the request arrived on -- purging "*" or naming a URL on another host is
  // refused -- backend cache server status (memcached/redis) is included
  // only when `is_global` is true; a per-vhost response includes statistics
  // for the vhost's own cache path when one is available.
  void PrintCaches(bool is_global, AdminSource source,
                   const GoogleUrl& stripped_gurl,
                   const QueryParams& query_params,
                   const RewriteOptions* options, SystemCachePath* cache_path,
                   AsyncFetch* fetch, SystemCaches* system_caches,
                   CacheInterface* filesystem_metadata_cache,
                   HTTPCache* http_cache, CacheInterface* metadata_cache,
                   PropertyCache* page_property_cache,
                   ServerContext* server_context);

  // Print histograms showing the dynamics of server activity as JSON.
  void PrintHistograms(AdminSource source, AsyncFetch* fetch,
                       Statistics* stats);

  void PurgeHandler(StringPiece url, SystemCachePath* cache_path,
                    AsyncFetch* fetch);

  // Return the daemon proxy handler (for the /v1/daemon/* endpoints).
  AdminDaemonHandler* daemon_handler() { return daemon_handler_.get(); }

  // Return the message handler for debugging use.
  MessageHandler* MessageHandlerForTesting() { return message_handler_; }

  // Marks the grouped message-history read as in flight (true) or free.
  void SetGroupedSlotBusyForTesting(bool busy) {
    grouped_in_flight_.store(busy, std::memory_order_release);
  }

 private:
  MessageHandler* message_handler_;
  Timer* timer_;
  // The daemon reader is owned here; the handler borrows it.  Declaration
  // order matters: the handler is destroyed first and drains in-flight
  // fetches before the reader (and its transport) is torn down.
  std::unique_ptr<DaemonReader> daemon_reader_;
  std::unique_ptr<AdminDaemonHandler> daemon_handler_;
  // One grouped message-history read at a time per admin site: the grouping
  // walks the whole retained buffer.
  std::atomic<bool> grouped_in_flight_{false};
  AdminSite(const AdminSite&) = delete;
  AdminSite& operator=(const AdminSite&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_ADMIN_SITE_H_
