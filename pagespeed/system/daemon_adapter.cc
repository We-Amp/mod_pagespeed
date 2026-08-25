/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/system/daemon_adapter.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"

#ifndef _WIN32
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace net_instaweb {

namespace {

// Conditions already announced in this process.
//
// Process-wide, not per-adapter: a server re-reads its configuration during
// startup and rebuilds its server contexts, so a per-object latch delivers one
// line per rebuild while promising one line per condition.  Keyed on the
// message so a second virtual host failing a DIFFERENT way is still heard --
// the same reasoning the daemon's own version-skew reporting uses.
struct Announcements {
  std::mutex mu;
  std::set<GoogleString> seen;
};

Announcements* announcements() {
  // Allocated once and never freed, deliberately: this is the no-destructor
  // idiom, not an oversight. The object has to outlive every caller, and
  // giving it a destructor would schedule one to run during static
  // destruction, where a late StartupCheck would touch a destroyed mutex.
  //
  // It is also not a leak in any tool's sense -- the pointer lives in static
  // storage, so it stays reachable for the life of the process and leak
  // checkers treat it as a root rather than as lost memory.
  static Announcements* a = new Announcements;
  return a;
}

bool ShouldAnnounce(const GoogleString& message) {
  Announcements* a = announcements();
  std::lock_guard<std::mutex> lock(a->mu);
  return a->seen.insert(message).second;
}

// True iff `name` looks like a daemon volume file for `stem`: the stem, then
// "-<digits>", then the rest.  The digit is what keeps an unrelated sibling
// that merely starts with the stem out of the count.
//
// One wrinkle: the cache layer derives the physical volume name by INSERTING
// "-<generation>-<geometry>" before the stem's extension, so a stem of
// "cache.vol" yields "cache-6-<hash>.vol", never "cache.vol-6-<hash>".  An
// extensioned stem is therefore matched as <base> + "-<digit>" + ... + <ext>;
// matching it by plain prefix would find nothing, and the caller would read a
// running, healthy daemon as "volume does not exist yet".
//
// COUPLED TO THE DAEMON'S NAMING, and one exclusion is load-bearing: the
// cache layer can place a small-object companion beside the volume under the
// same stem with a ".small" suffix.  That is part of ONE cache, not a second
// one, so counting it would read a healthy directory as a split.  It is inert
// in shipping configurations today (the small tier is off), which is exactly
// why it has to be handled here rather than discovered later.
bool IsVolumeNameFor(StringPiece name, StringPiece stem) {
  if (StringCaseEndsWith(name, ".small")) {
    return false;
  }
  // Split the stem at its last dot, mirroring the cache layer's
  // std::filesystem split: for an extensioned stem the daemon inserts
  // "-<generation>-<geometry>" before the extension, so the base is the
  // prefix to match and the extension is a required suffix.  The two dot
  // edges differ, and both must agree with the daemon's side:
  //  - a LEADING dot is not an extension on either side (".vol" has stem
  //    ".vol" and no extension, so the daemon appends: ".vol-<maj>-<hash>");
  //  - a TRAILING dot IS one ("cache." splits as stem "cache" plus the
  //    bare-dot extension ".", so the daemon writes "cache-<maj>-<hash>."
  //    and the matched name must END with that dot).  Reading a trailing
  //    dot as extensionless matches the whole "cache." as the prefix -- a
  //    name the daemon never writes, leaving its volume unfindable.
  // An extensionless stem matches by prefix alone.
  StringPiece prefix = stem;
  StringPiece extension;
  const size_t dot = stem.find_last_of('.');
  if (dot != StringPiece::npos && dot > 0) {
    prefix = stem.substr(0, dot);
    extension = stem.substr(dot);
  }
  if (!StringCaseStartsWith(name, prefix)) {
    return false;
  }
  if (!extension.empty() && !StringCaseEndsWith(name, extension)) {
    return false;
  }
  if (name.size() < prefix.size() + extension.size() + 2) {
    return false;
  }
  if (name[prefix.size()] != '-') {
    return false;
  }
  const char c = name[prefix.size() + 1];
  return c >= '0' && c <= '9';
}

#ifndef _WIN32
void ScanDirectory(const GoogleString& dir, StringPiece stem,
                   std::vector<GoogleString>* out) {
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) {
    return;
  }
  while (struct dirent* e = readdir(d)) {
    const StringPiece name(e->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (!stem.empty() && !IsVolumeNameFor(name, stem)) {
      continue;
    }
    GoogleString full = StrCat(dir, "/", name);
    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      out->push_back(name.as_string());
    }
  }
  closedir(d);
}
#endif

}  // namespace

DaemonAdapter::DaemonAdapter(StringPiece socket_path, StringPiece volume_path,
                             MessageHandler* handler)
    : socket_path_(socket_path.data(), socket_path.size()),
      volume_path_(volume_path.data(), volume_path.size()),
      library_path_(kDefaultDaemonLibraryName),
      handler_(handler),
      abi_loader_(&LoadDaemonAbi) {}

DaemonAdapter::~DaemonAdapter() {
  if (record_cache_ != nullptr && abi_ != nullptr) {
    abi_->CacheClose(record_cache_);
    record_cache_ = nullptr;
  }
}

void* DaemonAdapter::RecordCache() {
  if (health_ != DaemonHealth::kReady || abi_ == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(record_cache_mutex_);
  if (record_cache_attempted_) {
    return record_cache_;
  }
  // Attempted, whatever happens next.  A volume that will not open is not
  // going to start opening on the next request, and retrying per request
  // would turn one failure into one failure per request.
  record_cache_attempted_ = true;

  alignas(std::max_align_t) unsigned char
      config_storage[kCacheConfigProbeBytes] = {};
  PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(config_storage);
  abi_->CacheConfigInit(config);
  if (!ApplyInheritedSizing(config, inherited_volume_size_)) {
    return nullptr;
  }
  config->volume_path = volume_path_.c_str();

  void* cache = nullptr;
  const int open_error = abi_->CacheOpen(config, &cache);
  if (open_error != kPsOk) {
    if (handler_ != nullptr) {
      const GoogleString message = StrCat(
          "nothing will be recorded for in-place optimization: cannot open "
          "the optimizer daemon's cache volume at ",
          volume_path_, ": ", abi_->StrError(open_error));
      if (ShouldAnnounce(message)) {
        handler_->Message(kError, "%s", message.c_str());
      }
    }
    return nullptr;
  }
  record_cache_ = cache;
  return record_cache_;
}

void DaemonAdapter::ResetAnnouncementsForTesting() {
  Announcements* a = announcements();
  std::lock_guard<std::mutex> lock(a->mu);
  a->seen.clear();
}

bool DaemonAdapter::ApplyInheritedSizing(PsCacheConfig* config,
                                         uint64_t inherited_size) {
  // Unconditional, and before any early return: no path out of this function
  // may leave a RAM tier this module did not choose.
  config->ram_cache_size = kMirroredRamCacheSizeBytes;
  if (inherited_size == 0) {
    // Not a size.  Substituting one here is exactly the defect this whole
    // mechanism exists to prevent, so it is refused rather than defaulted.
    return false;
  }
  config->volume_size = inherited_size;
  return true;
}

std::vector<GoogleString> DaemonAdapter::VolumeFiles(StringPiece volume_path) {
  std::vector<GoogleString> out;
#ifndef _WIN32
  const GoogleString path(volume_path.data(), volume_path.size());
  if (path.empty()) {
    return out;
  }
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    // The configured path is a directory; the volume lives inside it.
    ScanDirectory(path, StringPiece(), &out);
  }
  // ... and/or the configured path is a stem, with the volume beside it.
  const size_t slash = path.find_last_of('/');
  const GoogleString parent =
      slash == GoogleString::npos ? GoogleString(".") : path.substr(0, slash);
  const GoogleString stem =
      slash == GoogleString::npos ? path : path.substr(slash + 1);
  if (!stem.empty()) {
    ScanDirectory(parent, stem, &out);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
#endif
  return out;
}

bool DaemonAdapter::SocketAnswers(StringPiece path, GoogleString* error) {
#ifdef _WIN32
  StrAppend(error, "socket probing is not implemented on this platform (", path,
            ")");
  return false;
#else
  const GoogleString spelled(path.data(), path.size());
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (spelled.size() >= sizeof(address.sun_path)) {
    StrAppend(error, "the daemon socket path is too long for this platform: ",
              spelled);
    return false;
  }
  memcpy(address.sun_path, spelled.data(), spelled.size());

  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    StrAppend(error, "cannot create a socket to reach the optimizer daemon: ",
              strerror(errno));
    return false;
  }
  const bool connected =
      connect(fd, reinterpret_cast<struct sockaddr*>(&address),
              sizeof(address)) == 0;
  if (!connected) {
    StrAppend(error, "the optimizer daemon does not answer at ", spelled, ": ",
              strerror(errno));
  }
  close(fd);
  return connected;
#endif
}

DaemonStartupStatus DaemonAdapter::Resolve(GoogleString* error) {
  health_ = DaemonHealth::kUnavailable;
  extra_volume_warning_.clear();

  if (socket_path_.empty() && volume_path_.empty()) {
    health_ = DaemonHealth::kNotConfigured;
    return DaemonStartupStatus::kOk;
  }
  if (!configured()) {
    // Half a configuration is a configuration mistake, and a loud one already
    // — the operator sees this the first time they start.  Deliberately not a
    // refusal: refusing belongs to the failures that are silent.
    StrAppend(error,
              "in-place optimization is OFF: both the daemon socket path and "
              "the daemon volume path must be set, and only one of them is");
    return DaemonStartupStatus::kOk;
  }

  GoogleString why;
  abi_.reset(abi_loader_(library_path_, &why));
  if (abi_ == nullptr) {
    StrAppend(error, "in-place optimization is OFF: ", why);
    return DaemonStartupStatus::kOk;
  }

  if (!abi_->PublishesVolumeSize()) {
    // An older daemon package: it cannot tell us the size it opened its volume
    // with, so the mandatory mirror cannot be evaluated at all.  Degrade — an
    // out-of-step package pair is not a reason to refuse a start — and, above
    // all, do not open anything.  Guessing the size here is the split-brain.
    StrAppend(error,
              "in-place optimization is OFF: the installed optimizer daemon "
              "does not publish the size of its cache volume, so this module "
              "cannot open that volume without risking a second, separate "
              "cache. Install a daemon package matching this module.");
    return DaemonStartupStatus::kOk;
  }

  const uint64_t inherited = abi_->SharedConfigVolumeSize(volume_path_.c_str());
  inherited_volume_size_ = inherited;
  if (inherited == 0) {
    // Either the daemon has never run (no shared config to read), or its
    // config is unreadable, or it declares a schema this build refuses.  All
    // of them mean the same thing here: the size is not known, so the volume
    // is not opened.
    StrAppend(error,
              "in-place optimization is OFF: the optimizer daemon has not "
              "published the size of its cache volume at ",
              volume_path_,
              " (it may not be running yet). This module will not open that "
              "volume without knowing the size the daemon uses.");
    return DaemonStartupStatus::kOk;
  }

  const std::vector<GoogleString> before = VolumeFiles(volume_path_);
  if (before.empty()) {
    // The daemon published a size but its volume file is not there.  Creating
    // it is exactly what must not happen: the daemon is the volume's owner and
    // a volume we invent is one nothing else reads.
    StrAppend(error,
              "in-place optimization is OFF: the optimizer daemon's cache "
              "volume does not exist yet at ",
              volume_path_,
              ". This module never creates it — start the daemon first.");
    return DaemonStartupStatus::kOk;
  }
  if (before.size() > 1) {
    // Several volume files is the EXPECTED steady state after an operator
    // changes the daemon's cache size: the filename encodes the geometry and
    // the daemon does not remove the file it used to use, so the old one is
    // simply left behind.  Refusing to start here would turn a routine resize
    // into a server that will not come up.
    //
    // It is also not the failure this check exists for.  That failure is
    // SILENT -- a module quietly using a different file from the daemon --
    // and this state is neither silent nor ambiguous: the daemon published
    // which size it is using, so the inherited size names exactly one of
    // these files, and the attach check below proves we landed on it.  So:
    // say something an operator can act on, and carry on.
    extra_volume_warning_ = StrCat(
        "the optimizer daemon's cache directory holds more than one cache "
        "volume (",
        JoinCollection(before, ", "),
        "). Only the one matching the size the daemon publishes is in use; "
        "the others are left over from an earlier cache size and are just "
        "occupying disk. They can be removed while the daemon is stopped.");
  }

  // Only now, with a known size and exactly one volume on disk, is it safe to
  // open — and even then the result is CHECKED rather than assumed.
  alignas(std::max_align_t) unsigned char
      config_storage[kCacheConfigProbeBytes] = {};
  PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(config_storage);
  abi_->CacheConfigInit(config);
  if (!ApplyInheritedSizing(config, inherited)) {
    StrAppend(error,
              "in-place optimization is OFF: the optimizer daemon reported an "
              "unusable cache volume size");
    return DaemonStartupStatus::kOk;
  }
  config->volume_path = volume_path_.c_str();

  void* cache = nullptr;
  const int open_error = abi_->CacheOpen(config, &cache);
  if (open_error != kPsOk) {
    StrAppend(error,
              "in-place optimization is OFF: cannot open the optimizer "
              "daemon's cache volume at ",
              volume_path_, ": ", abi_->StrError(open_error));
    return DaemonStartupStatus::kOk;
  }
  // The probe is all this check needs the volume for.  Holding it open across
  // the server's fork into worker processes would hand every child a handle it
  // never asked for.
  abi_->CacheClose(cache);

  // Did opening at the inherited size land on the daemon's file, or make a new
  // one?  This is the mirror.  It catches a size that disagrees with the
  // volume actually on disk, and it also catches any OTHER geometry input
  // diverging — which matters, because the size is the only one of them the
  // daemon publishes.
  const std::vector<GoogleString> after = VolumeFiles(volume_path_);
  if (after.size() > before.size()) {
    std::vector<GoogleString> created;
    for (const GoogleString& name : after) {
      if (std::find(before.begin(), before.end(), name) == before.end()) {
        created.push_back(name);
      }
    }
    StrAppend(error,
              "refusing to start: opening the optimizer daemon's cache volume "
              "at the size the daemon published (",
              Integer64ToString(static_cast<int64>(inherited)),
              " bytes) created a SECOND volume file (",
              JoinCollection(created, ", "), ") beside the daemon's (",
              JoinCollection(before, ", "),
              ") instead of attaching to it. The two would share nothing and "
              "each run a permanently cold cache, with no error to show for "
              "it. Remove the file this start just created, then install a "
              "module and daemon package pair that agree.");
    return DaemonStartupStatus::kRefuseToStart;
  }

  if (!SocketAnswers(socket_path_, &why)) {
    StrAppend(error, "in-place optimization is OFF: ", why);
    return DaemonStartupStatus::kOk;
  }

  health_ = DaemonHealth::kReady;
  if (!extra_volume_warning_.empty()) {
    // Reported even though the server is healthy: nothing else will ever
    // mention the wasted disk, and the same latch keeps it to one line.
    StrAppend(error, extra_volume_warning_);
  }
  return DaemonStartupStatus::kOk;
}

DaemonStartupStatus DaemonAdapter::StartupCheck() {
  GoogleString error;
  const DaemonStartupStatus status = Resolve(&error);
  if (!error.empty() && handler_ != nullptr && ShouldAnnounce(error)) {
    handler_->Message(kError, "%s", error.c_str());
  }
  return status;
}

}  // namespace net_instaweb
