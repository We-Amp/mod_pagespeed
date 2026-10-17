// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// FileKeyDirectoryCache: the operator-local JWKS files, read once per change.
//
// The request path resolves keys from an operator-configured JWKS file. That
// file changes rarely, so instead of reading it on every request this keeps
// the loaded StaticKeyDirectory per path and reuses it until the file's
// modification time or size changes. Checking costs one stat; the read
// (bounded by max_file_bytes) happens only on the first use and after a
// change. A file that cannot be stat'ed or read yields an empty directory
// (every lookup kNotFound), the same fail-closed result as before.
//
// Paths come only from operator configuration, so the number of entries is
// bounded by the configuration. Safe to call from several threads.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_FILE_KEY_DIRECTORY_CACHE_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_FILE_KEY_DIRECTORY_CACHE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/webbotauth/static_key_directory.h"

namespace net_instaweb {
namespace webbotauth {

class FileKeyDirectoryCache {
 public:
  explicit FileKeyDirectoryCache(int64_t max_file_bytes)
      : max_file_bytes_(max_file_bytes) {}

  // The directory loaded from `path`, re-read only when the file's
  // modification time or size differs from the last read. Never null.
  std::shared_ptr<StaticKeyDirectory> Get(FileSystem* fs,
                                          const GoogleString& path,
                                          MessageHandler* handler);

 private:
  struct Entry {
    int64 mtime_sec;
    int64 size;
    std::shared_ptr<StaticKeyDirectory> directory;
  };

  const int64_t max_file_bytes_;
  std::mutex mutex_;
  std::map<GoogleString, Entry> entries_;
};

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_FILE_KEY_DIRECTORY_CACHE_H_
