// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/file_key_directory_cache.h"

namespace net_instaweb {
namespace webbotauth {

std::shared_ptr<StaticKeyDirectory> FileKeyDirectoryCache::Get(
    FileSystem* fs, const GoogleString& path, MessageHandler* handler) {
  int64 mtime_sec = 0;
  int64 size = 0;
  if (fs == nullptr || path.empty() || !fs->Size(path, &size, handler) ||
      !fs->Mtime(path, &mtime_sec, handler)) {
    // Missing or unreadable: no keys, and nothing kept for the path.
    if (!path.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.erase(path);
    }
    return std::make_shared<StaticKeyDirectory>(StringPiece());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.mtime_sec == mtime_sec &&
        it->second.size == size) {
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
  entries_[path] = Entry{mtime_sec, size, directory};
  return directory;
}

}  // namespace webbotauth
}  // namespace net_instaweb
