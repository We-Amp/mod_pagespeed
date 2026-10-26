// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/file_key_directory_cache.h"

namespace net_instaweb {
namespace webbotauth {

std::shared_ptr<StaticKeyDirectory> FileKeyDirectoryCache::Get(
    FileSystem* fs, int64 now_ms, const GoogleString& path,
    MessageHandler* handler) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(path);
    // The stat itself is what the request path pays for between changes:
    // serve the last result without touching the file system until the
    // recheck interval has passed.
    if (it != entries_.end() &&
        now_ms - it->second.last_check_ms < kRecheckIntervalMs) {
      return it->second.directory;
    }
  }

  int64 mtime_sec = 0;
  int64 size = 0;
  if (fs == nullptr || path.empty() || !fs->Size(path, &size, handler) ||
      !fs->Mtime(path, &mtime_sec, handler)) {
    // Missing or unreadable: no keys, remembered like any other result so a
    // missing file is not stat'ed on every lookup either.
    if (fs != nullptr && !path.empty()) {
      std::shared_ptr<StaticKeyDirectory> empty =
          std::make_shared<StaticKeyDirectory>(StringPiece());
      std::lock_guard<std::mutex> lock(mutex_);
      entries_[path] = Entry{-1, -1, now_ms, empty};
    }
    return std::make_shared<StaticKeyDirectory>(StringPiece());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.mtime_sec == mtime_sec &&
        it->second.size == size) {
      it->second.last_check_ms = now_ms;
      return it->second.directory;
    }
  }

  // Read outside the lock; a concurrent reader of the same change just
  // stores an equivalent directory.
  GoogleString jwks_doc;
  if (!fs->ReadFile(path.c_str(), max_file_bytes_, &jwks_doc, handler)) {
    jwks_doc.clear();
  }
  std::shared_ptr<StaticKeyDirectory> directory =
      std::make_shared<StaticKeyDirectory>(jwks_doc);
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[path] = Entry{mtime_sec, size, now_ms, directory};
  return directory;
}

}  // namespace webbotauth
}  // namespace net_instaweb
