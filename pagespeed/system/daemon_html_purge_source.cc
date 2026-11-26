// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_purge_source.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

namespace {

bool IsAbsolutePath(StringPiece path) {
  if (!path.empty() && path[0] == '/') return true;
#ifdef _WIN32
  return path.size() >= 3 && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/');
#else
  return false;
#endif
}

}  // namespace

GoogleString DaemonHtmlCacheFlushPath(StringPiece cache_flush_filename,
                                      StringPiece file_cache_path) {
  // The rule SystemServerContext::CheckLegacyGlobalCacheFlushFile applies.
  const StringPiece name = cache_flush_filename.empty()
                               ? StringPiece("cache.flush")
                               : cache_flush_filename;
  if (IsAbsolutePath(name)) return name.as_string();
  if (file_cache_path.empty()) return GoogleString();
  return StrCat(file_cache_path, "/", name);
}

SystemDaemonHtmlPurgeSource::SystemDaemonHtmlPurgeSource(
    const RewriteOptions* options, FileSystem* file_system,
    StringPiece flush_path)
    : options_(options),
      file_system_(file_system),
      flush_path_(flush_path.as_string()) {}

bool SystemDaemonHtmlPurgeSource::IsUrlValidIgnoringGlobal(
    const GoogleString& url, int64 time_ms) const {
  return options_->IsUrlCacheValidIgnoringGlobal(url, time_ms);
}

int64 SystemDaemonHtmlPurgeSource::GlobalInvalidationTimestampMs() const {
  return options_->has_cache_invalidation_timestamp_ms()
             ? options_->cache_invalidation_timestamp()
             : -1;
}

bool SystemDaemonHtmlPurgeSource::FlushFileMtimeMs(int64* mtime_ms) const {
  if (options_->enable_cache_purge() || flush_path_.empty() ||
      file_system_ == nullptr) {
    return false;
  }
  int64 mtime_s = 0;
  NullMessageHandler handler;
  if (!file_system_->Mtime(flush_path_, &mtime_s, &handler)) return false;
  *mtime_ms = mtime_s * Timer::kSecondMs;
  return true;
}

}  // namespace net_instaweb
