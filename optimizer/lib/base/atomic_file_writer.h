// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Atomic file writer: write-to-temp + fsync + rename.
// Consolidates 5 independent implementations across the codebase.

#ifndef LIB_BASE_ATOMIC_FILE_WRITER_H_
#define LIB_BASE_ATOMIC_FILE_WRITER_H_

#include <string>
#include <string_view>

namespace pagespeed {

// Atomically writes `content` to `path` via exclusive temp file + rename.
//
// Steps: unlink stale .tmp → exclusive create → write → fsync → close → rename.
// On failure, cleans up the temp file and returns false.
//
// `mode` is the POSIX permission bits (e.g., 0600 for secrets, 0644 for
// world-readable config). Ignored on Windows.
//
// Symlink-safe: refuses to follow symlinks on both POSIX and Windows.
bool AtomicWriteFile(const std::string& path, std::string_view content,
                     int mode = 0644);

}  // namespace pagespeed

#endif  // LIB_BASE_ATOMIC_FILE_WRITER_H_
