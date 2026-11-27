// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Cache directory startup validation.
//
// Since the H1 privilege drop the daemon runs unprivileged (User=pagespeed)
// and MUST own every file in its cache directory.  Startup refuses — loudly,
// never silently cache-off — when the directory is absent, unwritable, or
// holds content owned by another uid (the classic case: a root-owned legacy
// volume moved into vN/ by hand).  Nothing here ever chowns, migrates, or
// falls back to another location.

#ifndef SRC_WORKER_CACHE_DIR_H_
#define SRC_WORKER_CACHE_DIR_H_

#include <cstdint>
#include <string>

namespace pagespeed {

class MessageHandler;

// The compiled-in cache-directory generation (N in
// /var/cache/pagespeed-optimizer/v<N>) lives in shared_config.h as
// kCacheDirGeneration; the default directory derives from it.
// Exposed here so main.cc's flag default and the packaged tmpfiles.d line
// cannot drift apart without touching the same file.
constexpr char kDefaultCacheDirPrefix[] = "/var/cache/pagespeed-optimizer/v";

// Why the cache directory is unusable.  kOk means the daemon may proceed.
enum class CacheDirStatus : std::uint8_t {
  kOk,
  kMissing,        // does not exist (packaging/tmpfiles should have made it)
  kNotADirectory,  // exists but is not a directory
  kNotWritable,    // permission denied for this euid (search/write)
  kForeignOwned,   // an entry inside is owned by a different uid
  kStatFailed,     // stat/lstat failed for a reason other than the above
};

// Inspect `dir` from the perspective of the current euid.  When the result
// is not kOk, `detail` names the offending path (for kForeignOwned: the
// first foreign-owned DAEMON-AUTHORED entry found) and, for
// kForeignOwned/kStatFailed, carries the observed uid / errno context for
// the log line.
//
// POSIX-only: on Windows this always returns kOk (ACLs, not uid/mode, are
// the access model there; the Windows service story is a separate arc).
// `volume_stem` is the configured cache path's basename WITHOUT its
// extension (`std::filesystem::path::stem`) — the stem the cache layer names
// its volume files after.  It must be the extension-free form: volume names
// are built by inserting -<format>-<geometry> before the extension, so
// `cache.vol` produces `cache-6-<hex>.vol`, which only the stem matches.  The ownership scan is limited
// to entries the daemon itself authors — stem-named files plus the fixed set
// of sidecars listed in cache_dir.cc — because the cache directory is
// group-writable by design (the web-server peers are in the daemon's group)
// and a directory-wide scan would let any group member, or a filesystem's
// own `lost+found`, refuse the daemon's start permanently.  An empty stem
// disables the stem-prefix half of the match.
CacheDirStatus ValidateCacheDir(const std::string& dir,
                                const std::string& volume_stem,
                                std::string* detail);

// Whether `name` (a bare directory entry name) is a file the daemon authors
// inside its cache directory, given the configured volume stem.  Exposed for
// testing; the enumeration lives in cache_dir.cc.
bool IsDaemonAuthoredCacheEntry(const std::string& name,
                                const std::string& volume_stem);

// Log the loud, fatal explanation for a non-kOk status and return false, so
// call sites read `if (!RefuseCacheDir(...)) return false;`.  The message
// always distinguishes "absent" from "permission denied" from "foreign
// ownership", states that nothing was chowned/migrated, names the packaged
// mechanism that should have created the directory, and points at the
// migration documentation.  Never returns true for a non-kOk status.
bool RefuseCacheDir(CacheDirStatus status, const std::string& dir,
                    const std::string& detail, MessageHandler* handler);

}  // namespace pagespeed

#endif  // SRC_WORKER_CACHE_DIR_H_
