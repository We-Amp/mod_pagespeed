// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The purge state the HTML cache engine reads on IIS
// (DaemonHtmlPurgeSource): the server's own (SystemDaemonHtmlPurgeSource),
// plus the purge file as it is on disk now.
//
// In a web garden each worker process reads the shared purge file only at
// the start of a request and at the purge poll interval, so the purge set
// its options hold can miss a URL purge another process wrote a moment
// ago: a commit or a serve in this process would then not see that the page
// was purged.  This source also reads the file itself, again whenever its
// modification time or size changed and at least once a second, and holds
// an entry purged when either view says so.  It never replaces the server's
// own purge set, so a purge this process made and has not written to the
// file yet is still seen.
//
// Free of the IIS headers; thread-safe.

#ifndef PAGESPEED_IIS_IIS_DAEMON_HTML_PURGE_SOURCE_H_
#define PAGESPEED_IIS_IIS_DAEMON_HTML_PURGE_SOURCE_H_

#include <mutex>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/system/daemon_html_engine.h"

namespace net_instaweb {

class FileSystem;
class Timer;

class IisDaemonHtmlPurgeSource : public DaemonHtmlPurgeSource {
 public:
  // `system`, `file_system` and `timer` are borrowed; `purge_file` is the
  // cache path's purge file (SystemCachePath::purge_file).
  IisDaemonHtmlPurgeSource(const DaemonHtmlPurgeSource* system,
                           FileSystem* file_system, Timer* timer,
                           StringPiece purge_file);

  bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                int64 time_ms) const override;
  int64 GlobalInvalidationTimestampMs() const override;
  bool FlushFileMtimeMs(int64* mtime_ms) const override;

 private:
  // Re-reads the file when it changed or the last read is a second old.
  void Refresh() const;

  const DaemonHtmlPurgeSource* system_;
  FileSystem* file_system_;
  Timer* timer_;
  const GoogleString purge_file_;

  mutable std::mutex mutex_;
  mutable PurgeSet from_file_;
  mutable int64 read_ms_ = -1;
  mutable int64 mtime_s_ = -1;
  mutable int64 size_ = -1;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DAEMON_HTML_PURGE_SOURCE_H_
