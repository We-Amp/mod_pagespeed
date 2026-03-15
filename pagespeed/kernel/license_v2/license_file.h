// Copyright (c) 2024-2026 We-Amp B.V.

// License file I/O: read/write license tokens from/to disk.

#ifndef PAGESPEED_KERNEL_LICENSE_V2_LICENSE_FILE_H_
#define PAGESPEED_KERNEL_LICENSE_V2_LICENSE_FILE_H_

#include <filesystem>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Returns the license file path derived from the cache directory.
// Places the license file in the parent of the cache path, e.g.
// cache_path="/var/cache/mod_pagespeed" → "/var/cache/pagespeed.license"
// Falls back to /etc/modpagespeed/license.key if cache_path is empty.
std::filesystem::path LicenseFilePath(const GoogleString& cache_path = "");

// Read the license token from the given file path.
// Returns true on success, false if the file does not exist or cannot be read.
bool ReadLicenseFile(const std::filesystem::path& path, GoogleString* token);

// Write a license token to the given file path.
// Creates parent directories if needed.
// Returns true on success, false on I/O error.
bool WriteLicenseFile(const std::filesystem::path& path, StringPiece token);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_V2_LICENSE_FILE_H_
