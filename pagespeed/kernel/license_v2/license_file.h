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

// Outcome of attempting to read the license file. Distinguishes "no license
// file at all" (kAbsent — the normal unlicensed install) from "a license file
// IS present but we could not read it" (kUnreadable — e.g. a root-owned 0600
// file under a non-root worker), so an operator gets an actionable signal
// instead of only the generic UNLICENSED banner.
enum class LicenseFileReadStatus {
  kOk,          // File read and a non-empty token was returned.
  kAbsent,      // File does not exist (normal unlicensed state).
  kUnreadable,  // File exists but open()/stat() failed (e.g. EACCES). errno-
                // derived detail is logged at WARNING.
  kEmpty,       // File exists but is empty / whitespace-only.
  kTooLarge,    // File exists but exceeds the maximum license token size.
};

// Read the license token from the given file path.
// Returns true on success, false if the file does not exist or cannot be read.
//
// When the file exists but cannot be read (permissions, oversized, empty), a
// single WARNING is logged with the path, the errno reason (strerror), and the
// worker's effective uid, so a misconfigured license file is diagnosable rather
// than silently producing the generic UNLICENSED banner.
bool ReadLicenseFile(const std::filesystem::path& path, GoogleString* token);

// As above, but also reports the read outcome via |status| (may be null). The
// boolean return is true iff |status| is kOk. Exposed so callers (and tests)
// can distinguish "absent" from "present-but-unreadable".
bool ReadLicenseFile(const std::filesystem::path& path, GoogleString* token,
                     LicenseFileReadStatus* status);

// Write a license token to the given file path.
// Creates parent directories if needed.
// Returns true on success, false on I/O error.
bool WriteLicenseFile(const std::filesystem::path& path, StringPiece token);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_LICENSE_V2_LICENSE_FILE_H_
