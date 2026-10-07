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
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
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
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#else
#include <windows.h>
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

// Start-up refusals already repeated in this process (see
// DaemonAdapter::ReannounceStartupRefusal).  A latch of its own: a child
// process inherits the one above across fork, and in the parent it already
// holds the text, so reusing it would silence every repeat.  Never freed,
// for the same reason as announcements().
Announcements* reannouncements() {
  static Announcements* a = new Announcements;
  return a;
}

bool ShouldReannounce(const GoogleString& message) {
  Announcements* a = reannouncements();
  std::lock_guard<std::mutex> lock(a->mu);
  return a->seen.insert(message).second;
}

// The user id the owner guard compares against.  A seam so a test running
// as an ordinary user can stage a file it owns as "another user's" (only
// root can chown); production always asks the kernel.  The setter must
// not be called once a startup check has begun: the guard reads the
// pointer without synchronization.
#ifndef _WIN32
using EffectiveUidFn = uid_t (*)();
EffectiveUidFn g_effective_uid_for_testing = &geteuid;

uid_t EffectiveUid() { return g_effective_uid_for_testing(); }
#endif

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

// The reachability probe's deadline: the probe runs while the server
// starts, and a daemon that exists but is not accepting (stopped, or its
// accept queue full) must never hang a start.
constexpr int64_t kSocketConnectDeadlineMs = 2000;
// How often a backlog-full connect (EAGAIN) is retried inside that deadline.
constexpr int64_t kSocketConnectRetryMs = 50;
// The deadline message below says "2 seconds"; keep the two in step.
static_assert(kSocketConnectDeadlineMs == 2000, "message says two seconds");
// How long a probe that reached the deadline is remembered for its path:
// long enough to cover one start's pass over all servers, short enough that
// a later graceful restart or reload in the same process re-probes a daemon
// that has since recovered.  A daemon that starts accepting inside this
// window is not noticed by the checks made inside it: the startup verdict
// is final for the process.
constexpr int64_t kSocketVerdictLatchMs = 10000;

// The deadline message for a probe that did not get accepted in time,
// produced in exactly one place so the real timed-out probe and the latched
// verdict report byte-identically.
void AppendDaemonDeadlineMessage(GoogleString* error,
                                 const GoogleString& spelled) {
  StrAppend(error, "the optimizer daemon did not accept a connection at ",
            spelled, " within ", kSocketConnectDeadlineMs / 1000,
            " seconds -- it may be stopped or overloaded.");
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

// Whether `dir` can be looked inside, and if not, WHY.  opendir, not stat:
// listing the directory is exactly what the volume scan above does, so its
// failure mode is the one worth reporting -- and "permission denied" must not
// read as "the daemon is not there" (see VolumeDirAccess).
DaemonAdapter::DirAccess ProbeDir(const GoogleString& dir) {
  DIR* d = opendir(dir.c_str());
  if (d != nullptr) {
    closedir(d);
    return DaemonAdapter::DirAccess::kReadable;
  }
  return (errno == EACCES || errno == EPERM)
             ? DaemonAdapter::DirAccess::kDenied
             : DaemonAdapter::DirAccess::kAbsent;
}
#endif

#ifdef _WIN32

// UTF-8 at the edge, wide at the API: the adapter's paths are UTF-8
// everywhere else in this tree, so the conversion happens here and nowhere
// else.  Invalid UTF-8 is refused (the callers treat that as an absent
// directory or an empty scan), never crashed on.
bool Utf8ToWide(const GoogleString& utf8, std::wstring* out) {
  if (utf8.empty()) {
    return false;
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                          static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) {
    return false;
  }
  out->assign(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                      static_cast<int>(utf8.size()), &(*out)[0], length);
  return true;
}

bool WideToUtf8(const std::wstring& wide, GoogleString* out) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
  if (length <= 0) {
    return false;
  }
  out->resize(length);
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      &(*out)[0], length, nullptr, nullptr);
  return true;
}

// GetLastError() rendered as UTF-8 text, with the trailing line break
// FormatMessage appends trimmed -- the Windows spelling of the POSIX
// probe's strerror clauses.
GoogleString WindowsErrorText(DWORD code) {
  wchar_t* message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  GoogleString out;
  if (length != 0 && message != nullptr) {
    std::wstring trimmed(message, length);
    while (!trimmed.empty() &&
           (trimmed.back() == L'\r' || trimmed.back() == L'\n' ||
            trimmed.back() == L' ')) {
      trimmed.pop_back();
    }
    WideToUtf8(trimmed, &out);
  }
  if (message != nullptr) {
    LocalFree(message);
  }
  if (out.empty()) {
    out = "error " + Integer64ToString(static_cast<int64_t>(code));
  }
  return out;
}

// The pattern that lists `wide_dir`.  A directory that already ends in a
// separator -- the root of the current drive, "\\" -- takes the bare
// wildcard: "\\" + "\\*" would spell a UNC prefix, not that root.
std::wstring ListingPattern(const std::wstring& wide_dir) {
  if (!wide_dir.empty() &&
      (wide_dir.back() == L'\\' || wide_dir.back() == L'/')) {
    return wide_dir + L"*";
  }
  return wide_dir + L"\\*";
}

// The Windows spelling of the POSIX scan above, same contract: every regular
// file in `dir` whose name matches the stem (or every regular file, for an
// empty stem), as names, sorted and deduplicated by the caller.
void ScanDirectory(const GoogleString& dir, StringPiece stem,
                   std::vector<GoogleString>* out) {
  std::wstring wide_dir;
  if (!Utf8ToWide(dir, &wide_dir)) {
    return;
  }
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(ListingPattern(wide_dir).c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (wcscmp(data.cFileName, L".") == 0 ||
        wcscmp(data.cFileName, L"..") == 0) {
      continue;
    }
    // A directory is not a volume file, whatever its name: a directory named
    // like one beside the stem must not be counted.
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    GoogleString name;
    // A name that does not survive the UTF-8 round trip cannot match a
    // UTF-8 stem and is skipped rather than mangled.
    if (!WideToUtf8(data.cFileName, &name)) {
      continue;
    }
    if (!stem.empty() && !IsVolumeNameFor(name, stem)) {
      continue;
    }
    out->push_back(name);
  } while (FindNextFileW(find, &data));
  FindClose(find);
}

// The Windows spelling of ProbeDir, same contract: listing the directory is
// exactly what the volume scan does, so its failure mode is the one worth
// reporting.  Access denied is distinguished from every other failure because
// it names a different fix (see the remediation hint below).
DaemonAdapter::DirAccess ProbeDir(const GoogleString& dir) {
  std::wstring wide_dir;
  if (!Utf8ToWide(dir, &wide_dir)) {
    return DaemonAdapter::DirAccess::kAbsent;
  }
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(ListingPattern(wide_dir).c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    FindClose(find);
    return DaemonAdapter::DirAccess::kReadable;
  }
  return GetLastError() == ERROR_ACCESS_DENIED
             ? DaemonAdapter::DirAccess::kDenied
             : DaemonAdapter::DirAccess::kAbsent;
}
#endif  // _WIN32

// The remediation line for the failure H2/H3 make the most likely, per
// platform.  POSIX: after the daemon's privilege drop its cache directory,
// volume, socket and shared config are group-rw (0660/0640
// pagespeed:pagespeed), and a web-server user outside the `pagespeed` group
// gets EACCES where a pre-drop deployment got in.  Windows: the group model
// is not the mechanism, an access grant on the cache directory is, so the
// sentence names that instead.  Named in every permission-denied degrade so
// the fix is one command.
#ifdef _WIN32
const char kGroupMembershipHint[] =
    " The web server's worker identity probably has no access grant on the "
    "optimizer daemon's cache directory. Grant it Modify on that directory, "
    "inherited by the files below (for the built-in application-pool "
    "identities, `icacls \"<cache directory>\" /grant "
    "\"IIS_IUSRS:(OI)(CI)M\"`), and restart the web server.";
#else
const char kGroupMembershipHint[] =
    " The web-server user is probably not in the `pagespeed` group, which "
    "owns the optimizer daemon's cache and socket since the daemon's "
    "privilege drop. Add it (for example `usermod -a -G pagespeed www-data`, "
    "or `apache`/`nginx` on an RPM host) and restart the web server.";
#endif

// Removes one volume-file name a VolumeFiles scan reported for
// `volume_path`, from the directory that name lives in.  The scan reads two
// shapes -- the configured path as a directory, and a stem with the file
// beside it -- but only the STEM shape removes: there the scan itself
// filtered the names (`IsVolumeNameFor`), so the removal touches exactly
// the family the scan counted.  In the directory shape the scan counts
// every regular file inside, no family can be named for a name that
// appeared, and this returns false so the caller reports the manual step.
// Returns true when the name is gone from the one place the removal may
// look; false with `why` carrying the OS error, or the owner refusal below,
// from the path where the file still is.
//
// The name was watched APPEAR between the check's two scans -- whoever made
// it.  Most of the time that is this open; it can also be the optimizer
// daemon's own new volume, which comes to exist in exactly that window
// whenever the daemon begins running onto a volume that does not exist yet.
// POSIX refuses to unlink a file another user owns (the daemon runs under
// its own account): the start still refuses, and `why` says the file is
// probably the daemon's.  Windows has no owner check in this package --
// there the delete simply fails while the daemon holds the file open, and
// the caller's message covers both cases.
// How one removal attempt ended.  The caller composes its message from
// this, because the two failure kinds need OPPOSITE advice: a name this
// module could not delete for its own reasons is safe for an operator to
// remove by hand, while a name held by another user most likely IS the
// optimizer's live volume and must be left alone.
enum class RemoveOutcome : std::uint8_t {
  kRemoved,         // gone, by this call or already
  kFailed,          // this module's own failure; manual removal is safe
  kHeldByOtherUser  // POSIX: another user owns it (or Windows: held open);
                    // leave it in place
};

RemoveOutcome RemoveVolumeFile(const GoogleString& volume_path,
                               const GoogleString& name, GoogleString* why) {
#ifdef _WIN32
  const size_t slash = volume_path.find_last_of("/\\");
#else
  const size_t slash = volume_path.find_last_of('/');
#endif
  const GoogleString stem =
      slash == GoogleString::npos ? volume_path : volume_path.substr(slash + 1);
  if (stem.empty() || !IsVolumeNameFor(name, stem)) {
    // Not a name the stem-shape scan would have counted (a
    // directory-shaped configured path counts EVERY regular file inside,
    // so a name that appeared there can be anything at all): nothing this
    // removal can safely point at.  The re-scan in the caller is the
    // authority on whether the name is gone.
    StrAppend(why, volume_path,
              " is a directory, and this start does not remove files "
              "inside a directory-shaped volume path: ",
              volume_path,
#ifdef _WIN32
              "\\",
#else
              "/",
#endif
              name,
              " was left in place; remove it by hand only if it is not "
              "the optimizer daemon's own volume");
    return RemoveOutcome::kFailed;
  }
  GoogleString parent;
  if (slash == 0) {
#ifdef _WIN32
    parent = "\\";
#else
    parent = "/";
#endif
  } else if (slash != GoogleString::npos) {
    parent = volume_path.substr(0, slash);
  } else {
    parent = ".";
  }
  const GoogleString full =
      (!parent.empty() && (parent.back() == '/' || parent.back() == '\\'))
          ? StrCat(parent, name)
          : StrCat(parent,
#ifdef _WIN32
                   "\\",
#else
                   "/",
#endif
                   name);
#ifdef _WIN32
  std::wstring wide;
  if (!Utf8ToWide(full, &wide)) {
    StrAppend(why, full, " is not a path this module can spell");
    return RemoveOutcome::kFailed;
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return RemoveOutcome::kRemoved;  // no file of this name here
  }
  if (DeleteFileW(wide.c_str())) {
    return RemoveOutcome::kRemoved;
  }
  const DWORD error = GetLastError();
  GoogleString os_text = WindowsErrorText(error);
  while (!os_text.empty() && os_text.back() == '.') {
    os_text.pop_back();
  }
  if (error == ERROR_SHARING_VIOLATION) {
    // A sharing violation on a file this module has itself closed almost
    // certainly means the optimizer holds it: its live volume, not ours.
    // Same advice as the POSIX owner refusal -- leave it in place.
    StrAppend(why, full,
              " was left in place: another process holds it open, most "
              "likely the optimizer daemon's new volume");
    return RemoveOutcome::kHeldByOtherUser;
  }
  StrAppend(why, full, ": ", os_text);
  return RemoveOutcome::kFailed;
#else
  struct stat st;
  if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return RemoveOutcome::kRemoved;  // no file of this name here
  }
  if (st.st_uid != EffectiveUid()) {
    // The daemon's files belong to its own account and the module's to
    // whoever started the web server, so the owner is the one fact that
    // separates them.  (Filesystems where ownership is not the creator's
    // -- NFS with root_squash, vfat/CIFS mounted with uid= -- can make
    // this refuse the module's own file; unusual for a mapped cache
    // volume, and the cost is one left-in-place file, not a deleted
    // volume.)
    StrAppend(why, full,
              " was left in place: it is owned by another user, most "
              "likely the optimizer daemon's new volume");
    return RemoveOutcome::kHeldByOtherUser;
  }
  if (unlink(full.c_str()) == 0) {
    return RemoveOutcome::kRemoved;
  }
  StrAppend(why, full, ": ", strerror(errno));
  return RemoveOutcome::kFailed;
#endif
}

// The cache format number in a volume file's name: the digits that follow
// "<stem>-" (see IsVolumeNameFor for the shape).  -1 for a name with none.
int VolumeFormatOf(StringPiece name, StringPiece stem) {
  StringPiece prefix = stem;
  const size_t dot = stem.find_last_of('.');
  if (dot != StringPiece::npos && dot > 0) {
    prefix = stem.substr(0, dot);
  }
  int format = -1;
  for (size_t i = prefix.size() + 1; i < name.size(); ++i) {
    const char c = name[i];
    if (c < '0' || c > '9' || format > 100000) {
      break;
    }
    format = (format < 0 ? 0 : format * 10) + (c - '0');
  }
  return format;
}

// Of the volume files a scan reported, the ones in the CURRENT cache
// format: those whose name carries the highest format number present.  The
// daemon removes its own format's files when it purges and deliberately
// keeps a file written in an earlier format (the operator's way back to the
// earlier build), so a file with a lower number is never the daemon's
// volume and never a candidate.
std::vector<GoogleString> CurrentFormatVolumeFiles(
    const GoogleString& volume_path, const std::vector<GoogleString>& files) {
#ifdef _WIN32
  const size_t slash = volume_path.find_last_of("/\\");
#else
  const size_t slash = volume_path.find_last_of('/');
#endif
  const GoogleString stem =
      slash == GoogleString::npos ? volume_path : volume_path.substr(slash + 1);
  int highest = -1;
  for (const GoogleString& name : files) {
    highest = std::max(highest, VolumeFormatOf(name, stem));
  }
  std::vector<GoogleString> current;
  for (const GoogleString& name : files) {
    if (VolumeFormatOf(name, stem) == highest) {
      current.push_back(name);
    }
  }
  return current;
}

// What makes a volume file THIS file rather than another one under the same
// name: the device and file numbers.  A file that was deleted and created
// again keeps its name and changes these.
struct VolumeFileIdentity {
  uint64_t device = 0;
  uint64_t file = 0;
  bool operator==(const VolumeFileIdentity& other) const {
    return device == other.device && file == other.file;
  }
};

// Whether the configured volume path is itself a directory.
bool VolumePathIsDirectory(const GoogleString& volume_path) {
#ifdef _WIN32
  std::wstring wide;
  if (!Utf8ToWide(volume_path, &wide)) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  return stat(volume_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// The identity of the volume file `name` that a VolumeFiles scan reported
// beside the stem `volume_path`.  False when there is no regular file of
// that name there, or it cannot be looked at.  Looks; changes nothing.
bool ReadVolumeFileIdentity(const GoogleString& volume_path,
                            const GoogleString& name,
                            VolumeFileIdentity* identity) {
#ifdef _WIN32
  const size_t slash = volume_path.find_last_of("/\\");
  const GoogleString full =
      slash == GoogleString::npos
          ? name
          : StrCat(volume_path.substr(0, slash + 1), name);
  std::wstring wide;
  if (!Utf8ToWide(full, &wide)) {
    return false;
  }
  // No access requested and every kind of sharing allowed: this must not
  // get in the way of the daemon deleting or renaming the file.
  HANDLE handle = CreateFileW(
      wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  BY_HANDLE_FILE_INFORMATION info;
  const bool ok = GetFileInformationByHandle(handle, &info) != 0 &&
                  (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  CloseHandle(handle);
  if (!ok) {
    return false;
  }
  identity->device = info.dwVolumeSerialNumber;
  identity->file = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
                   static_cast<uint64_t>(info.nFileIndexLow);
  return true;
#else
  const size_t slash = volume_path.find_last_of('/');
  const GoogleString full =
      slash == GoogleString::npos
          ? name
          : StrCat(volume_path.substr(0, slash + 1), name);
  struct stat st;
  if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  identity->device = static_cast<uint64_t>(st.st_dev);
  identity->file = static_cast<uint64_t>(st.st_ino);
  return true;
#endif
}

// Probe verdicts already reached in this process, keyed by the socket path
// exactly as configured: a probe that reached the deadline is remembered
// with a monotonic expiry.  Process-wide, not per-adapter, for the same
// reason as the announcement latch above: one start's pass over all servers
// pays the deadline once per path, and after the expiry the next probe is a
// real probe again.  Only the timed-out outcome is latched -- every
// immediate outcome already costs nothing, and caching it would only create
// staleness.
struct SocketVerdicts {
  std::mutex mu;
  std::map<GoogleString, int64_t> deadline_expiries_ms;
  int64_t latch_ms = kSocketVerdictLatchMs;
};

SocketVerdicts* socket_verdicts() {
  // Same no-destructor idiom as announcements() above, and for the same
  // reason: the latch has to outlive every caller, and a destructor would
  // run during static destruction, where a late probe would touch a
  // destroyed mutex.  It is also not a leak in any tool's sense.
  static SocketVerdicts* v = new SocketVerdicts;
  return v;
}

// A path that already cost one probe its deadline is answered from the
// latch inside the interval: the verdict and its message are the ones a
// real timed-out probe would produce, and the deadline is not paid twice.
// Shared by both platforms' probes; only the timed-out outcome is latched,
// every immediate outcome still answers at once, every time.  `now_ms` is
// the caller's steady clock, the same clock the deadlines are taken on.
bool SocketDeadlineIsLatched(const GoogleString& spelled, int64_t now_ms) {
  SocketVerdicts* verdicts = socket_verdicts();
  std::lock_guard<std::mutex> lock(verdicts->mu);
  const auto it = verdicts->deadline_expiries_ms.find(spelled);
  if (it == verdicts->deadline_expiries_ms.end()) {
    return false;
  }
  if (it->second > now_ms) {
    return true;
  }
  verdicts->deadline_expiries_ms.erase(it);
  return false;
}

// Records one deadline verdict, reaping expired entries while the lock is
// held anyway so a path that is never probed again does not keep one for
// the life of the process.
void RecordSocketDeadline(const GoogleString& spelled, int64_t now_ms) {
  SocketVerdicts* verdicts = socket_verdicts();
  std::lock_guard<std::mutex> lock(verdicts->mu);
  for (auto it = verdicts->deadline_expiries_ms.begin();
       it != verdicts->deadline_expiries_ms.end();) {
    if (it->second <= now_ms) {
      it = verdicts->deadline_expiries_ms.erase(it);
    } else {
      ++it;
    }
  }
  verdicts->deadline_expiries_ms[spelled] = now_ms + verdicts->latch_ms;
}

// The peer's report of a call that has just failed: the error class it
// returned, plus the sentence the peer kept about THIS failure when the
// installed library publishes one.
//
// CALL IT IMMEDIATELY AFTER THE FAILING CALL and nothing else -- the peer's
// explanation describes the last failure on this thread, so anything that
// re-enters the library first replaces it.  The reason is captured here, into
// a string this module owns, before any of the filesystem checks that decorate
// the same message run.
//
// The two are joined rather than substituted because they answer different
// questions and the field failure needed both: the class is what an operator
// greps for and what the daemon's own documentation is indexed by, while the
// reason is the part that says what to do.  A library too old to publish a
// reason yields exactly the line this module produced before.
GoogleString DescribeDaemonError(const DaemonAbi* abi, int error) {
  // THE REASON IS READ FIRST, before StrError, and copied out of the peer's
  // storage in the same breath.  The rule this function documents is that
  // nothing may re-enter the library between the failure and the read, and a
  // function that broke its own rule would be the one place the ordering is
  // easiest to get wrong later: StrError is a lookup today, but it is the
  // peer's code, and "today it does not touch the buffer" is not a property
  // this side can hold.  Reading first costs nothing and removes the
  // question.
  const char* peer_reason = abi->LastErrorMessage();
  // Copied, not aliased: the peer hands back a pointer into a thread_local
  // buffer it clears on the next ps_cache_open, so the borrowed pointer is
  // only good until this module calls the library again.
  const GoogleString reason(peer_reason == nullptr ? "" : peer_reason);

  GoogleString described(abi->StrError(error));
  // Empty and absent are the same answer -- the peer leaves the buffer empty
  // when it has nothing to say, so a caller that only checked for nullptr
  // would print a bare "()".  And a reason that merely repeats the class is
  // not a second fact; appending it would print the same words twice.
  if (!reason.empty() && described != reason) {
    StrAppend(&described, " (", reason, ")");
  }
  return described;
}

}  // namespace

int64_t DaemonAdapter::MonotonicNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t DaemonAdapter::BackoffMsAfter(int attempts) {
  int64_t backoff = kRecordCacheFirstBackoffMs;
  // Doubling by shifting would need its own overflow argument; multiplying up
  // to the cap and stopping there needs none, and the loop runs at most
  // kRecordCacheMaxAttempts times.
  for (int i = 1; i < attempts && backoff < kRecordCacheMaxBackoffMs; ++i) {
    backoff *= 2;
  }
  return std::min(backoff, kRecordCacheMaxBackoffMs);
}

DaemonAdapter::DaemonAdapter(StringPiece socket_path, StringPiece volume_path,
                             MessageHandler* handler)
    : socket_path_(socket_path.data(), socket_path.size()),
      volume_path_(volume_path.data(), volume_path.size()),
      library_path_(kDefaultDaemonLibraryName),
      handler_(handler),
      abi_loader_(&LoadDaemonAbi),
      monotonic_clock_(&DaemonAdapter::MonotonicNowMs) {}

DaemonAdapter::~DaemonAdapter() {
  record_cache_published_.store(nullptr, std::memory_order_release);
  if (record_cache_ != nullptr && abi_ != nullptr) {
    abi_->CacheClose(record_cache_);
    record_cache_ = nullptr;
  }
  if (abi_ != nullptr) {
    for (void* retired : retired_record_caches_) {
      abi_->CacheClose(retired);
    }
  }
  retired_record_caches_.clear();
}

bool DaemonAdapter::ReadVolumeGeneration(StringPiece volume_path,
                                         uint64_t* generation) {
  *generation = 0;
  const GoogleString path = StrCat(volume_path, ".gen");
  errno = 0;
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // ABSENT is an answer: the daemon has never replaced its volume, which
    // is generation 0.  Every other failure -- no descriptors left,
    // permission denied, an I/O error -- is not an answer about the volume
    // at all, and must not be read as "generation 0".
    return errno == ENOENT;
  }
  char buffer[32];
  const size_t length = fread(buffer, 1, sizeof(buffer), file);
  fclose(file);
  // The daemon writes a decimal number and a newline, and replaces the file
  // atomically.  Anything else -- nothing at all, no newline after the
  // digits (a file caught half written by some other writer), a leading
  // character that is not a digit, a number that does not fit -- is
  // UNKNOWN.
  uint64_t value = 0;
  size_t digits = 0;
  size_t i = 0;
  for (; i < length; ++i) {
    const char c = buffer[i];
    if (c < '0' || c > '9') {
      break;
    }
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    ++digits;
  }
  // The line may end in a carriage return and a newline: a writer in text
  // mode on Windows produces that, and it is as complete a line as one that
  // ends in the newline alone.  A carriage return with no newline after it
  // is a line cut short.
  if (i < length && buffer[i] == '\r') {
    ++i;
  }
  if (digits == 0 || i >= length || buffer[i] != '\n') {
    return false;
  }
  *generation = value;
  return true;
}

bool DaemonAdapter::VolumeWasReplaced() const {
  if (record_cache_replaced_.load(std::memory_order_acquire)) {
    return true;
  }
  const int64_t now_ms = monotonic_clock_();
  int64_t next_ms = generation_next_check_ms_.load(std::memory_order_relaxed);
  if (now_ms < next_ms) {
    return false;
  }
  // One thread per interval reads the file; the others answer "no" from the
  // last read and are right until that thread reports back.
  if (!generation_next_check_ms_.compare_exchange_strong(
          next_ms, now_ms + kVolumeGenerationCheckIntervalMs,
          std::memory_order_relaxed)) {
    return false;
  }
  uint64_t generation = 0;
  if (!ReadVolumeGeneration(volume_path_, &generation)) {
    // UNKNOWN, with nothing detected so far.  Not a replacement: the handle
    // in use stays in use, and the next interval asks again.
    return false;
  }
  if (generation == record_cache_generation_.load(std::memory_order_acquire)) {
    return false;
  }
  record_cache_replaced_.store(true, std::memory_order_release);
  return true;
}

void* DaemonAdapter::RecordCache() {
  if (health_ != DaemonHealth::kReady || abi_ == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(record_cache_mutex_);
  if (record_cache_ != nullptr && !record_cache_closed_ &&
      VolumeWasReplaced()) {
    // Decided again under the lock, against the number this handle is held
    // against: the look that raised the flag ran without it, and may have
    // raced the open that produced this very handle.
    const int64_t recheck_now_ms = monotonic_clock_();
    if (recheck_now_ms < replaced_recheck_ms_) {
      // The last read under the lock gave no answer and the next is not due.
      return nullptr;
    }
    uint64_t generation = 0;
    if (!ReadVolumeGeneration(volume_path_, &generation)) {
      // UNKNOWN, after a replacement was seen.  That is no evidence against
      // it: the flag stays, the handle stays out of use, and the question is
      // asked again one interval from now.
      replaced_recheck_ms_ = recheck_now_ms + kVolumeGenerationCheckIntervalMs;
      return nullptr;
    }
    if (generation ==
        record_cache_generation_.load(std::memory_order_relaxed)) {
      // A number that WAS read, and it is this handle's own: the flag came
      // from a look that raced this handle's open.  Back in use.
      record_cache_replaced_.store(false, std::memory_order_release);
    } else {
      // The daemon replaced its volume (a full cache purge).  This handle
      // is on the deleted file: stop handing it out, and set it aside -- a
      // recorder may still hold it, so it is not closed here.
      record_cache_published_.store(nullptr, std::memory_order_release);
      retired_record_caches_.push_back(record_cache_);
      record_cache_ = nullptr;
      record_cache_replaced_.store(false, std::memory_order_release);
      record_cache_attempts_ = 0;
      record_cache_next_attempt_ms_ = 0;
      record_cache_reopening_ = true;
      ++volume_replacements_;
      if (volume_replacements_ > kMaxVolumeReplacements) {
        // The bound.  This process holds kMaxVolumeReplacements + 1
        // replaced volumes open and will not take on another: it stops
        // using the cache until it ends.
        record_cache_gave_up_ = true;
        if (handler_ != nullptr) {
          const GoogleString message = StrCat(
              "nothing will be recorded for in-place optimization: the "
              "optimizer daemon's cache volume at ",
              volume_path_, " has been replaced more than ",
              IntegerToString(kMaxVolumeReplacements),
              " times since this process started, and each replaced volume "
              "stays open (its disk space and address space held) until the "
              "process exits. In-place optimization stays off in this "
              "process until the web server is restarted or its worker "
              "processes are recycled.");
          if (ShouldAnnounce(message)) {
            handler_->Message(kError, "%s", message.c_str());
          }
        }
      } else if (handler_ != nullptr) {
        const GoogleString message = StrCat(
            "the optimizer daemon replaced its cache volume at ", volume_path_,
            " (generation ", Integer64ToString(static_cast<int64>(generation)),
            "); opening the new volume");
        if (ShouldAnnounce(message)) {
          handler_->Message(kInfo, "%s", message.c_str());
        }
      }
    }
  }
  if (record_cache_ != nullptr || record_cache_gave_up_ ||
      record_cache_closed_) {
    // Opened, or done trying.  Either way there is nothing left to decide.
    return record_cache_;
  }
  const int64_t now_ms = monotonic_clock_();
  if (record_cache_attempts_ > 0 && now_ms < record_cache_next_attempt_ms_) {
    // Inside a backoff window.  This is the common path once something is
    // wrong -- it is what keeps a durable failure from costing an open
    // syscall, and a log line, on every request.
    return nullptr;
  }
  ++record_cache_attempts_;

  // An attempt that must not go ahead, or whose handle must not be used,
  // for a reason that may pass: it takes the ordinary retry schedule and
  // says why, once per attempt.  The arm stays closed in between.
  const auto not_yet = [&](const GoogleString& reason) -> void* {
    const bool giving_up = record_cache_attempts_ >= kRecordCacheMaxAttempts;
    if (giving_up) {
      record_cache_gave_up_ = true;
    } else {
      record_cache_next_attempt_ms_ =
          now_ms + BackoffMsAfter(record_cache_attempts_);
    }
    if (handler_ != nullptr) {
      GoogleString message = StrCat(
          "nothing will be recorded for in-place optimization: the optimizer "
          "daemon's cache volume at ",
          volume_path_, " cannot be used yet: ", reason, " (attempt ",
          IntegerToString(record_cache_attempts_), " of ",
          IntegerToString(kRecordCacheMaxAttempts), ")");
      if (giving_up) {
        StrAppend(&message,
                  ". Giving up: in-place optimization stays off in this "
                  "process until the web server is restarted.");
      }
      if (ShouldAnnounce(message)) {
        handler_->Message(giving_up ? kError : kWarning, "%s", message.c_str());
      }
    }
    return nullptr;
  };
  // An open that cannot be shown to have landed on the daemon's own volume,
  // or that cannot be checked at all.  Not retried: the process stops using
  // the cache until it ends, and says so once.  Nothing on disk is touched.
  const auto stop_for_good = [&](const GoogleString& reason) -> void* {
    record_cache_gave_up_ = true;
    if (handler_ != nullptr) {
      const GoogleString message = StrCat(
          "nothing will be recorded for in-place optimization: the optimizer "
          "daemon's cache volume at ",
          volume_path_, " is not used: ", reason,
          ". In-place optimization stays off in this process until the web "
          "server is restarted or its worker processes are recycled.");
      if (ShouldAnnounce(message)) {
        handler_->Message(kError, "%s", message.c_str());
      }
    }
    return nullptr;
  };

  // EVERY OPEN IN A SERVING PROCESS LOOKS BEFORE IT OPENS AND AGAIN AFTER --
  // a process's first open as much as a reopen -- because the daemon's open
  // call creates the volume file it does not find, and a volume this module
  // made is one nothing else reads.
  //
  // Before: the size the daemon publishes NOW.  The size read when the
  // server started may be out of date -- the daemon may have been restarted
  // with another cache size since, and a worker process can start at any
  // time -- and an open at the old size would create a file of the old
  // geometry.  Then the files: at least one of the current cache format
  // must be there, and for a REOPEN exactly one (a purge leaves one; a
  // second one is a file some process created by mistake, and nothing here
  // can tell which of the two the daemon uses).  A first open tolerates
  // several, as the start-up check does: after a resize without a purge the
  // file of the earlier size is still there.  Every file's identity is
  // noted.
  //
  // The one exception is a configured path that is itself a directory,
  // where every file inside counts as a volume file and none can be named
  // as the daemon's.  No supported configuration has that shape.  A first
  // open there is made as it always was, at the size the start-up check
  // verified; a reopen is not made at all.
  uint64_t volume_size = inherited_volume_size_;
  std::vector<GoogleString> volume_files_before;
  std::vector<VolumeFileIdentity> volume_identities_before;
  const bool checked_open = !VolumePathIsDirectory(volume_path_);
  if (!checked_open && record_cache_reopening_) {
    return stop_for_good(
        "the daemon replaced it, and the configured path is a directory, "
        "where the daemon's volume file cannot be told from the other files "
        "in it, so the new volume is not opened");
  }
  if (checked_open) {
    volume_size = abi_->SharedConfigVolumeSize(volume_path_.c_str());
    if (volume_size == 0) {
      return not_yet(
          "the daemon does not publish the size of its cache volume at the "
          "moment");
    }
    volume_files_before = VolumeFiles(volume_path_);
    const std::vector<GoogleString> candidates =
        CurrentFormatVolumeFiles(volume_path_, volume_files_before);
    if (candidates.empty()) {
      return not_yet(
          "the daemon has not created its volume file yet, and this module "
          "does not create it");
    }
    if (record_cache_reopening_ && candidates.size() > 1) {
      return not_yet(StrCat(
          "after the daemon replaced its volume there is more than one "
          "volume file of the current cache format (",
          JoinCollection(candidates, ", "),
          "), and this module cannot tell which one the daemon uses. The "
          "daemon has exactly one of them open; any other was not created "
          "by the daemon"));
    }
    for (const GoogleString& name : volume_files_before) {
      VolumeFileIdentity identity;
      if (!ReadVolumeFileIdentity(volume_path_, name, &identity)) {
        return not_yet(
            StrCat("the volume file ", name, " cannot be looked at"));
      }
      volume_identities_before.push_back(identity);
    }
  }

  alignas(std::max_align_t) unsigned char
      config_storage[kCacheConfigProbeBytes] = {};
  PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(config_storage);
  abi_->CacheConfigInit(config);
  if (!ApplyInheritedSizing(config, volume_size)) {
    // NOT retried, and this is the one open failure that still latches: the
    // size is the daemon's own and re-deriving the configuration from the
    // same number cannot reach a different answer.  Retrying it would be a
    // schedule with no state to wait for.
    record_cache_gave_up_ = true;
    return nullptr;
  }
  config->volume_path = volume_path_.c_str();

  // The generation this handle will be held against, read BEFORE the open:
  // a replacement that lands between this read and the open then shows as a
  // difference at the read after it.  No answer, no open.
  uint64_t generation_before_open = 0;
  if (!ReadVolumeGeneration(volume_path_, &generation_before_open)) {
    return not_yet(
        "the generation the daemon publishes beside it cannot be read");
  }
  void* cache = nullptr;
  const int open_error = abi_->CacheOpen(config, &cache);
  if (open_error != kPsOk) {
    // Described FIRST, before the filesystem probe below: the peer's reason
    // lives in a buffer it clears on the next ps_cache_open, so it has to be
    // copied out before this module does anything else.
    const GoogleString reason = DescribeDaemonError(abi_.get(), open_error);
    const bool giving_up = record_cache_attempts_ >= kRecordCacheMaxAttempts;
    if (giving_up) {
      record_cache_gave_up_ = true;
    } else {
      record_cache_next_attempt_ms_ =
          now_ms + BackoffMsAfter(record_cache_attempts_);
    }
    if (handler_ != nullptr) {
      GoogleString message = StrCat(
          "nothing will be recorded for in-place optimization: cannot open "
          "the optimizer daemon's cache volume at ",
          volume_path_, ": ", reason);
      // This open runs in the request-serving process, which is the one that
      // carries the web-server user's group memberships -- so a permission
      // failure HERE is the group join the startup probe (which may run with
      // different privileges) could not see.
      if (VolumeDirAccess(volume_path_) == DirAccess::kDenied) {
        StrAppend(&message, kGroupMembershipHint);
      }
      // The attempt counter is part of the TEXT, not just of the sentence:
      // the announcement latch is keyed on the message, so counting here is
      // what lets each attempt be heard once while still holding a repeated
      // failure to one line per attempt rather than one per request.
      StrAppend(&message, " (attempt ", IntegerToString(record_cache_attempts_),
                " of ", IntegerToString(kRecordCacheMaxAttempts), ")");
      if (giving_up) {
        StrAppend(&message,
                  ". Giving up: in-place optimization stays off in this "
                  "process until the web server is restarted.");
      }
      if (ShouldAnnounce(message)) {
        // Warning while the arm may still recover on its own, error only for
        // the state that needs an operator.  A start-up race that heals on
        // the second attempt should not page anyone.
        handler_->Message(giving_up ? kError : kWarning, "%s", message.c_str());
      }
    }
    return nullptr;
  }
  if (checked_open) {
    // Did the open land on a file that was there, or make one?  The same
    // names, and the same file under each, or the handle is not used.  It
    // was never handed out, so closing it here cannot pull a mapping out
    // from under a request.  Whatever appeared stays on disk: this module
    // deletes nothing.
    const std::vector<GoogleString> volume_files_after =
        VolumeFiles(volume_path_);
    bool unchanged = volume_files_after == volume_files_before;
    for (size_t i = 0; unchanged && i < volume_files_after.size(); ++i) {
      VolumeFileIdentity identity;
      unchanged = ReadVolumeFileIdentity(volume_path_, volume_files_after[i],
                                         &identity) &&
                  identity == volume_identities_before[i];
    }
    if (!unchanged) {
      abi_->CacheClose(cache);
      return stop_for_good(StrCat(
          "while this process was opening it, a volume file appeared, "
          "disappeared or was replaced (before: ",
          JoinCollection(volume_files_before, ", "),
          "; after: ", JoinCollection(volume_files_after, ", "),
          "), so the open may have created a volume file of its own "
          "instead of using the daemon's. No file was removed. Compare the "
          "volume files beside that path with the one the daemon has open"));
    }
  }
  // Read again AFTER the open.  Only a handle whose number was known before
  // and is the same now is handed out; one that is not was never handed
  // out either, so it is closed here and the open is tried again.
  uint64_t generation_after_open = 0;
  if (!ReadVolumeGeneration(volume_path_, &generation_after_open) ||
      generation_after_open != generation_before_open) {
    abi_->CacheClose(cache);
    return not_yet(
        "the generation the daemon publishes beside it changed, or could "
        "not be read, while the volume was being opened");
  }
  record_cache_generation_.store(generation_before_open,
                                 std::memory_order_release);
  generation_next_check_ms_.store(now_ms + kVolumeGenerationCheckIntervalMs,
                                  std::memory_order_relaxed);
  record_cache_replaced_.store(false, std::memory_order_release);
  replaced_recheck_ms_ = 0;
  record_cache_ = cache;
  record_cache_published_.store(record_cache_, std::memory_order_release);
  return record_cache_;
}

void* DaemonAdapter::RecordCacheIfOpen() const {
  if (record_cache_published_.load(std::memory_order_acquire) == nullptr) {
    return nullptr;
  }
  if (VolumeWasReplaced()) {
    // The daemon replaced its volume and this handle is on the deleted
    // file.  Handing out nothing sends the request to the origin, which is
    // always safe; RecordCache() opens the new volume.
    return nullptr;
  }
  // Loaded AFTER the check, so a caller that was held up across another
  // thread's whole set-aside and reopen is handed the new handle, not the
  // one it saw before.
  return record_cache_published_.load(std::memory_order_acquire);
}

void DaemonAdapter::CloseRecordCache() {
  std::lock_guard<std::mutex> lock(record_cache_mutex_);
  record_cache_published_.store(nullptr, std::memory_order_release);
  if (record_cache_ != nullptr && abi_ != nullptr) {
    abi_->CacheClose(record_cache_);
    record_cache_ = nullptr;
  }
  // The handles set aside when the daemon replaced its volume: the port
  // that calls this has finished or joined every holder, which is the
  // condition under which they may finally be closed.
  if (abi_ != nullptr) {
    for (void* retired : retired_record_caches_) {
      abi_->CacheClose(retired);
    }
  }
  retired_record_caches_.clear();
  // Final: RecordCache() reports nothing and never reopens from here.  Its
  // own flag, not the retry schedule's gave-up latch -- a clean close is not
  // a failure.
  record_cache_closed_ = true;
}

#ifndef _WIN32
void DaemonAdapter::SetEffectiveUidForTesting(EffectiveUidFn uid_fn) {
  g_effective_uid_for_testing = uid_fn == nullptr ? &geteuid : uid_fn;
}
#endif

void DaemonAdapter::ResetAnnouncementsForTesting() {
  for (Announcements* a : {announcements(), reannouncements()}) {
    std::lock_guard<std::mutex> lock(a->mu);
    a->seen.clear();
  }
}

void DaemonAdapter::ResetSocketVerdictsForTesting() {
  SocketVerdicts* verdicts = socket_verdicts();
  std::lock_guard<std::mutex> lock(verdicts->mu);
  verdicts->deadline_expiries_ms.clear();
  verdicts->latch_ms = kSocketVerdictLatchMs;
}

void DaemonAdapter::SetSocketVerdictLatchMsForTesting(int64_t latch_ms) {
  SocketVerdicts* verdicts = socket_verdicts();
  std::lock_guard<std::mutex> lock(verdicts->mu);
  verdicts->latch_ms = latch_ms;
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
  const GoogleString path(volume_path.data(), volume_path.size());
  if (path.empty()) {
    return out;
  }
#ifdef _WIN32
  // The same two shapes as POSIX: the configured path is a directory the
  // volume lives inside, and/or a stem the volume sits beside.  Either
  // separator may appear -- an operator writes both spellings -- so the
  // stem/parent split takes the LAST of either.
  std::wstring wide_path;
  // A configured path that is not valid UTF-8 yields an empty result, never
  // a crash: the path is configuration data, and a probe must not take the
  // server down over it.
  if (!Utf8ToWide(path, &wide_path)) {
    return out;
  }
  const DWORD attributes = GetFileAttributesW(wide_path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    ScanDirectory(path, StringPiece(), &out);
  }
  const size_t slash = path.find_last_of("/\\");
  // A stem directly under the root of the current drive (`\volume`) scans
  // that root, mirroring the POSIX rule for `/volume`.
  const GoogleString parent = slash == GoogleString::npos ? GoogleString(".")
                              : slash == 0                ? GoogleString("\\")
                                           : path.substr(0, slash);
  const GoogleString stem =
      slash == GoogleString::npos ? path : path.substr(slash + 1);
  if (!stem.empty()) {
    ScanDirectory(parent, stem, &out);
  }
#else
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
#endif
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

DaemonAdapter::DirAccess DaemonAdapter::VolumeDirAccess(
    StringPiece volume_path) {
  const GoogleString path(volume_path.data(), volume_path.size());
  if (path.empty()) {
    return DirAccess::kAbsent;
  }
  // The configured path may name the directory the volume lives in, or be a
  // stem the volume sits beside -- the same two shapes VolumeFiles scans.
#ifdef _WIN32
  std::wstring wide_path;
  if (!Utf8ToWide(path, &wide_path)) {
    return DirAccess::kAbsent;
  }
  const DWORD attributes = GetFileAttributesW(wide_path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return ProbeDir(path);
  }
  const size_t slash = path.find_last_of("/\\");
  // A stem directly under the root of the current drive (`\volume`) scans
  // that root, mirroring the POSIX rule for `/volume` -- never an empty
  // parent that reads as no directory at all.
  const GoogleString parent = slash == GoogleString::npos ? GoogleString(".")
                              : slash == 0                ? GoogleString("\\")
                                           : path.substr(0, slash);
  return ProbeDir(parent);
#else
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    return ProbeDir(path);
  }
  const size_t slash = path.find_last_of('/');
  const GoogleString parent = slash == GoogleString::npos ? GoogleString(".")
                              : slash == 0                ? GoogleString("/")
                                           : path.substr(0, slash);
  return ProbeDir(parent);
#endif
}

bool DaemonAdapter::SocketAnswers(StringPiece path, GoogleString* error) {
#ifdef _WIN32
  // The configured "socket path" on this platform is a PIPE BASE NAME; the
  // client library prepends the device prefix, verbatim, and so does the
  // probe, so the messages spell the pipe the way the operator recognises
  // it.  The probe is the standard named-pipe client sequence minus the
  // write: open the pipe first, and wait for an instance only when the open
  // says every instance is taken; a successful open is closed again at once.
  const GoogleString configured(path.data(), path.size());
  const GoogleString spelled = StrCat("\\\\.\\pipe\\", configured);
  if (SocketDeadlineIsLatched(spelled, MonotonicNowMs())) {
    AppendDaemonDeadlineMessage(error, spelled);
    return false;
  }
  std::wstring wide_spelled;
  if (!Utf8ToWide(spelled, &wide_spelled)) {
    StrAppend(error,
              "the daemon socket path is not valid UTF-8 and cannot name a "
              "pipe: ",
              configured);
    return false;
  }

  const int64_t deadline_ms = MonotonicNowMs() + kSocketConnectDeadlineMs;
  bool connected = false;
  bool timed_out = false;
  DWORD last_error = 0;
  bool access_denied = false;
  bool not_found = false;
  // Set when WaitNamedPipeW has just reported a free instance: a busy open
  // right after that lost the instance to another client, and is paced
  // before waiting again rather than spinning.
  bool instance_was_free = false;
  while (!connected && !timed_out) {
    if (deadline_ms - MonotonicNowMs() <= 0) {
      timed_out = true;
      break;
    }
    // The open comes FIRST: an absent pipe answers ERROR_FILE_NOT_FOUND at
    // once and definitively, where a wait on a name with no instance can sit
    // out its whole timeout on some hosts (#1050).
    // SECURITY_IDENTIFICATION: the open identifies this process and nothing
    // more; whatever answers under the configured name cannot act as it.
    HANDLE pipe =
        CreateFileW(wide_spelled.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING,
                    SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe);
      connected = true;
      break;
    }
    last_error = GetLastError();
    if (last_error == ERROR_FILE_NOT_FOUND) {
      not_found = true;
      break;
    }
    if (last_error == ERROR_ACCESS_DENIED) {
      access_denied = true;
      break;
    }
    if (last_error != ERROR_PIPE_BUSY && last_error != ERROR_SEM_TIMEOUT) {
      break;
    }
    // Every instance is taken: wait inside what remains of the deadline,
    // exactly the treatment a full POSIX accept queue gets.
    if (instance_was_free) {
      // Paced exactly like the POSIX lane's backlog retry, so a busy open
      // loops at the same rate instead of spinning.
      const int64_t left_ms = deadline_ms - MonotonicNowMs();
      if (left_ms > 0) {
        Sleep(static_cast<DWORD>(
            std::min<int64_t>(kSocketConnectRetryMs, left_ms)));
      }
    }
    instance_was_free = false;
    const int64_t remaining_ms = deadline_ms - MonotonicNowMs();
    if (remaining_ms <= 0) {
      // Never pass 0: to WaitNamedPipeW that is NMPWAIT_USE_DEFAULT_WAIT.
      timed_out = true;
      break;
    }
    if (WaitNamedPipeW(wide_spelled.c_str(),
                       static_cast<DWORD>(remaining_ms))) {
      // An instance is available NOW.  Another client may still take it
      // first; the open above retries inside what remains of the deadline.
      instance_was_free = true;
      continue;
    }
    last_error = GetLastError();
    if (last_error == ERROR_SEM_TIMEOUT) {
      timed_out = true;
      break;
    }
    if (last_error == ERROR_FILE_NOT_FOUND) {
      // The daemon closed its last instance between the open and the wait.
      not_found = true;
      break;
    }
    if (last_error == ERROR_ACCESS_DENIED) {
      access_denied = true;
      break;
    }
    break;
  }

  if (timed_out) {
    AppendDaemonDeadlineMessage(error, spelled);
    RecordSocketDeadline(spelled, MonotonicNowMs());
  } else if (!connected) {
    StrAppend(error, "the optimizer daemon does not answer at ", spelled, ": ",
              WindowsErrorText(last_error));
    // Distinguish the two causes an operator can act on: a refused open
    // says who refused and why no grant here can fix it; an absent pipe
    // means the daemon is not running.
    if (access_denied) {
      // A PIPE is not a directory: no grant this module can name would fix
      // a refused open here, because the optimizer daemon decides who may
      // open its pipe.  The sentence says what happened and to whom.
      StrAppend(error,
                " The optimizer daemon refused the web server's worker "
                "identity access to that pipe; the daemon decides who may "
                "open it.");
    } else if (not_found) {
      StrAppend(error,
                " The named pipe does not exist -- start the optimizer "
                "daemon.");
    }
  }
  return connected;
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

  // A path that already cost one probe its deadline is not re-probed inside
  // the latch interval: the verdict and its message are the ones a real
  // timed-out probe would produce.
  if (SocketDeadlineIsLatched(spelled, MonotonicNowMs())) {
    AppendDaemonDeadlineMessage(error, spelled);
    return false;
  }

  // Non-blocking from creation: the connect below is deadline-bounded, and a
  // blocking connect can wait indefinitely on a daemon that is not accepting.
  // Close-on-exec as well: the probe runs in the parent before it forks.
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd >= 0) {
    // Read-modify-write on the status flags, and fail closed: an fd that
    // stays blocking would silently reinstate the unbounded wait the
    // deadline below removes.
    const int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      const int saved_errno = errno;
      close(fd);
      fd = -1;
      errno = saved_errno;
    }
  }
#endif
  if (fd < 0) {
    StrAppend(error, "cannot create a socket to reach the optimizer daemon: ",
              strerror(errno));
    return false;
  }

  const int64_t deadline_ms = MonotonicNowMs() + kSocketConnectDeadlineMs;
  bool connected = false;
  bool timed_out = false;
  int connect_errno = 0;

  // Attempt the connect until it succeeds, goes in flight (EINPROGRESS), or
  // fails outright.  EAGAIN -- what Linux returns for a unix stream socket
  // whose listener's backlog is full -- retries at a short fixed interval,
  // because the socket is not in a connecting state and poll() cannot wait
  // it out; on macOS a full queue answers ECONNREFUSED at once instead.
  // EINTR takes the same bounded retry, so a signal storm cannot spin.
  // Both consume only the remaining deadline.
  while (!connected && !timed_out) {
    errno = 0;
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) == 0) {
      connected = true;
      break;
    }
    connect_errno = errno;
    if (connect_errno == EISCONN) {
      // An interrupted connect completes asynchronously; the retry reports
      // the connection that already succeeded.
      connected = true;
    } else if (connect_errno == EINTR || connect_errno == EAGAIN) {
      const int64_t remaining_ms = deadline_ms - MonotonicNowMs();
      if (remaining_ms <= 0) {
        timed_out = true;
      } else {
        usleep(1000 * std::min(kSocketConnectRetryMs, remaining_ms));
      }
    } else {
      break;
    }
  }

  // A connect in flight completes asynchronously: wait for writability
  // inside the deadline, then read SO_ERROR for the verdict.  AF_UNIX stream
  // sockets do not take this arm on Linux or the BSDs -- their connect
  // completes synchronously or fails at once -- it stays for correctness if
  // the probe is ever pointed at another family.
  if (!connected && !timed_out &&
      (connect_errno == EINPROGRESS || connect_errno == EALREADY)) {
    for (;;) {
      const int64_t remaining_ms = deadline_ms - MonotonicNowMs();
      if (remaining_ms <= 0) {
        timed_out = true;
        break;
      }
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      errno = 0;
      const int ready = poll(&pfd, 1, static_cast<int>(remaining_ms));
      if (ready > 0) {
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) !=
            0) {
          connect_errno = errno;
        } else if (so_error != 0) {
          connect_errno = so_error;
        } else if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
                   (pfd.revents & POLLOUT) == 0) {
          // The peer signalled an error or hang-up with nothing pending.
          connect_errno = ECONNRESET;
        } else {
          connected = true;
        }
        break;
      }
      if (ready == 0) {
        timed_out = true;
        break;
      }
      if (errno != EINTR) {
        connect_errno = errno;
        break;
      }
      // EINTR: wait again, against the recomputed remaining deadline.
    }
  }

  if (timed_out) {
    // Latch the verdict with its expiry: the next probe of this path inside
    // the interval answers from the latch instead of costing the deadline
    // again.  After the expiry the next probe is a real probe again.
    AppendDaemonDeadlineMessage(error, spelled);
    RecordSocketDeadline(spelled, MonotonicNowMs());
  } else if (!connected) {
    StrAppend(error, "the optimizer daemon does not answer at ", spelled, ": ",
              strerror(connect_errno));
    // Distinguish the two causes an operator can act on.  After H2/H3 a
    // permission failure means the group join is missing; an absent socket
    // means the daemon is not running.
    if (connect_errno == EACCES || connect_errno == EPERM) {
      StrAppend(error, kGroupMembershipHint);
    } else if (connect_errno == ENOENT) {
      StrAppend(error,
                " The socket does not exist -- start the pagespeed-optimizer "
                "daemon (for example `systemctl start pagespeed-optimizer`).");
    }
  }
  close(fd);
  return connected;
#endif
}

DaemonStartupStatus DaemonAdapter::Resolve(GoogleString* error) {
  health_ = DaemonHealth::kUnavailable;
  extra_volume_warning_.clear();
  legacy_generation_layout_ = false;

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

  // The cache-directory handshake.  Since the daemon's privilege drop its
  // cache lives in a versioned cold-start directory (vN) and the daemon
  // publishes N in its shared config as cache_dir_generation.  A published
  // generation this build is not written for means the two layouts share
  // NOTHING, so the substrate goes down loudly here rather than attaching to
  // a directory whose contents mean something else -- the loud handshake
  // failure, never a silent split-brain.  Nothing is opened on this path.
  const uint32_t generation =
      abi_->SharedConfigGeneration(volume_path_.c_str());
  if (generation != 0 && generation != kCacheDirGeneration) {
    StrAppend(error,
              "in-place optimization is OFF: the optimizer daemon publishes "
              "cache directory generation ",
              Integer64ToString(static_cast<int64>(generation)),
              " (cache_dir_generation) and this module is built for "
              "generation ",
              Integer64ToString(static_cast<int64>(kCacheDirGeneration)),
              ". The two cache layouts share nothing; nothing was opened and "
              "nothing was created. Install a module and daemon package pair "
              "that agree.");
    return DaemonStartupStatus::kOk;
  }
  // 0 is "unknown", not a generation: a pre-H1 daemon publishes no such field
  // (or its package is too old to export the reader).  That is the legacy,
  // unversioned layout, and it is TOLERATED -- the configured paths are used
  // as-is, exactly as before the field existed.  The tolerance is announced
  // once on the healthy path below, where it would otherwise be invisible.
  legacy_generation_layout_ = (generation == 0);

  const uint64_t inherited = abi_->SharedConfigVolumeSize(volume_path_.c_str());
  inherited_volume_size_ = inherited;
  if (inherited == 0) {
    // Either the daemon has never run (no shared config to read), or its
    // config is unreadable, or it declares a schema this build refuses.  All
    // of them mean the same thing here: the size is not known, so the volume
    // is not opened.
    if (VolumeDirAccess(volume_path_) == DirAccess::kDenied) {
      // "Unreadable" because of PERMISSIONS, not absence: since the
      // privilege drop the shared config is group-readable only (0640
      // pagespeed:pagespeed), so this is the group join that did not happen.
      StrAppend(error,
                "in-place optimization is OFF: cannot read the optimizer "
                "daemon's shared configuration beside ",
                volume_path_, ": permission denied.", kGroupMembershipHint);
      return DaemonStartupStatus::kOk;
    }
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
    if (VolumeDirAccess(volume_path_) == DirAccess::kDenied) {
      // The scan found nothing because it was not ALLOWED to look: the cache
      // directory is group-only (2770 pagespeed:pagespeed) since the
      // privilege drop.  Reported apart from the absent case below, because
      // "start the daemon" is the wrong fix for this one.
      StrAppend(error,
                "in-place optimization is OFF: cannot look inside the "
                "optimizer daemon's cache directory at ",
                volume_path_, ": permission denied.", kGroupMembershipHint);
      return DaemonStartupStatus::kOk;
    }
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
    // these files, and the attach check below proves we landed on the file
    // named for the published size.  That is the daemon's file unless
    // something other than the daemon left a file of that name.  So: say
    // something an operator can act on, and carry on.  (A start the mirror
    // below refuses removes the file its own open created, so a refused
    // start does not add to this state unless its removal fails, and then
    // its message says so.)
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
              volume_path_, ": ", DescribeDaemonError(abi_.get(), open_error));
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
    // A refused start removes what APPEARED between its two scans, whoever
    // made it.  Left in place, that file would let the next start of this
    // server attach to it and come up healthy on a cache the daemon never
    // reads -- the silent split this refusal exists to prevent.  Most of
    // the time what appeared is this open's own file: the cache library's
    // creating open writes the volume file and nothing beside it while the
    // small tier is off, which is how this module opens it.  It can also be
    // the daemon's own new volume (see the race below), and on POSIX the
    // owner refusal in RemoveVolumeFile keeps that one on disk.
    //
    // THE RACE THIS INVITES, both directions.  This open made the file and
    // the daemon attached a moment later: on Linux the unlink lands on an
    // open file -- the daemon keeps its handle, the name is gone, the next
    // start of this server refuses again for the same reason, and a daemon
    // restart recovers onto a fresh volume of its own; before this removal
    // existed, that same overlap healed itself, the retry taking the
    // left-over-files branch and attaching to the very file the daemon was
    // using.  On Windows the delete fails while another process holds the
    // file without delete sharing: the verdict stays refused and the
    // message below names the manual step.  The daemon made the file and
    // this open attached to it: the check refuses either way -- it saw a
    // file appear -- and the removal is what must not then take the
    // daemon's volume.  No lock is added: coordinating two processes' opens
    // of a volume the daemon owns would invent a protocol neither side has.
    std::vector<GoogleString> removed;
    std::vector<GoogleString> stuck;
    std::vector<GoogleString> held;
    std::vector<GoogleString> unverified;
    std::vector<GoogleString> why_stuck;
    std::vector<GoogleString> why_held;
    for (const GoogleString& name : created) {
      GoogleString why;
      const RemoveOutcome outcome = RemoveVolumeFile(volume_path_, name, &why);
      if (outcome == RemoveOutcome::kHeldByOtherUser) {
        held.push_back(name);
        why_held.push_back(why);
      } else if (outcome == RemoveOutcome::kRemoved) {
        removed.push_back(name);
      } else {
        stuck.push_back(name);
        why_stuck.push_back(why);
      }
    }
    // The claim below is only as strong as the scan that detected the
    // split, so the scan has the last word -- in BOTH directions.  A name
    // counts as removed only when the removal call said so AND the scan
    // no longer lists it (a transient stat failure must not claim a
    // removal that did not happen), and a name the call FAILED to remove
    // is never reported as removed even if the scan no longer lists it
    // (the optimizer may have taken the name over in between).  A name
    // that fails either test is UNVERIFIED, never silently dropped: the
    // file may still be on disk, and a directory-shaped configured path
    // is the plain case -- the removal looks beside the directory, does
    // not find the name there, and the re-scan still lists it from
    // inside.  A scan that no longer lists the DAEMON's own files either
    // was never readable or lost them, so in that case nothing is
    // verified at all.
    const std::vector<GoogleString> verified = VolumeFiles(volume_path_);
    bool scan_usable = true;
    for (const GoogleString& name : before) {
      if (std::find(verified.begin(), verified.end(), name) == verified.end()) {
        scan_usable = false;
        break;
      }
    }
    if (scan_usable) {
      std::vector<GoogleString> confirmed;
      for (const GoogleString& name : removed) {
        if (std::find(verified.begin(), verified.end(), name) ==
            verified.end()) {
          confirmed.push_back(name);
        } else {
          unverified.push_back(name);
        }
      }
      removed.swap(confirmed);
    } else {
      // Nothing can be verified; every name the calls reported gone is
      // unverified rather than removed, and the message says so below.
      unverified.insert(unverified.end(), removed.begin(), removed.end());
      removed.clear();
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
              "it. ");
    // The removed sentence never prints with an empty list: a name whose
    // removal could not be confirmed is unverified, not removed, and every
    // created name lands in exactly one of the four buckets.
    if (!held.empty()) {
      // The one case where "do not touch it" is the advice: the file is
      // most likely the optimizer's own new volume, and attaching to it
      // on the next start is the CORRECT outcome -- deleting it by hand
      // would undo exactly the protection this refusal exists for.  Each
      // held name's reason already spells what happened to which file.
      for (const GoogleString& why : why_held) {
        StrAppend(error, why, ". ");
      }
      StrAppend(error,
                "Do not remove it while the optimizer daemon is running; "
                "once it is running on it, the next start of this server "
                "attaches to it. If no optimizer daemon is running on it, "
                "remove it while the daemon is stopped. ");
      if (!removed.empty()) {
        StrAppend(error, "This start removed ", JoinCollection(removed, ", "),
                  ". ");
      }
      if (!stuck.empty()) {
        StrAppend(error, "This start could not remove ",
                  JoinCollection(stuck, ", "), ": ",
                  JoinCollection(why_stuck, "; "),
                  ". Remove those by hand before this server starts "
                  "again: a start that finds them attaches to them "
                  "without refusing. ");
      }
      if (!unverified.empty()) {
        StrAppend(error,
                  "This start could not confirm that the file it created (",
                  JoinCollection(unverified, ", "),
                  ") is gone; check the cache directory before this "
                  "server starts again. ");
      }
    } else if (stuck.empty() && unverified.empty()) {
      StrAppend(error, "This start removed the file it created (",
                JoinCollection(removed, ", "),
                "), so the directory holds only what it held before this "
                "start. ");
    } else {
      if (!stuck.empty()) {
        StrAppend(error, "This start could not remove the file it created (",
                  JoinCollection(stuck, ", "),
                  "): ", JoinCollection(why_stuck, "; "), ". ");
      }
      if (!unverified.empty()) {
        StrAppend(error,
                  "This start could not confirm that the file it created (",
                  JoinCollection(unverified, ", "),
                  ") is gone; check the cache directory before this "
                  "server starts again: a start that finds it attaches to "
                  "it without refusing. ");
      }
      if (!removed.empty()) {
        StrAppend(error, "It removed ", JoinCollection(removed, ", "), ". ");
      }
      if (!stuck.empty()) {
        StrAppend(error,
                  "Remove what is still there by hand before this server "
                  "starts again: a start that finds it attaches to it "
                  "without refusing. ");
      }
    }
    StrAppend(error,
              "The usual cause is start order during an upgrade: the "
              "optimizer daemon has a new on-disk cache format and has not "
              "restarted onto it yet, so its new volume does not exist for "
              "this server to attach to. Start or restart the optimizer "
              "daemon, let it create its volume, then start this server. If "
              "this server still refuses after that, the sizes genuinely "
              "disagree: install a module and daemon package pair that "
              "agree.");
    return DaemonStartupStatus::kRefuseToStart;
  }

  if (!SocketAnswers(socket_path_, &why)) {
    StrAppend(error, "in-place optimization is OFF: ", why);
    return DaemonStartupStatus::kOk;
  }

  health_ = DaemonHealth::kReady;
  if (legacy_generation_layout_) {
    // Reported even though the server is healthy: the tolerance is a
    // deliberate, working state, and one loud line is what keeps it from
    // being mistaken for the generation handshake having passed.  The same
    // latch keeps it to one line per process.
    StrAppend(error,
              "the optimizer daemon does not publish a cache directory "
              "generation (cache_dir_generation): treating it as the legacy, "
              "pre-privilege-drop layout and using the configured paths "
              "as-is. This is expected with a daemon package from before the "
              "privilege drop and disappears when the daemon is upgraded.");
  }
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
  // Kept so a serving child can repeat it once its message history exists.
  // Only a verdict that leaves the arm off is a refusal: the healthy-path
  // notes (legacy layout, left-over volumes) are not repeated.
  startup_refusal_ = health_ == DaemonHealth::kReady ? GoogleString() : error;
  if (!error.empty() && handler_ != nullptr && ShouldAnnounce(error)) {
    handler_->Message(kError, "%s", error.c_str());
  }
  return status;
}

void DaemonAdapter::ReannounceStartupRefusal() {
  if (startup_refusal_.empty() || handler_ == nullptr ||
      !ShouldReannounce(startup_refusal_)) {
    return;
  }
  // A warning: the parent already wrote the error to the server's error log;
  // this line is for the message history, which the parent could not reach.
  handler_->Message(kWarning, "%s", startup_refusal_.c_str());
}

}  // namespace net_instaweb
