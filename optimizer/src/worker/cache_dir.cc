// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/cache_dir.h"

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <string_view>

#include "lib/base/message_handler.h"

namespace pagespeed {

// The files the daemon itself authors inside its cache directory.  Every name
// here is the one built by the corresponding path helper, so this list moves
// with the code rather than with a guess:
//   pagespeed-shared.conf            shared_config.cc  SharedConfigFilePath
//   .pagespeed-serve-stats           serve_stats.cc    ServeStatsPath
//   pagespeed-hosts.conf             host_aliases.cc   HostAliasesFilePath
//   pagespeed.json                   config_file.cc    ConfigFilePath
//   pagespeed-webbotauth-keys.conf   webbotauth_warmer.cc  WebBotAuthKeysFilePath
//   pagespeed-rslcap-keys.conf       webbotauth_warmer.cc  RslCapKeysFilePath
// Two more names are recognised but no longer written: pagespeed.license and
// pagespeed.instance-id were authored by 2.0 daemons (license token and
// telemetry instance id, both retired at 2.1).  A 2.1 daemon
// never creates them, but an upgraded install may still carry them, and
// they must keep counting as this daemon's own leftovers rather than as
// foreign content in its cache directory.
// Volume files, the generation file and their siblings are all named after
// the configured stem and are matched by prefix instead — the stem being the
// extension-free basename, since the cache layer inserts
// -<format>-<geometry> before any extension the operator configured.
namespace {

constexpr std::string_view kAuthoredSidecars[] = {
    "pagespeed-shared.conf",
    ".pagespeed-serve-stats",
    "pagespeed.license",      // legacy (2.0), never written by 2.1
    "pagespeed.instance-id",  // legacy (2.0), never written by 2.1
    "pagespeed-hosts.conf",
    "pagespeed.json",
    "pagespeed-webbotauth-keys.conf",
    "pagespeed-rslcap-keys.conf",
};

// AtomicWriteFile stages every sidecar as "<name>.tmp" beside itself.
constexpr std::string_view kTmpSuffix = ".tmp";

}  // namespace

bool IsDaemonAuthoredCacheEntry(const std::string& name,
                                const std::string& volume_stem) {
  const std::string_view entry(name);
  // Volume files (<stem>-<format>-<geometry>), the generation file
  // (<stem>.gen) and any temporary staged beside them.
  if (!volume_stem.empty() && entry.starts_with(volume_stem)) {
    return true;
  }
  for (std::string_view sidecar : kAuthoredSidecars) {
    if (entry == sidecar) {
      return true;
    }
    if (entry.size() == sidecar.size() + kTmpSuffix.size() &&
        entry.starts_with(sidecar) && entry.ends_with(kTmpSuffix)) {
      return true;
    }
  }
  return false;
}

CacheDirStatus ValidateCacheDir(const std::string& dir,
                                const std::string& volume_stem,
                                std::string* detail) {
#ifdef _WIN32
  (void)dir;
  (void)volume_stem;
  (void)detail;
  return CacheDirStatus::kOk;
#else
  struct stat st;
  if (::stat(dir.c_str(), &st) != 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return (errno == ENOENT) ? CacheDirStatus::kMissing
                             : CacheDirStatus::kStatFailed;
  }
  if (!S_ISDIR(st.st_mode)) {
    if (detail != nullptr) {
      *detail = "path exists but is not a directory";
    }
    return CacheDirStatus::kNotADirectory;
  }
  // Writability from this process's EFFECTIVE ids (AT_EACCESS) — access(2)
  // would answer for the real uid, which is the wrong question the moment
  // the two ever differ.
  if (::faccessat(AT_FDCWD, dir.c_str(), W_OK | X_OK, AT_EACCESS) != 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return (errno == EACCES) ? CacheDirStatus::kNotWritable
                             : CacheDirStatus::kStatFailed;
  }
  // Every DAEMON-AUTHORED entry in the directory must be owned by this euid.
  // The daemon writes, unlinks and recreates those files; one it does not own
  // is one it cannot safely manage — and chowning it is forbidden (no
  // install or startup path ever modifies ownership).
  //
  // The scan is deliberately NOT directory-wide.  The cache directory is
  // group-writable by design (the web-server peers share the daemon's group),
  // so a directory-wide rule would let any group member permanently refuse
  // the daemon's start by creating a single file, and would also refuse on a
  // dedicated mount's root-owned `lost+found` or on any unrelated file an
  // operator parked in a hand-configured --cache-path directory.  Foreign
  // files the daemon never touches are not its problem; foreign files it
  // would try to rewrite are.
  const uid_t self = ::geteuid();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return (errno == EACCES) ? CacheDirStatus::kNotWritable
                             : CacheDirStatus::kStatFailed;
  }
  CacheDirStatus status = CacheDirStatus::kOk;
  struct dirent* ent;
  while ((ent = ::readdir(d)) != nullptr) {
    if (std::strcmp(ent->d_name, ".") == 0 ||
        std::strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    if (!IsDaemonAuthoredCacheEntry(ent->d_name, volume_stem)) {
      continue;
    }
    std::string child = dir + "/" + ent->d_name;
    struct stat cst;
    if (::lstat(child.c_str(), &cst) != 0) {
      if (detail != nullptr) {
        *detail = child + ": lstat: " + std::strerror(errno);
      }
      status = CacheDirStatus::kStatFailed;
      break;
    }
    if (cst.st_uid != self) {
      if (detail != nullptr) {
        *detail = child + " owned by uid " + std::to_string(cst.st_uid) +
                  ", daemon runs as uid " + std::to_string(self) +
                  " (this is a file the daemon itself writes)";
      }
      status = CacheDirStatus::kForeignOwned;
      break;
    }
  }
  ::closedir(d);
  return status;
#endif
}

bool RefuseCacheDir(CacheDirStatus status, const std::string& dir,
                    const std::string& detail, MessageHandler* handler) {
  if (status == CacheDirStatus::kOk) {
    return true;
  }
  static ConsoleMessageHandler console;
  MessageHandler& h = (handler != nullptr) ? *handler : console;
  switch (status) {
    case CacheDirStatus::kMissing:
      h.Error(
          "Cache directory %s does not exist. The packaged layout creates "
          "it via tmpfiles.d (pagespeed-optimizer.conf) as mode 3770 "
          "pagespeed:pagespeed; create it that way or run systemd-tmpfiles "
          "--create pagespeed-optimizer.conf. Refusing to start rather than "
          "running with a silent cache-off fallback.",
          dir.c_str());
      break;
    case CacheDirStatus::kNotADirectory:
      h.Error("Cache directory %s: %s. Refusing to start.", dir.c_str(),
              detail.c_str());
      break;
    case CacheDirStatus::kNotWritable:
      h.Error(
          "Cache directory %s is not writable by this daemon (permission "
          "denied: %s). The daemon runs unprivileged and needs write access; "
          "fix ownership/mode on the directory (packaged default: 3770 "
          "pagespeed:pagespeed via tmpfiles.d). Refusing to start.",
          dir.c_str(), detail.c_str());
      break;
    case CacheDirStatus::kForeignOwned:
      h.Error(
          "Cache directory %s contains content this daemon cannot own: %s. "
          "No content is ever chowned or migrated at startup; a "
          "pre-existing root-owned cache is abandoned in place and the "
          "daemon cold-starts into a fresh, owned directory. Move the "
          "foreign-owned content out of %s (or remove it) and restart. See "
          "the migration notes in "
          "https://modpagespeed.com/docs/deployment/. Refusing to start.",
          dir.c_str(), detail.c_str(), dir.c_str());
      break;
    case CacheDirStatus::kStatFailed:
      h.Error(
          "Cache directory %s could not be inspected: %s. Refusing to "
          "start rather than guessing.",
          dir.c_str(), detail.c_str());
      break;
    case CacheDirStatus::kOk:
      break;  // Unreachable; handled above.
  }
  return false;
}

}  // namespace pagespeed
