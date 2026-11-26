// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The purge state a server's own options hold, as the HTML cache engine
// reads it (DaemonHtmlPurgeSource): the URL purges of the options' purge set
// (RewriteOptions::IsUrlCacheValidIgnoringGlobal), the global invalidation
// timestamp, and the cache.flush file's modification time.
//
// The options are the server's global options, which the cache path's purge
// polling and the cache.flush check update in place; this class only reads
// them, under their own lock, and is thread-safe as they are.

#ifndef PAGESPEED_SYSTEM_DAEMON_HTML_PURGE_SOURCE_H_
#define PAGESPEED_SYSTEM_DAEMON_HTML_PURGE_SOURCE_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_engine.h"

namespace net_instaweb {

class FileSystem;
class RewriteOptions;

// The cache.flush file the legacy global flush watches: the server's
// CacheFlushFilename, relative to its FileCachePath unless absolute
// ("cache.flush" when unset).  Empty when there is no FileCachePath to put a
// relative name under.
GoogleString DaemonHtmlCacheFlushPath(StringPiece cache_flush_filename,
                                      StringPiece file_cache_path);

class SystemDaemonHtmlPurgeSource : public DaemonHtmlPurgeSource {
 public:
  // `options` and `file_system` are borrowed and must outlive this source;
  // `flush_path` is DaemonHtmlCacheFlushPath for the server.
  SystemDaemonHtmlPurgeSource(const RewriteOptions* options,
                              FileSystem* file_system, StringPiece flush_path);

  bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                int64 time_ms) const override;
  int64 GlobalInvalidationTimestampMs() const override;
  // Only while EnableCachePurge is off: with it on, the module does not
  // treat a touch of cache.flush as a purge, so neither does the engine.
  bool FlushFileMtimeMs(int64* mtime_ms) const override;

 private:
  const RewriteOptions* options_;
  FileSystem* file_system_;
  const GoogleString flush_path_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_PURGE_SOURCE_H_
