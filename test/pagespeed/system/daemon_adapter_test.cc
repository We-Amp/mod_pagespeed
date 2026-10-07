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

// Startup behaviour of the optimizer-daemon adapter, its per-process volume
// handle, and the recorder factories that are built on that handle (they
// live here because this file owns the only fake that counts opens).
//
// The peer is faked at exactly ONE seam: the client-library binder.  A unit
// test cannot install a daemon package, and a fake that reproduced the
// daemon's cache would be a second implementation of the contract under test.
// So the fake stands in only for what a real peer would REPORT -- its ABI
// version, its cache-volume sizing, whether its volume opens -- and every
// other input is real: real filesystem paths, a real AF_UNIX socket for the
// reachability probe, and the adapter's real decision logic throughout.

#include "pagespeed/system/daemon_adapter.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <process.h>

#include <cerrno>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifdef _WIN32
// After windows.h: the security-API declarations it does not pull in.
#include <aclapi.h>
#include <sddl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_ipro_recorder.h"
#include "pagespeed/system/daemon_record_arm.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"

namespace net_instaweb {

namespace {

#ifdef _WIN32
// POSIX setenv/unsetenv have no direct Windows spelling; _putenv_s covers
// both, with an empty value for unset.
int setenv(const char* name, const char* value, int) {
  return _putenv_s(name, value);
}
int unsetenv(const char* name) { return _putenv_s(name, ""); }

// The CRT's narrow directory and file calls carry the ANSI code page, not
// UTF-8, so the non-ASCII fixtures below go through the wide APIs directly,
// converting exactly the way the adapter's own scan does.
bool Utf8ToWide(const GoogleString& utf8, std::wstring* out) {
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

// Creates (or truncates) one file at a wide path; false on failure.
bool TouchWideFile(const std::wstring& path) {
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  CloseHandle(handle);
  return true;
}

// Makes a directory the CURRENT process user cannot list -- when a deny ACE
// binds this process; an ELEVATED token's backup and restore privileges
// bypass read denials, so the cases that need the refused listing ASK the
// OS, with one raw listing call, whether the deny bound, and skip with a
// reason when it did not (the exact analogue of the POSIX cases skipping
// under root; the ask never goes through the adapter under test).  The deny
// ACE
// names FILE_LIST_DIRECTORY only, on the current user's own SID: WRITE_DAC
// is not denied, and the owner of an object always holds WRITE_DAC, which
// is exactly what makes the restore possible.  The original DACL is put
// back when the fixture goes away, so the fixture's cleanup can still
// delete what is inside.
class DeniedDirectory {
 public:
  explicit DeniedDirectory(const GoogleString& dir) : dir_(dir) {
    if (!Utf8ToWide(dir, &wide_dir_)) {
      return;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<char> user(needed);
    if (needed > 0 &&
        GetTokenInformation(token, TokenUser, user.data(), needed, &needed)) {
      const PTOKEN_USER token_user = reinterpret_cast<PTOKEN_USER>(user.data());
      if (token_user->User.Sid != nullptr) {
        ApplyDeny(token_user->User.Sid);
      }
    }
    CloseHandle(token);
  }

  ~DeniedDirectory() {
    if (applied_) {
      // Restore the ORIGINAL DACL before anything tries to list or delete
      // the directory; the ACL pointers live in the descriptor's storage,
      // which is freed only afterwards.
      SetNamedSecurityInfoW(wide_dir_.data(), SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION, nullptr, nullptr,
                            original_dacl_, nullptr);
      applied_ = false;
    }
    if (descriptor_ != nullptr) {
      LocalFree(descriptor_);
    }
  }

  DeniedDirectory(const DeniedDirectory&) = delete;
  DeniedDirectory& operator=(const DeniedDirectory&) = delete;

  bool denied() const { return applied_; }

 private:
  // One deny ACE for FILE_LIST_DIRECTORY on `sid`, merged into the current
  // DACL.  SetEntriesInAclW produces canonical order, which puts deny ACEs
  // ahead of the allows already there, so the deny binds even though the
  // same SID also holds an allow.
  void ApplyDeny(PSID sid) {
    EXPLICIT_ACCESSW deny;
    ZeroMemory(&deny, sizeof(deny));
    deny.grfAccessPermissions = FILE_LIST_DIRECTORY;
    deny.grfAccessMode = DENY_ACCESS;
    deny.grfInheritance = NO_INHERITANCE;
    deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    deny.Trustee.TrusteeType = TRUSTEE_IS_USER;

    PACL original = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(wide_dir_.c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &original, nullptr,
                              &descriptor) != ERROR_SUCCESS) {
      return;
    }
    // The descriptor owns the ACL it hands out; keep the descriptor and the
    // alias together for the restore, and free both in the destructor.
    PACL merged = nullptr;
    if (SetEntriesInAclW(1, &deny, original, &merged) == ERROR_SUCCESS) {
      if (SetNamedSecurityInfoW(wide_dir_.data(), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                merged, nullptr) == ERROR_SUCCESS) {
        original_dacl_ = original;
        descriptor_ = descriptor;
        applied_ = true;
        descriptor = nullptr;
      }
    }
    if (merged != nullptr) {
      LocalFree(merged);
    }
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
  }

  GoogleString dir_;
  std::wstring wide_dir_;
  PACL original_dacl_ = nullptr;
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  bool applied_ = false;
};
#endif  // _WIN32

// The remediation fragment the permission cases assert on: POSIX names the
// `pagespeed` group join, Windows the access grant on the cache directory.
#ifdef _WIN32
const char kHintFragment[] = "IIS_IUSRS";
#else
const char kHintFragment[] = "not in the `pagespeed` group";
#endif

// A daemon that reports whatever the test tells it to, and — crucially —
// MODELS the one behaviour the mirror is written against: the volume's
// filename is a function of the size it is opened with, and opening creates
// the file if it is not there.  Without that, every assertion about "did we
// attach or did we author a second volume" would be asserting against a mock
// that cannot express the failure.  The geohash itself is not reproduced (that
// would be a second implementation of the peer's naming); a distinguishable
// function of the size is enough and is what the test needs.
class FakeDaemonAbi : public DaemonAbi {
 public:
  FakeDaemonAbi(GoogleString volume_path, uint64_t published_size,
                bool publishes_size, bool sized_init)
      : volume_path_(std::move(volume_path)),
        published_size_(published_size),
        publishes_size_(publishes_size),
        sized_init_(sized_init) {}

  int VersionMajor() const override { return kRequiredAbiMajor; }
  int VersionMinor() const override { return kRequiredAbiMinor; }

  void CacheConfigInit(PsCacheConfig* config) const override {
    // Models the REAL dispatch, including the part that matters most: which
    // number ends up in struct_size.  The sized spelling records the size it
    // is GIVEN (the loader passes sizeof(PsCacheConfig)); the unsized one
    // records ITS OWN sizeof, which is how a grown peer becomes detectable.
    // A fake that stamped this build's size in both modes would agree with
    // the peer only by accident, and would hide exactly the disagreement the
    // loader's handshake exists to catch.
    if (sized_init_) {
      const size_t writes = std::min(sizeof(PsCacheConfig), grown_struct_bytes);
      observed_write_bytes = writes;
      memset(config, 0, writes);
      config->struct_size = sizeof(PsCacheConfig);
    } else {
      observed_write_bytes = grown_struct_bytes;
      memset(config, 0, grown_struct_bytes);
      config->struct_size = grown_struct_bytes;
    }
    config->volume_size = 1ULL << 30;  // the peer's compiled-in default
    config->ram_cache_size = 8 << 20;  // a peer that DOES offer a RAM tier
    config->max_metadata_size = 4096;
  }

  bool HasSizedCacheConfigInit() const override { return sized_init_; }

  uint64_t SharedConfigVolumeSize(const char* volume_path) const override {
    if (!publishes_size_ || republishes_nothing) {
      return 0;
    }
    return republished_size != 0 ? republished_size : published_size_;
  }

  bool PublishesVolumeSize() const override { return publishes_size_; }

  uint32_t SharedConfigGeneration(const char* volume_path) const override {
    return publishes_generation ? published_generation : 0;
  }

  bool PublishesGeneration() const override { return publishes_generation; }

  int CacheOpen(const PsCacheConfig* config, void** out_cache) const override {
    // A test can hold the open here until it says go: the witness flips as
    // the open is reached, so another thread can measure against a known
    // state, and the gate releases when it stores non-zero.
    if (open_gate_witness != nullptr) {
      open_gate_witness->store(true);
    }
    while (open_gate != nullptr && open_gate->load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    observed_ram_cache_size = config->ram_cache_size;
    observed_volume_size = config->volume_size;
    ++opens;
    if (open_error != kPsOk) {
      return open_error;
    }
    // Name the file from the size, and create it if absent — exactly the
    // behaviour that turns a size disagreement into a second, cold cache.
    const GoogleString name = VolumeFileFor(volume_path_, config->volume_size);
    FILE* f = fopen(name.c_str(), "a");
    if (f != nullptr) fclose(f);
    // Fires after the file exists and before anything else runs, so a test
    // can make this exact name undeletable and watch the refusal's own
    // removal fail against it.
    if (after_open != nullptr) {
      after_open(name);
    }
    *out_cache = const_cast<FakeDaemonAbi*>(this);
    return kPsOk;
  }

  void CacheClose(void* cache) const override {
    ++closes;
    if (external_closes != nullptr) {
      ++(*external_closes);
    }
  }
  const char* StrError(int error) const override { return "fake error"; }

  // The peer's per-failure explanation, or its absence.  Both are real
  // states: the entry point is bound optionally, so a daemon package from
  // before it was published answers nullptr and the adapter must still
  // produce a usable line.
  const char* LastErrorMessage() const override {
    return publishes_last_error ? last_error_message : nullptr;
  }

  // The record arm, which nothing in THIS file exercises: these cases are
  // about startup, and the arm has its own tests driven through the real
  // loader against the real stand-in library.  Every one of them therefore
  // refuses rather than quietly succeeding -- a fake that returned success
  // here would let a startup case accidentally "record" something and read
  // as evidence about an arm it never touched.
  void WriteParamsInit(PsWriteParams* params) const override {
    memset(params, 0, sizeof(*params));
    params->struct_size = sizeof(*params);
  }
  void NotifyParamsInit(PsNotifyParams* params) const override {
    memset(params, 0, sizeof(*params));
    params->struct_size = sizeof(*params);
  }
  int CacheWriteOriginal(void* /*cache*/, const char* /*url*/,
                         const char* /*hostname*/, const char* /*scheme*/,
                         const PsWriteParams* /*params*/,
                         void** /*out_handle*/) const override {
    return kPsErrInvalidArg;
  }
  int WriteData(void* /*handle*/, const void* /*data*/,
                size_t /*length*/) const override {
    return kPsErrInvalidArg;
  }
  int WriteClose(void* /*handle*/) const override { return kPsErrInvalidArg; }
  void WriteAbort(void* /*handle*/) const override {}
  int VaryUncacheable(const char* /*vary*/) const override { return 1; }
  // Refuses everything, and this pair is CONSISTENT rather than merely both
  // negative: the peer guarantees that a response it refuses never varies on
  // `Accept`, so a fake that refused storage and still claimed the axis would
  // be a peer no real library can be.
  int VaryVariesAccept(const char* /*vary*/) const override { return 0; }
  int ParseCacheControl(const char* /*header_value*/,
                        PsCacheControl* /*out*/) const override {
    return kPsErrInvalidArg;
  }
  uint32_t AgeAdjustedInsertTime(uint32_t now_seconds,
                                 uint32_t /*age_seconds*/) const override {
    return now_seconds;
  }
  uint32_t Classify(const char* /*accept*/, const char* /*user_agent*/,
                    const char* /*save_data*/,
                    const char* /*accept_encoding*/) const override {
    return 0;
  }
  int ClassifyContentType(const char* /*header*/) const override { return 4; }
  int OptionContextSignature(const char* /*payload*/, size_t /*length*/,
                             char* /*out*/,
                             size_t /*out_size*/) const override {
    return kPsErrInvalidArg;
  }
  int NotifyWorker(const char* /*socket_path*/,
                   const PsNotifyParams* /*params*/) const override {
    return kPsErrInvalidArg;
  }

  // The serve arm, on exactly the same terms as the record arm above: this
  // file is about STARTUP, and every read here answers "not found" rather
  // than quietly producing an entry.  A fake that served something would let
  // a startup case read as evidence about an arm it never touched, and the
  // serve arm has its own suite driven through the real loader against the
  // real stand-in.
  int CacheReadBest(void* /*cache*/, const char* /*url*/,
                    const char* /*hostname*/, const char* /*scheme*/,
                    uint32_t /*mask*/, void** out_result) const override {
    *out_result = nullptr;
    return kPsErrNotFound;
  }
  int CacheReadAlternate(void* /*cache*/, const char* /*url*/,
                         const char* /*hostname*/, const char* /*scheme*/,
                         uint8_t /*alternate_id*/,
                         void** out_result) const override {
    *out_result = nullptr;
    return kPsErrNotFound;
  }
  int ReadContent(const void* /*result*/, const uint8_t** /*out_data*/,
                  size_t* /*out_length*/) const override {
    return kPsErrInvalidArg;
  }
  uint32_t ReadMask(const void* /*result*/) const override { return 0; }
  uint8_t ReadFlags(const void* /*result*/) const override { return 0; }
  int ReadContentType(const void* /*result*/) const override { return 4; }
  const char* ReadOriginContentType(const void* /*result*/) const override {
    return nullptr;
  }
  uint32_t ReadCacheInsertedAt(const void* /*result*/) const override {
    return 0;
  }
  uint32_t ReadOriginMaxAge(const void* /*result*/) const override { return 0; }
  uint32_t ReadOriginSMaxAge(const void* /*result*/) const override {
    return 0;
  }
  uint16_t ReadOriginCcFlags(const void* /*result*/) const override {
    return 0;
  }
  uint32_t ReadOriginLastModified(const void* /*result*/) const override {
    return 0;
  }
  const char* ReadOriginEtag(const void* /*result*/) const override {
    return nullptr;
  }
  const uint8_t* ReadOriginHtmlHash(const void* /*result*/) const override {
    return nullptr;
  }
  int ReadIsWorkerProcessed(const void* /*result*/) const override { return 0; }
  uint32_t ReadOriginContentLength(const void* /*result*/) const override {
    return 0;
  }
  void ReadFree(void* /*result*/) const override {}
  int EvaluateFreshness(const PsFreshnessInput* /*input*/,
                        const PsFreshnessConfig* /*config*/,
                        PsFreshnessResult* /*out*/) const override {
    return kPsErrInvalidArg;
  }
  int BuildCacheControl(const PsCacheControlInput* /*input*/, char* /*buf*/,
                        size_t /*capacity*/, size_t* /*out_len*/,
                        uint32_t* /*out_final_max_age*/) const override {
    return kPsErrInvalidArg;
  }
  int ServeStatsOpen(const char* /*cache_path*/,
                     void** out_handle) const override {
    *out_handle = nullptr;
    return kPsErrNotFound;
  }
  void ServeStatsRecordServeClass(void* /*handle*/, int /*serve_class*/,
                                  uint32_t /*flags*/) const override {}
  void ServeStatsRecordHit(void* /*handle*/, int /*content_type*/,
                           uint64_t /*original_bytes*/,
                           uint64_t /*optimized_bytes*/,
                           uint32_t /*mask*/) const override {}
  void ServeStatsClose(void* /*handle*/) const override {}

  // `<stem>-6-<size>` — the shape DaemonAdapter::VolumeFiles recognises.
  // The digit is the cache's on-disk FORMAT MAJOR.  Neither the matcher nor
  // this fake reads it: `VolumeFiles` requires a digit after the separator
  // and nothing more, so every case below is about where the "-N-<size>"
  // insert LANDS relative to the extension, not about which N it is.  The
  // literal 6 is the major that was in force when these were written and is
  // deliberately left alone across format bumps; pinning it would assert
  // something the code does not check.
  // For an extensioned stem the real cache layer INSERTS "-6-<size>" before
  // the extension ("cache.vol" -> "cache-6-<size>.vol"), splitting on the
  // LAST dot the way std::filesystem does: a leading dot is no extension
  // (".vol" -> ".vol-6-<size>"), a trailing dot is a bare-dot one ("cache."
  // -> "cache-6-<size>."), and a multi-dot name splits at its last dot
  // ("cache.vol.small" -> "cache.vol-6-<size>.small").  The split runs on
  // the BASENAME, as std::filesystem's does -- a naive full-path split would
  // treat a leading-dot basename (or a dotted directory) as an extension and
  // derive a name the real peer never writes.  The fake models all of it:
  // the adapter's matcher is written against exactly this naming, so a fake
  // that only knew the append form could not express the case.
  static GoogleString VolumeFileFor(const GoogleString& path, uint64_t size) {
    const GoogleString inserted =
        StrCat("-6-", Integer64ToString(static_cast<int64>(size)));
    const size_t slash = path.find_last_of('/');
    const GoogleString parent = slash == GoogleString::npos
                                    ? GoogleString()
                                    : path.substr(0, slash + 1);
    const GoogleString base =
        slash == GoogleString::npos ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != GoogleString::npos && dot > 0) {
      return StrCat(parent, base.substr(0, dot), inserted, base.substr(dot));
    }
    return StrCat(path, inserted);
  }

  size_t grown_struct_bytes = sizeof(PsCacheConfig);
  int open_error = kPsOk;
  // Called after every open, attaching ones included, with the volume-file
  // name the open used, once the file exists.  Set by a test that needs the
  // name to be undeletable.
  std::function<void(const GoogleString& name)> after_open;
  // The reason the peer keeps beside the code, and whether it publishes one.
  // The default text is the field failure this seam was added for: the class
  // ("Input/output error") said nothing, the reason said exactly what to fix.
  bool publishes_last_error = true;
  const char* last_error_message =
      "could not set its mode to 0660 (Operation not permitted)";
  // The generation the fake's shared config reports, and whether it reports
  // one at all.  Defaults are the post-H1 contract (generation published,
  // matching this build); a test moves them to play a pre-H1 daemon or a
  // skewed one.
  bool publishes_generation = true;
  uint32_t published_generation = DaemonAdapter::kCacheDirGeneration;

  mutable size_t observed_ram_cache_size = 0;
  mutable uint64_t observed_volume_size = 0;
  mutable size_t observed_write_bytes = 0;
  mutable int opens = 0;
  mutable int closes = 0;
  // Set by a test that needs to count closes after the fake itself is gone
  // (adapter destruction frees the fake; the pointee must outlive it).
  int* external_closes = nullptr;
  // What the daemon publishes AFTER this fake was made: a daemon restarted
  // with another cache size while the web server kept running.  0 leaves
  // the size it was made with.
  uint64_t republished_size = 0;
  // The daemon's shared configuration has become unreadable.
  bool republishes_nothing = false;
  // Set by a test that needs the open to block until it says go.
  std::atomic<int>* open_gate = nullptr;
  std::atomic<bool>* open_gate_witness = nullptr;

 private:
  GoogleString volume_path_;
  uint64_t published_size_;
  bool publishes_size_;
  bool sized_init_;
};

#ifndef _WIN32
// Owns a listening AF_UNIX socket so the reachability probe has something real
// to connect to.
class ListeningSocket {
 public:
  explicit ListeningSocket(const GoogleString& path) : path_(path) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path_.data(), path_.size());
    bound_ = bind(fd_, reinterpret_cast<struct sockaddr*>(&address),
                  sizeof(address)) == 0 &&
             listen(fd_, 4) == 0;
  }
  ~ListeningSocket() {
    if (fd_ >= 0) close(fd_);
    unlink(path_.c_str());
  }
  bool bound() const { return bound_; }

 private:
  GoogleString path_;
  int fd_ = -1;
  bool bound_ = false;
};

// Releases the listen fd, every parked client fd and the socket file on
// every exit path -- including an ASSERT failure before the end of the case,
// which would otherwise leak them and strand the fixture's rmdir.
class BacklogFillGuard {
 public:
  BacklogFillGuard(int listen_fd, const GoogleString& path)
      : listen_fd_(listen_fd), path_(path) {}
  ~BacklogFillGuard() {
    for (const int client : parked_) {
      close(client);
    }
    if (listen_fd_ >= 0) {
      close(listen_fd_);
    }
    unlink(path_.c_str());
  }

  std::vector<int>* parked() { return &parked_; }
  bool bound() const { return bound_; }
  void set_bound() { bound_ = true; }
  // The errno the failed client connect answered, 0 when the queue filled.
  int fill_errno() const { return fill_errno_; }
  void set_fill_errno(int fill_errno) { fill_errno_ = fill_errno; }

 private:
  std::vector<int> parked_;
  int listen_fd_;
  GoogleString path_;
  bool bound_ = false;
  int fill_errno_ = 0;
};
#endif  // !_WIN32

#ifdef _WIN32
// The reachability-text fragments the absent-socket cases assert on, per
// platform: the error spelling is the OS's, and the start line is generic
// on Windows (it names no unit to start).
const char kNotFoundText[] = "cannot find the file specified";
const char kStartDaemonFragment[] = "start the optimizer daemon";
const char kNotExistsStartFragment[] =
    "The named pipe does not exist -- start the optimizer daemon";
// The pipe-permission fragment: the daemon decides who may open its pipe,
// so the sentence names that and the refusal -- no grant.
const char kPipeAccessFragment[] = "the daemon decides who may open it";
#else
const char kNotFoundText[] = "No such file or directory";
const char kStartDaemonFragment[] = "start the pagespeed-optimizer daemon";
const char kNotExistsStartFragment[] =
    "The socket does not exist -- start the pagespeed-optimizer daemon";
#endif

#ifdef _WIN32
namespace {
// Spell a configured pipe base name the way the operator recognises it (and
// the way the probe's messages do).
GoogleString FullPipeName(const GoogleString& base) {
  return StrCat("\\\\.\\pipe\\", base);
}
bool Utf8ToWidePath(const GoogleString& utf8, std::wstring* out) {
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
}  // namespace

// The raw-OS skip condition for the refused-listing cases: whether a
// listing of `dir` is refused with access denied RIGHT NOW.  Decided by the
// OS call itself, never by the adapter under test -- a broken adapter must
// not be able to turn its own tests into skips.  A listing that fails for
// any other reason is reported through `unexpected` (the case fails with
// the code; it does not skip).
bool DirectoryListingIsRefused(const GoogleString& dir, DWORD* unexpected) {
  *unexpected = 0;
  std::wstring wide;
  if (!Utf8ToWidePath(dir, &wide)) {
    *unexpected = ERROR_INVALID_PARAMETER;
    return false;
  }
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW((wide + L"\\*").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    FindClose(find);
    return false;
  }
  const DWORD code = GetLastError();
  if (code == ERROR_ACCESS_DENIED) {
    return true;
  }
  *unexpected = code;
  return false;
}
// A pipe whose DACL denies the current user, so a connect meets access
// denied -- WHEN the denial binds this process.  The cases that need it ask
// the OS for the probe's own first answer and skip with the observed code
// when the denial does not bind (the same elevation caveat the refused
// -listing cases carry).
class DeniedPipe {
 public:
  explicit DeniedPipe(const GoogleString& base) : full_(FullPipeName(base)) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return;
    }
    wchar_t* sid_text = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    do {
      DWORD needed = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
      std::vector<char> user(needed);
      if (needed == 0 || !GetTokenInformation(token, TokenUser, user.data(),
                                              needed, &needed)) {
        break;
      }
      const PTOKEN_USER token_user = reinterpret_cast<PTOKEN_USER>(user.data());
      if (token_user->User.Sid == nullptr ||
          !ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
        break;
      }
      // Deny FILE_ALL_ACCESS for the current user, nothing else: the
      // creator's own handle is unaffected.
      const std::wstring sddl_text =
          std::wstring(L"D:(D;;FA;;;") + sid_text + L")";
      SECURITY_ATTRIBUTES attributes;
      ZeroMemory(&attributes, sizeof(attributes));
      attributes.nLength = sizeof(attributes);
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
              sddl_text.c_str(), SDDL_REVISION_1,
              &attributes.lpSecurityDescriptor, nullptr)) {
        break;
      }
      std::wstring wide;
      if (Utf8ToWidePath(full_, &wide)) {
        instance_ =
            CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                             4096, 4096, 0, &attributes);
      }
      descriptor = attributes.lpSecurityDescriptor;
    } while (false);
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
    if (sid_text != nullptr) {
      LocalFree(sid_text);
    }
    CloseHandle(token);
  }

  ~DeniedPipe() {
    if (instance_ != INVALID_HANDLE_VALUE) {
      CloseHandle(instance_);
    }
  }

  DeniedPipe(const DeniedPipe&) = delete;
  DeniedPipe& operator=(const DeniedPipe&) = delete;

  bool created() const { return instance_ != INVALID_HANDLE_VALUE; }

  // Whether OPENING the pipe is refused for this process.  The guard is a
  // raw CreateFileW -- the probe's own first call, not the wait:
  // WaitNamedPipeW performs no access check, so it can succeed (or report
  // absence) for a pipe this identity may not open, and a guard built on it
  // would skip everywhere while the denied branch never ran anywhere.
  // Success means the deny does not bind this token; ERROR_ACCESS_DENIED
  // means it does; anything else is surfaced through `observed_error` for
  // the case to fail with.
  bool access_is_denied_for_this_process() const {
    std::wstring wide;
    if (!Utf8ToWidePath(full_, &wide)) {
      return false;
    }
    HANDLE pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe);
      observed_error_ = 0;
      return false;
    }
    observed_error_ = GetLastError();
    return observed_error_ == ERROR_ACCESS_DENIED;
  }

  DWORD observed_error() const { return observed_error_; }

 private:
  GoogleString full_;
  HANDLE instance_ = INVALID_HANDLE_VALUE;
  mutable DWORD observed_error_ = 0;
};

// The ListeningSocket twin: a named-pipe server on its own thread, offering
// instances of `base` for as long as it lives.  The next instance is created
// BEFORE the served one is dropped, so a connecting probe never meets a
// moment with no instance at all; each probe opens and closes at once and
// the server loops.  The connect wait runs OVERLAPPED alongside a stop
// event, so teardown only signals -- no handle is closed out from under a
// wait, and the destructor's join can never block on an unbounded wait
// (the server leaves its last handle open for the destructor to close).
class ListeningSocket {
 public:
  explicit ListeningSocket(const GoogleString& base)
      : full_(FullPipeName(base)) {
    std::wstring wide;
    if (!Utf8ToWidePath(full_, &wide)) {
      return;
    }
    wide_ = wide;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    connect_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stop_event_ == nullptr || connect_event_ == nullptr) {
      return;
    }
    thread_ = std::thread([this] { Serve(); });
    // Bounded wait for the first instance to exist.
    for (int i = 0; i < 200 && !bound_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  ~ListeningSocket() {
    stop_.store(true);
    if (stop_event_ != nullptr) {
      SetEvent(stop_event_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    // The server never closes the instance it is waiting on; it leaves it
    // registered for the destructor, after the join.
    HANDLE leftover = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(mu_);
      leftover = current_;
    }
    if (leftover != INVALID_HANDLE_VALUE) {
      CloseHandle(leftover);
    }
    if (connect_event_ != nullptr) {
      CloseHandle(connect_event_);
    }
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
    }
  }

  bool bound() const { return bound_.load(); }

 private:
  HANDLE MakeInstance() {
    return CreateNamedPipeW(
        wide_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        2 /* the rotation holds two */, 4096, 4096, 0, nullptr);
  }

  void Serve() {
    HANDLE instance = MakeInstance();
    if (instance == INVALID_HANDLE_VALUE) {
      return;
    }
    bound_.store(true);
    for (;;) {
      // A member, not a local: on the stop path the connect is still
      // pending when this thread returns, and the kernel completes it into
      // this structure when the destructor closes the instance.
      ZeroMemory(&overlapped_, sizeof(overlapped_));
      overlapped_.hEvent = connect_event_;
      ResetEvent(connect_event_);
      BOOL connected = ConnectNamedPipe(instance, &overlapped_);
      if (!connected) {
        const DWORD why = GetLastError();
        // ERROR_NO_DATA: a client opened and closed between this instance's
        // creation and this call -- a probe that already came and went,
        // handled like any other served connection.
        if (why == ERROR_PIPE_CONNECTED || why == ERROR_NO_DATA) {
          connected = TRUE;
        } else if (why != ERROR_IO_PENDING) {
          break;
        }
      }
      if (!connected) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          current_ = instance;
        }
        const HANDLE waits[2] = {connect_event_, stop_event_};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        {
          std::lock_guard<std::mutex> lock(mu_);
          // On stop, LEAVE the instance registered for the destructor,
          // which closes it after the join (the pending connect completes
          // into `overlapped_`); on connect, deregister it -- the loop
          // below owns it from here.
          current_ = (wait == WAIT_OBJECT_0) ? INVALID_HANDLE_VALUE : instance;
        }
        if (wait != WAIT_OBJECT_0) {
          return;
        }
      }
      // A client attached and, for the probe, closed at once.  The NEXT
      // instance exists before this one is dropped, so the namespace never
      // has a no-instance moment.
      HANDLE next = MakeInstance();
      DisconnectNamedPipe(instance);
      CloseHandle(instance);
      if (next == INVALID_HANDLE_VALUE) {
        return;
      }
      instance = next;
      if (stop_.load()) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          current_ = instance;
        }
        return;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (current_ == INVALID_HANDLE_VALUE) {
        current_ = instance;
      }
    }
  }

  GoogleString full_;
  std::wstring wide_;
  OVERLAPPED overlapped_ = {};
  std::thread thread_;
  std::atomic<bool> bound_{false};
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  HANDLE current_ = INVALID_HANDLE_VALUE;
  HANDLE stop_event_ = nullptr;
  HANDLE connect_event_ = nullptr;
};

// The BacklogFillGuard twin: a pipe with ONE instance, taken and held for
// this object's life, so every later probe meets the all-instances-taken
// state -- deterministically, which is why the backlog cases run rather
// than skip on this platform.
class BacklogFillGuard {
 public:
  explicit BacklogFillGuard(const GoogleString& base)
      : full_(FullPipeName(base)) {
    std::wstring wide;
    if (!Utf8ToWidePath(full_, &wide)) {
      return;
    }
    server_ =
        CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                         1 /* exactly one instance */, 4096, 4096, 0, nullptr);
    if (server_ == INVALID_HANDLE_VALUE) {
      return;
    }
    bound_ = true;
    client_ = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    if (client_ == INVALID_HANDLE_VALUE) {
      return;
    }
    // Wait until the server side has seen the connection: from here the one
    // instance is taken and the busy state is in force.
    if (ConnectNamedPipe(server_, nullptr) ||
        GetLastError() == ERROR_PIPE_CONNECTED) {
      held_ = true;
    }
  }

  ~BacklogFillGuard() {
    if (client_ != INVALID_HANDLE_VALUE) {
      CloseHandle(client_);
    }
    if (server_ != INVALID_HANDLE_VALUE) {
      DisconnectNamedPipe(server_);
      CloseHandle(server_);
    }
  }

  bool bound() const { return bound_; }
  // The full-queue state is deterministic on this platform: a fixture that
  // failed to hold its one instance is a FAILURE of the case, not a
  // condition to skip on (the POSIX lane skips when the KERNEL answers a
  // full queue with something else; this fixture creates the full queue
  // itself, so it either holds it or the case is broken).
  int fill_errno() const {
    if (!held_) {
      ADD_FAILURE() << "the busy fixture could not hold its pipe instance";
    }
    return EAGAIN;
  }

 private:
  GoogleString full_;
  HANDLE server_ = INVALID_HANDLE_VALUE;
  HANDLE client_ = INVALID_HANDLE_VALUE;
  bool bound_ = false;
  bool held_ = false;
};
#endif  // _WIN32

const uint64_t kDaemonSize = 2ULL << 30;  // NOT the peer's compiled-in default

// The uid the owner guard sees during the seam test, and the plain
// function handing it back (a capturing lambda cannot become a function
// pointer).  POSIX-only, like the guard itself; the Windows lane exercises
// the held-open form instead.
#ifndef _WIN32
uid_t g_seamed_uid = 0;
uid_t SeamedUid() { return g_seamed_uid; }
#endif

class DaemonAdapterTest : public testing::Test {
 protected:
  DaemonAdapterTest() : handler_(new NullMutex) {
    // A SHORT directory, on purpose.  AF_UNIX paths are capped at ~108 bytes
    // and the test runner's temp directory is routinely longer than that on
    // its own, which would make every socket case fail for a reason that has
    // nothing to do with the adapter.
#ifndef _WIN32
    dir_ = StrCat("/tmp/mps-daemon-", IntegerToString(getpid()));
    mkdir(dir_.c_str(), 0700);
#else
    dir_ = StrCat(GTestTempDir(), "\\mps-daemon-", IntegerToString(_getpid()));
    const int mk = _mkdir(dir_.c_str());
    EXPECT_TRUE(mk == 0 || errno == EEXIST) << dir_;
#endif
    // A forward slash before the leaf name on every platform: the fake's
    // VolumeFileFor splits the path on '/', so the separator is load-bearing
    // (mixed separators in the remainder are fine to Windows APIs).
    volume_ = StrCat(dir_, "/volume");
    // Process-wide by design, so each test starts from a clean slate.
    DaemonAdapter::ResetAnnouncementsForTesting();
    DaemonAdapter::ResetSocketVerdictsForTesting();
  }

  ~DaemonAdapterTest() override {
#ifndef _WIN32
    // A test that died between setting the uid seam and resetting it must
    // not leave the seam set for the tests that follow.
    DaemonAdapter::SetEffectiveUidForTesting(nullptr);
    for (const GoogleString& name : DaemonAdapter::VolumeFiles(volume_)) {
      unlink(StrCat(dir_, "/", name).c_str());
    }
    unlink(StrCat(volume_, ".gen").c_str());
    rmdir(dir_.c_str());
#else
    // The native shape of the POSIX dirent loop on the left: remove the
    // fixture's files, then the directory.
    const GoogleString pattern = StrCat(dir_, "\\*");
    struct _finddata_t found;
    const intptr_t handle = _findfirst(pattern.c_str(), &found);
    if (handle != -1) {
      do {
        if ((found.attrib & _A_SUBDIR) == 0) {
          _unlink(StrCat(dir_, "\\", found.name).c_str());
        }
      } while (_findnext(handle, &found) == 0);
      _findclose(handle);
    }
    _rmdir(dir_.c_str());
#endif
  }

  // Stand in for the daemon having already created its volume at `size`.
  void DaemonCreatedItsVolume(uint64_t size) {
    FILE* f = fopen(FakeDaemonAbi::VolumeFileFor(volume_, size).c_str(), "a");
    ASSERT_TRUE(f != nullptr);
    fclose(f);
  }

  // Stand in for the daemon having replaced its volume: a full cache purge
  // rewrites "<volume path>.gen" with the next number and a newline.
  void DaemonPublishedGeneration(uint64_t generation) {
    FILE* f = fopen(StrCat(volume_, ".gen").c_str(), "w");
    ASSERT_TRUE(f != nullptr);
    fprintf(f, "%llu\n", static_cast<unsigned long long>(generation));
    fclose(f);
  }

  // The generation file with exactly `text` in it.
  void WriteGenerationFile(const GoogleString& text) {
    FILE* f = fopen(StrCat(volume_, ".gen").c_str(), "w");
    ASSERT_TRUE(f != nullptr);
    fputs(text.c_str(), f);
    fclose(f);
  }

  DaemonAdapter* MakeAdapter(const GoogleString& socket_path) {
    DaemonAdapter* adapter = new DaemonAdapter(socket_path, volume_, &handler_);
    adapter->set_abi_loader(
        [this](StringPiece path, GoogleString* error) -> DaemonAbi* {
          // Serialised so two tests can drive adapters from two threads.
          std::lock_guard<std::mutex> lock(abi_loader_mutex_);
          if (!library_available_) {
            StrAppend(error, "no library at ", path);
            return nullptr;
          }
          FakeDaemonAbi* abi = new FakeDaemonAbi(volume_, published_size_,
                                                 publishes_size_, sized_init_);
          abi->open_error = fake_open_error_;
          abi->grown_struct_bytes = grown_struct_bytes_;
          abi->publishes_generation = publishes_generation_;
          abi->published_generation = published_generation_;
          abi->publishes_last_error = publishes_last_error_;
          abi->last_error_message = last_error_message_;
          abi->after_open = fake_after_open_;
          last_abi_ = abi;
          return abi;
        });
    // Every case drives the retry schedule off `now_ms_`; nothing sleeps.
    adapter->set_monotonic_clock([this]() { return now_ms_; });
    return adapter;
  }

  // An adapter that has reached kReady, ready for the record arm.  The
  // record-cache open is a SECOND open, after startup's, so a case can make
  // startup succeed and that one fail -- which is the field failure: the
  // parent process could open the volume and the dropped children could not.
  // `socket_path` must already be listening.
  DaemonAdapter* ReadyAdapter(const GoogleString& socket_path) {
    DaemonCreatedItsVolume(kDaemonSize);
    DaemonAdapter* adapter = MakeAdapter(socket_path);
    EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
    EXPECT_EQ(DaemonHealth::kReady, adapter->health());
    return adapter;
  }

  GoogleString SocketPath(const GoogleString& name) {
#ifdef _WIN32
    // A pipe BASE name, unique per process and per call: the pipe namespace
    // is machine-global, and a leftover from a previous run must not answer
    // a case that expects an absent endpoint.
    static std::atomic<int> pipe_counter(0);
    return StrCat("mps-", IntegerToString(_getpid()), "-",
                  IntegerToString(pipe_counter.fetch_add(1)), "-", name);
#else
    return StrCat(dir_, "/", name);
#endif
  }

#ifndef _WIN32
  // Binds a listen(fd, 0) socket at `socket_path` and parks non-blocking
  // clients until its accept queue answers EAGAIN, so a probe meets a full
  // queue. The loop is bounded so a kernel that will not fill it cannot
  // spin. A kernel that answers a full queue with something other than
  // EAGAIN is reported through the guard's fill_errno() for the caller to
  // skip on, rather than failing the case for the wrong reason. The
  // returned guard owns the fds and the socket file.
  std::unique_ptr<BacklogFillGuard> FillBackloggedQueue(
      const GoogleString& socket_path) {
    const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_GE(listen_fd, 0);
    auto guard = std::make_unique<BacklogFillGuard>(listen_fd, socket_path);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
      ADD_FAILURE() << "socket path too long for sun_path: " << socket_path;
      return guard;
    }
    memcpy(address.sun_path, socket_path.data(), socket_path.size());
    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&address),
             sizeof(address)) == 0 &&
        listen(listen_fd, 0) == 0) {
      guard->set_bound();
    } else {
      ADD_FAILURE() << "cannot bind/listen on " << socket_path;
    }
    std::vector<int>& parked = *guard->parked();
    for (;;) {
      const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
      EXPECT_GE(client, 0);
      if (connect(client, reinterpret_cast<struct sockaddr*>(&address),
                  sizeof(address)) != 0) {
        const int fill_errno = errno;
        close(client);
        guard->set_fill_errno(fill_errno);
        break;
      }
      parked.push_back(client);
      if (parked.size() >= 1000u) {
        ADD_FAILURE() << "the accept queue never fills";
        guard->set_fill_errno(0);
        break;
      }
    }
    return guard;
  }

  // True when this kernel answered the full accept queue with EAGAIN, the
  // answer the probe's backlog path is built on.  The skip itself stays in
  // the test body: GTEST_SKIP() returns from the function it is written in.
  bool QueueFilledWithEagain(const BacklogFillGuard& fill) {
    return fill.fill_errno() == EAGAIN;
  }
#else
  // The Windows twin: ONE pipe instance, created and held, is the full
  // queue -- deterministically, with no kernel-dependent fill errno to
  // skip on.  Every later probe meets an instance that will not free.
  std::unique_ptr<BacklogFillGuard> FillBackloggedQueue(
      const GoogleString& socket_path) {
    return std::make_unique<BacklogFillGuard>(socket_path);
  }

  bool QueueFilledWithEagain(const BacklogFillGuard& fill) {
    return fill.fill_errno() == EAGAIN;
  }
#endif  // !_WIN32

  // Everything the handler has logged so far, for content assertions.
  GoogleString Messages() {
    GoogleString dump;
    StringWriter writer(&dump);
    handler_.Dump(&writer);
    return dump;
  }

  GoogleString dir_;
  GoogleString volume_;
  MockMessageHandler handler_;
  std::mutex abi_loader_mutex_;
  bool library_available_ = true;
  bool publishes_size_ = true;
  bool sized_init_ = true;
  uint64_t published_size_ = kDaemonSize;
  size_t grown_struct_bytes_ = sizeof(PsCacheConfig);
  int fake_open_error_ = kPsOk;
  bool publishes_generation_ = true;
  bool publishes_last_error_ = true;
  const char* last_error_message_ =
      "could not set its mode to 0660 (Operation not permitted)";
  uint32_t published_generation_ = DaemonAdapter::kCacheDirGeneration;
  // Wired onto every fake this fixture makes; a case sets it to make the
  // volume file the open creates undeletable.
  std::function<void(const GoogleString& name)> fake_after_open_;
  // The retry schedule's clock.  Advanced by a case, never by time passing.
  int64_t now_ms_ = 0;
  FakeDaemonAbi* last_abi_ = nullptr;

  // The handler's full text, for asserting what a message says.
  GoogleString DumpedMessages() {
    GoogleString dumped;
    StringWriter writer(&dumped);
    handler_.Dump(&writer);
    return dumped;
  }
};

// --- unconfigured -----------------------------------------------------------

TEST_F(DaemonAdapterTest, NoDaemonConfiguredIsSilentAndStarts) {
  DaemonAdapter adapter("", "", &handler_);
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter.StartupCheck());
  EXPECT_EQ(DaemonHealth::kNotConfigured, adapter.health());
  EXPECT_EQ(0, handler_.TotalMessages());
}

TEST_F(DaemonAdapterTest, HalfAConfigurationDegradesButStillStarts) {
  DaemonAdapter adapter("/nonexistent/socket", "", &handler_);
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter.StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter.health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
}

// --- daemon absent / unusable ----------------------------------------------

TEST_F(DaemonAdapterTest, AbsentLibraryTurnsIproOffWithOneLoudLine) {
  library_available_ = false;
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(SocketPath("a.sock")));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterTest, TheLoudLineIsOncePerProcessNotOncePerAdapter) {
  // The deployment doc tells an operator to read the error log once, and a
  // server rebuilds its contexts during startup — so a per-object latch would
  // make that instruction wrong.  Distinct ADAPTERS, same condition, one line.
  library_available_ = false;
  for (int i = 0; i < 5; ++i) {
    std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(SocketPath("a.sock")));
    EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  }
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterTest, ADifferentConditionIsStillHeard) {
  // ... but a second server failing a DIFFERENT way must not be swallowed.
  library_available_ = false;
  std::unique_ptr<DaemonAdapter> a(MakeAdapter(SocketPath("a.sock")));
  EXPECT_EQ(DaemonStartupStatus::kOk, a->StartupCheck());

  library_available_ = true;
  publishes_size_ = false;
  std::unique_ptr<DaemonAdapter> b(MakeAdapter(SocketPath("b.sock")));
  EXPECT_EQ(DaemonStartupStatus::kOk, b->StartupCheck());
  EXPECT_EQ(2, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterTest, UnreachableSocketTurnsIproOff) {
  DaemonCreatedItsVolume(kDaemonSize);
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(SocketPath("no.sock")));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterTest, AnAbsentSocketSaysToStartTheDaemon) {
  // One half of the H2/H3 startup distinction: a missing socket means the
  // daemon is not running, and the line carries that remediation.
  DaemonCreatedItsVolume(kDaemonSize);
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(SocketPath("gone.sock")));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kStartDaemonFragment))
      << Messages();
}

TEST_F(DaemonAdapterTest, AnAbsentSocketAnswersAtOnceWithTheSameText) {
  // The connect deadline changes nothing for a socket that is not there: the
  // same text as before, and it does not wait the deadline out.
  DaemonCreatedItsVolume(kDaemonSize);
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(SocketPath("fast.sock")));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_LT(elapsed_ms, 200) << elapsed_ms;
  EXPECT_NE(GoogleString::npos,
            Messages().find("the optimizer daemon does not answer at "))
      << Messages();
  EXPECT_NE(GoogleString::npos, Messages().find(kNotFoundText)) << Messages();
  EXPECT_NE(GoogleString::npos, Messages().find(kStartDaemonFragment))
      << Messages();
}

TEST_F(DaemonAdapterTest, ABackloggedListenerCostsTheDeadlineAndNoMore) {
  // Full-queue behaviour: a listener whose every instance is taken makes the
  // probe wait out its deadline rather than hang the start.  On POSIX the
  // queue is filled by parked clients (and a kernel that answers a full
  // queue with something other than EAGAIN skips); on Windows one held pipe
  // instance is the full queue, deterministically.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("full.sock");
  std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(socket_path);
  ASSERT_TRUE(fill->bound());
  if (!QueueFilledWithEagain(*fill)) {
    GTEST_SKIP() << "this kernel answers a full queue with "
                 << fill->fill_errno();
  }

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find("did not accept a connection"))
      << Messages();
  EXPECT_GE(elapsed_ms, 1900) << elapsed_ms;
  EXPECT_LT(elapsed_ms, 5000) << elapsed_ms;
}

TEST_F(DaemonAdapterTest, ASecondProbeOfTheSamePathInsideTheLatchIsFree) {
  // The deadline is paid once per process per path: the first probe of a
  // backlogged socket costs it, the second returns at once with the SAME
  // text -- a different text would announce a second line, because the
  // announcement latch only collapses identical messages.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("latched.sock");
  std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(socket_path);
  ASSERT_TRUE(fill->bound());
  if (!QueueFilledWithEagain(*fill)) {
    GTEST_SKIP() << "this kernel answers a full queue with "
                 << fill->fill_errno();
  }

  std::unique_ptr<DaemonAdapter> first(MakeAdapter(socket_path));
  const std::chrono::steady_clock::time_point first_start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, first->StartupCheck());
  const int64_t first_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - first_start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, first->health());
  EXPECT_GE(first_ms, 1900) << first_ms;

  std::unique_ptr<DaemonAdapter> second(MakeAdapter(socket_path));
  const std::chrono::steady_clock::time_point second_start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, second->StartupCheck());
  const int64_t second_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - second_start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, second->health());
  EXPECT_LT(second_ms, 200) << second_ms;
  EXPECT_EQ(1, handler_.MessagesOfType(kError)) << Messages();
}

TEST_F(DaemonAdapterTest, TheLatchIsPerSocketPath) {
  // An absent socket probed while a different path is latched still answers
  // at once with its own ENOENT text.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("latched2.sock");
  std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(socket_path);
  ASSERT_TRUE(fill->bound());
  if (!QueueFilledWithEagain(*fill)) {
    GTEST_SKIP() << "this kernel answers a full queue with "
                 << fill->fill_errno();
  }
  std::unique_ptr<DaemonAdapter> latched(MakeAdapter(socket_path));
  ASSERT_EQ(DaemonStartupStatus::kOk, latched->StartupCheck());
  ASSERT_EQ(DaemonHealth::kUnavailable, latched->health());

  std::unique_ptr<DaemonAdapter> absent(MakeAdapter(SocketPath("gone2.sock")));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, absent->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, absent->health());
  EXPECT_LT(elapsed_ms, 200) << elapsed_ms;
  EXPECT_NE(GoogleString::npos, Messages().find(kNotExistsStartFragment))
      << Messages();
  // Both messages were heard: their texts differ, so the announcement latch
  // does not collapse them.
  EXPECT_EQ(2, handler_.MessagesOfType(kError)) << Messages();
}

#ifdef _WIN32
TEST_F(DaemonAdapterTest, AnAbsentPipeAnswersAtOnceWhileAnotherPipeIsBusy) {
  // #1050: the probe once waited for an instance BEFORE opening, and on some
  // hosts a wait on an absent name sat out the whole deadline while another
  // pipe had every instance taken (and a probe waiting on it).  The probe
  // opens first now, so an absent pipe answers at once even then.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString busy_path = SocketPath("busy3.sock");
  std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(busy_path);
  ASSERT_TRUE(fill->bound());
  ASSERT_TRUE(QueueFilledWithEagain(*fill));

  // A probe of the busy pipe sits in its wait for the whole deadline, on
  // its own thread, while the absent pipe is probed.
  std::unique_ptr<DaemonAdapter> busy(MakeAdapter(busy_path));
  DaemonHealth busy_health = DaemonHealth::kNotConfigured;
  std::thread busy_thread([&]() {
    EXPECT_EQ(DaemonStartupStatus::kOk, busy->StartupCheck());
    busy_health = busy->health();
  });
  // Long enough for the busy probe to be inside its wait, far short of the
  // deadline it is paying.
  Sleep(200);

  std::unique_ptr<DaemonAdapter> absent(MakeAdapter(SocketPath("gone3.sock")));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, absent->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, absent->health());
  EXPECT_LT(elapsed_ms, 200) << elapsed_ms;

  busy_thread.join();
  EXPECT_EQ(DaemonHealth::kUnavailable, busy_health);
  EXPECT_NE(GoogleString::npos, Messages().find(kNotExistsStartFragment))
      << Messages();
  EXPECT_NE(GoogleString::npos, Messages().find("did not accept a connection"))
      << Messages();
  EXPECT_EQ(2, handler_.MessagesOfType(kError)) << Messages();
}
#endif  // _WIN32

TEST_F(DaemonAdapterTest, AProbeAfterTheLatchExpiryIsARealProbeAgain) {
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("expire.sock");
  std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(socket_path);
  ASSERT_TRUE(fill->bound());
  if (!QueueFilledWithEagain(*fill)) {
    GTEST_SKIP() << "this kernel answers a full queue with "
                 << fill->fill_errno();
  }
  DaemonAdapter::SetSocketVerdictLatchMsForTesting(100);

  std::unique_ptr<DaemonAdapter> first(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, first->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, first->health());

#ifdef _WIN32
  Sleep(150);
#else
  usleep(150 * 1000);
#endif

  std::unique_ptr<DaemonAdapter> second(MakeAdapter(socket_path));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, second->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, second->health());
  EXPECT_GE(elapsed_ms, 1900) << elapsed_ms;
}

TEST_F(DaemonAdapterTest, ALatchedPathThatRecoversAnswersAfterExpiry) {
  // The latch must not outlive a recovery: after the interval a probe is a
  // real probe again, and a listener that now accepts answers reachable.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("recover.sock");
  DaemonAdapter::SetSocketVerdictLatchMsForTesting(100);
  {
    std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(socket_path);
    ASSERT_TRUE(fill->bound());
    if (!QueueFilledWithEagain(*fill)) {
      GTEST_SKIP() << "this kernel answers a full queue with "
                   << fill->fill_errno();
    }
    std::unique_ptr<DaemonAdapter> blocked(MakeAdapter(socket_path));
    EXPECT_EQ(DaemonStartupStatus::kOk, blocked->StartupCheck());
    EXPECT_EQ(DaemonHealth::kUnavailable, blocked->health());
  }  // The guard drops: the parked clients and the listener close, and the
  // queue and the path are free.
  ListeningSocket accepting(socket_path);
  ASSERT_TRUE(accepting.bound());

#ifdef _WIN32
  Sleep(150);
#else
  usleep(150 * 1000);
#endif

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  EXPECT_LT(elapsed_ms, 1000) << elapsed_ms;
}

TEST_F(DaemonAdapterTest, TwoThreadsProbingOnePathCannotRaceTheLatch) {
  // Two threads may both pay the deadline once for one path; the latch must
  // not race them. Afterwards the path is latched once: a third probe
  // returns at once, and the whole exchange announced exactly one line.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("race.sock");
  std::unique_ptr<BacklogFillGuard> fill = FillBackloggedQueue(socket_path);
  ASSERT_TRUE(fill->bound());
  if (!QueueFilledWithEagain(*fill)) {
    GTEST_SKIP() << "this kernel answers a full queue with "
                 << fill->fill_errno();
  }

  std::unique_ptr<DaemonAdapter> first(MakeAdapter(socket_path));
  std::unique_ptr<DaemonAdapter> second(MakeAdapter(socket_path));
  DaemonHealth first_health = DaemonHealth::kNotConfigured;
  DaemonHealth second_health = DaemonHealth::kNotConfigured;
  std::thread first_thread([&]() {
    EXPECT_EQ(DaemonStartupStatus::kOk, first->StartupCheck());
    first_health = first->health();
  });
  std::thread second_thread([&]() {
    EXPECT_EQ(DaemonStartupStatus::kOk, second->StartupCheck());
    second_health = second->health();
  });
  first_thread.join();
  second_thread.join();
  EXPECT_EQ(DaemonHealth::kUnavailable, first_health);
  EXPECT_EQ(DaemonHealth::kUnavailable, second_health);

  std::unique_ptr<DaemonAdapter> third(MakeAdapter(socket_path));
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  EXPECT_EQ(DaemonStartupStatus::kOk, third->StartupCheck());
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_EQ(DaemonHealth::kUnavailable, third->health());
  EXPECT_LT(elapsed_ms, 200) << elapsed_ms;
  EXPECT_EQ(1, handler_.MessagesOfType(kError)) << Messages();
}

// --- permission denied vs absent (H2/H3) -----------------------------------
//
// After the daemon's privilege drop its cache directory, socket and shared
// config are group-rw (pagespeed:pagespeed), which makes "the web-server user
// is not in the `pagespeed` group" the most likely field failure -- and
// EACCES must not read as "the daemon is not there".  The chmod cases need a
// process the permission bits actually bind, so they skip under root (CI
// containers run as root, and mode 0000 is still readable there).

TEST_F(DaemonAdapterTest, VolumeDirAccessDistinguishesAbsentFromDenied) {
  EXPECT_EQ(
      DaemonAdapter::DirAccess::kAbsent,
      DaemonAdapter::VolumeDirAccess(StrCat(dir_, "/no-such-parent/volume")));
  EXPECT_EQ(DaemonAdapter::DirAccess::kReadable,
            DaemonAdapter::VolumeDirAccess(volume_));
#ifdef _WIN32
  // A deny ACE for listing on the current user stands in for mode 0000 --
  // WHEN IT BINDS.  It does not under an elevated token (whose backup and
  // restore privileges bypass read denials), which is this platform's
  // analogue of running as root; the case therefore asks the adapter
  // whether the deny bound before asserting it, and skips with the reason
  // when it did not.  The fixture restores the DACL when it goes away.
  DeniedDirectory denied(dir_);
  ASSERT_TRUE(denied.denied());
  DWORD unexpected = 0;
  if (!DirectoryListingIsRefused(dir_, &unexpected)) {
    if (unexpected != 0) {
      FAIL() << "listing the fixture directory failed with unexpected code "
             << unexpected;
    }
    GTEST_SKIP() << "a deny ACE does not bind an elevated process";
  }
  EXPECT_EQ(DaemonAdapter::DirAccess::kDenied,
            DaemonAdapter::VolumeDirAccess(volume_));
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
  ASSERT_EQ(0, chmod(dir_.c_str(), 0000));
  EXPECT_EQ(DaemonAdapter::DirAccess::kDenied,
            DaemonAdapter::VolumeDirAccess(volume_));
  EXPECT_EQ(0, chmod(dir_.c_str(), 0700));
#endif
}

TEST_F(DaemonAdapterTest, AnUnlistableCacheDirectoryNamesTheGroup) {
#ifdef _WIN32
  DaemonCreatedItsVolume(kDaemonSize);
  std::unique_ptr<DaemonAdapter> adapter(
      MakeAdapter(SocketPath("denied.sock")));
  // The degrade under test happens before the reachability probe (the probe
  // runs last in the startup check), so no listener is needed for it.
  DeniedDirectory denied(dir_);
  ASSERT_TRUE(denied.denied());
  DWORD unexpected = 0;
  if (!DirectoryListingIsRefused(dir_, &unexpected)) {
    if (unexpected != 0) {
      FAIL() << "listing the fixture directory failed with unexpected code "
             << unexpected;
    }
    GTEST_SKIP() << "a deny ACE does not bind an elevated process";
  }
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kHintFragment)) << Messages();
  // And the volume scan must not have misread the refused listing as "no
  // volume yet" -- the absent message would have sent the operator to start
  // the daemon.
  EXPECT_EQ(GoogleString::npos, Messages().find("start the daemon first"))
      << Messages();
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("denied.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  ASSERT_EQ(0, chmod(dir_.c_str(), 0000));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(0, chmod(dir_.c_str(), 0700));
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kHintFragment)) << Messages();
  // And the volume scan must not have misread EACCES as "no volume yet" --
  // the absent message would have sent the operator to start the daemon.
  EXPECT_EQ(GoogleString::npos, Messages().find("start the daemon first"))
      << Messages();
#endif
}

TEST_F(DaemonAdapterTest, AnUnreadableSharedConfigNamesTheGroup) {
  // Same distinction one state earlier: the shared config cannot be read, so
  // the size is unknown -- but the cause is permissions, not a daemon that
  // has never run.
  published_size_ = 0;
#ifdef _WIN32
  std::unique_ptr<DaemonAdapter> adapter(
      MakeAdapter(SocketPath("cfgdenied.sock")));
  // The degrade under test happens before the reachability probe (the probe
  // runs last in the startup check), so no listener is needed for it.
  DeniedDirectory denied(dir_);
  ASSERT_TRUE(denied.denied());
  DWORD unexpected = 0;
  if (!DirectoryListingIsRefused(dir_, &unexpected)) {
    if (unexpected != 0) {
      FAIL() << "listing the fixture directory failed with unexpected code "
             << unexpected;
    }
    GTEST_SKIP() << "a deny ACE does not bind an elevated process";
  }
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kHintFragment)) << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find("may not be running yet"))
      << Messages();
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
  const GoogleString socket_path = SocketPath("cfgdenied.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  ASSERT_EQ(0, chmod(dir_.c_str(), 0000));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(0, chmod(dir_.c_str(), 0700));
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kHintFragment)) << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find("may not be running yet"))
      << Messages();
#endif
}

TEST_F(DaemonAdapterTest, APermissionDeniedSocketNamesTheGroup) {
#ifdef _WIN32
  // A pipe whose DACL denies the current user: the connect meets access
  // denied and the line says the daemon refused this identity -- WHEN the
  // denial binds this process (see DeniedPipe for the elevation caveat).  When the pipe's
  // answer is success the deny does not bind this token and the case skips;
  // any answer that is neither success nor access denied fails the case
  // with its code.
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("pdenied.pipe");
  DeniedPipe denied(socket_path);
  ASSERT_TRUE(denied.created());
  if (!denied.access_is_denied_for_this_process()) {
    ASSERT_EQ(0u, denied.observed_error())
        << "opening the deny-DACL pipe failed with an unexpected code";
    GTEST_SKIP() << "the pipe's deny DACL does not bind this process";
  }

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kPipeAccessFragment))
      << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find(kStartDaemonFragment))
      << Messages();
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString subdir = StrCat(dir_, "/sub");
  ASSERT_EQ(0, mkdir(subdir.c_str(), 0700));
  const GoogleString socket_path = StrCat(subdir, "/daemon.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  ASSERT_EQ(0, chmod(subdir.c_str(), 0000));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(0, chmod(subdir.c_str(), 0700));
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find(kHintFragment)) << Messages();
  EXPECT_EQ(GoogleString::npos,
            Messages().find("start the pagespeed-optimizer daemon"))
      << Messages();
  // The listener's destructor unlinks the socket at scope exit, after this --
  // so remove it explicitly, or the rmdir below fails on a non-empty dir.
  EXPECT_EQ(0, unlink(socket_path.c_str()));
  EXPECT_EQ(0, rmdir(subdir.c_str()));
#endif
}

// --- the cache-directory generation handshake ------------------------------
//
// Since the privilege drop the daemon's cache lives in a versioned cold-start
// directory (vN) and the daemon publishes N in its shared config as
// cache_dir_generation.  A skew is a loud handshake failure; an absent field
// is the legacy layout, tolerated and announced once.

TEST_F(DaemonAdapterTest, TheCompiledGenerationIsTwo) {
  EXPECT_EQ(2u, DaemonAdapter::kCacheDirGeneration);
}

TEST_F(DaemonAdapterTest, AGenerationMismatchIsALoudHandshakeFailure) {
  // A v1 daemon vs a v2 module: the two layouts share nothing, so the
  // substrate goes down loudly -- and the volume is NEVER opened at the
  // other generation's geometry.
  published_generation_ = 1;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("skew.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find("cache_dir_generation"))
      << Messages();
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(0, last_abi_->opens);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, APreH1DaemonIsToleratedWithOneLoudLine) {
  // No generation reader at all (the rc-era daemon): the legacy layout, kept
  // working with the configured paths as-is, announced once per process.
  publishes_generation_ = false;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("legacy.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find("legacy")) << Messages();
  // And it really did keep working: attached to the daemon's volume at the
  // daemon's size, with no second file authored.
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(kDaemonSize, last_abi_->observed_volume_size);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AGenerationOfZeroIsUnknownNotAMismatch) {
  // The reader exists but the field is absent from the shared config: the
  // same tolerance as a pre-H1 daemon, and definitely not read as
  // "generation 0" refusing against a build for generation 2.
  published_generation_ = 0;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("genzero.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos, Messages().find("legacy")) << Messages();
}

// --- the start-up refusal, repeated once the message history exists -------
//
// A web server resolves the verdict in its parent process, before the shared
// message buffer is attached, so the line StartupCheck logs never reaches the
// admin console's message history.  Each serving child repeats a refusal
// once, as a warning, with the same text.

TEST_F(DaemonAdapterTest, ARefusalIsRepeatedOnceAsAWarningWithTheSameText) {
  published_generation_ = 1;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("skew-again.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_EQ(DaemonHealth::kUnavailable, adapter->health());
  ASSERT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_EQ(0, handler_.MessagesOfType(kWarning));

  adapter->ReannounceStartupRefusal();
  adapter->ReannounceStartupRefusal();
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning)) << Messages();
  EXPECT_EQ(1, handler_.MessagesOfType(kError)) << Messages();
  // The same text both times, so the console folds them into one template.
  const GoogleString dump = Messages();
  const GoogleString refusal =
      "in-place optimization is OFF: the optimizer daemon publishes cache "
      "directory generation 1 (cache_dir_generation)";
  const size_t first = dump.find(refusal);
  ASSERT_NE(GoogleString::npos, first) << dump;
  EXPECT_NE(GoogleString::npos, dump.find(refusal, first + 1)) << dump;
}

TEST_F(DaemonAdapterTest, ARepeatedRefusalIsOneLinePerProcessAcrossServers) {
  // Three virtual hosts refused for the same reason: one repeat, not three.
  published_generation_ = 1;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("skew-hosts.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  for (int i = 0; i < 3; ++i) {
    std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
    EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
    adapter->ReannounceStartupRefusal();
  }
  EXPECT_EQ(1, handler_.MessagesOfType(kError)) << Messages();
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning)) << Messages();
}

TEST_F(DaemonAdapterTest, AHealthyOrUnconfiguredStartRepeatsNothing) {
  // Healthy, with the one legacy-layout note: that note is not a refusal.
  publishes_generation_ = false;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("legacy-again.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_EQ(DaemonHealth::kReady, adapter->health());
  adapter->ReannounceStartupRefusal();
  EXPECT_EQ(1, handler_.TotalMessages()) << Messages();

  DaemonAdapter unconfigured("", "", &handler_);
  EXPECT_EQ(DaemonStartupStatus::kOk, unconfigured.StartupCheck());
  unconfigured.ReannounceStartupRefusal();
  EXPECT_EQ(1, handler_.TotalMessages()) << Messages();
}

// --- the mirror cannot be evaluated ⇒ degrade, and NEVER open --------------

TEST_F(DaemonAdapterTest, AnOlderDaemonThatPublishesNoSizeDegrades) {
  // The mandatory mirror needs the daemon's real size.  A daemon package that
  // predates the publishing entry point cannot supply it — that is a degrade,
  // not a refusal, because an out-of-step pair is not a corrupt one.
  publishes_size_ = false;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("old.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  // And it must not have touched the volume: opening at a guessed size is the
  // whole failure being guarded against.
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(0, last_abi_->opens);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AnUnknownSizeDegradesAndOpensNothing) {
  published_size_ = 0;  // no shared config yet: the daemon has never run
  const GoogleString socket_path = SocketPath("unk.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(0, last_abi_->opens);
  EXPECT_TRUE(DaemonAdapter::VolumeFiles(volume_).empty());
}

TEST_F(DaemonAdapterTest, AnUnknownSizeNeverGuessesAgainstAnExistingVolume) {
  // The precise shape of the defect this whole mechanism replaced: the daemon
  // is running with a volume on disk, the module cannot learn its size, and
  // the tempting thing to do is assume a default. Assuming one lands on a
  // DIFFERENT filename and authors a second, permanently cold cache beside a
  // perfectly good one — while reporting healthy.
  //
  // The earlier no-volume-on-disk case cannot catch this: with no volume
  // present, a guess is stopped by a different guard and the wrong code looks
  // right. Here the volume exists, so only "do not guess" saves it.
  DaemonCreatedItsVolume(64ULL << 20);
  published_size_ = 0;
  const GoogleString socket_path = SocketPath("guess.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(0, last_abi_->opens) << "the volume was opened at a guessed size";
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size())
      << "a second volume file was created beside the daemon's";
}

TEST_F(DaemonAdapterTest, AnOlderDaemonNeverGuessesAgainstAnExistingVolume) {
  // Same property, reached through the other not-known route: the installed
  // daemon package is too old to export the reader at all.
  DaemonCreatedItsVolume(64ULL << 20);
  publishes_size_ = false;
  const GoogleString socket_path = SocketPath("oldg.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(0, last_abi_->opens);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, NoVolumeOnDiskDegradesAndCreatesNothing) {
  // The daemon published a size but has not made its volume. Creating one
  // would be a cache nothing else reads.
  const GoogleString socket_path = SocketPath("novol.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_TRUE(DaemonAdapter::VolumeFiles(volume_).empty());
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(0, last_abi_->opens);
}

// --- the mirror ------------------------------------------------------------

TEST_F(DaemonAdapterTest, InheritingTheDaemonsSizeAttachesToItsVolume) {
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("ok.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  EXPECT_EQ(0, handler_.TotalMessages());
  // Opened at the DAEMON's size, which is deliberately not the peer library's
  // compiled-in default — the value a constant could ever have matched.
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(kDaemonSize, last_abi_->observed_volume_size);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AuthoringASecondVolumeRefusesToStart) {
  // The daemon's volume on disk was made at one size; the daemon now publishes
  // another.  Opening lands on a new file — a split cache — and that is the
  // one condition worth refusing a start over, because it is silent.  The
  // refused start removes the file its own open created, so the directory
  // holds exactly what it held before the check.
  DaemonCreatedItsVolume(64ULL << 20);
  // Real content, so "untouched" is a claim about bytes and the inode,
  // not just about a name and a zero length.
  const GoogleString daemon_file_name =
      FakeDaemonAbi::VolumeFileFor(volume_, 64ULL << 20);
  FILE* daemon_bytes = fopen(daemon_file_name.c_str(), "a");
  ASSERT_TRUE(daemon_bytes != nullptr);
  ASSERT_NE(EOF, fputs("the daemon's volume, with content", daemon_bytes));
  fclose(daemon_bytes);
  const std::vector<GoogleString> before = DaemonAdapter::VolumeFiles(volume_);
  struct stat daemon_file;
  ASSERT_EQ(0, stat(daemon_file_name.c_str(), &daemon_file));
  ASSERT_GT(daemon_file.st_size, 0);
  const GoogleString socket_path = SocketPath("split.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  // ONE volume file remains — the daemon's — and it is the same file,
  // the same inode with the same bytes, as before the refused check.
  EXPECT_EQ(before, DaemonAdapter::VolumeFiles(volume_));
  struct stat after_file;
  ASSERT_EQ(0, stat(daemon_file_name.c_str(), &after_file));
  EXPECT_EQ(daemon_file.st_size, after_file.st_size);
  EXPECT_EQ(daemon_file.st_ino, after_file.st_ino);
  // And the refusal says so.
  EXPECT_NE(GoogleString::npos,
            DumpedMessages().find("This start removed the file it created"));
  EXPECT_NE(GoogleString::npos,
            DumpedMessages().find(
                "so the directory holds only what it held before this start"));
}

TEST_F(DaemonAdapterTest, LeftoverVolumesFromAResizeWarnButStillStart) {
  // The daemon does not delete the volume it stops using when an operator
  // changes the cache size, so more than one file is the EXPECTED steady
  // state after a resize. Refusing here would turn a routine operation into a
  // server that will not come up -- and it is not the failure the check is
  // for, because the published size names exactly one of these files and the
  // attach check proves the module took it.
  DaemonCreatedItsVolume(kDaemonSize);
  DaemonCreatedItsVolume(64ULL << 20);  // left over from a smaller cache
  const GoogleString socket_path = SocketPath("pre.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  // Loud about the wasted disk, because nothing else ever will be.
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  // And it attached rather than adding a third.
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, TheSmallTierCompanionIsNotASecondVolume) {
  // The cache layer may place a small-object companion beside the volume
  // under the same stem. It belongs to ONE cache; counting it would read a
  // healthy directory as a split.  Build the companion through the shipping
  // derivation: the small tier's cache path is "<volume path>.small" and the
  // cache layer fingerprints THAT path, so the companion is the fingerprint
  // of the .small path, not the volume's name with ".small" appended (for an
  // extensionless stem the two coincide; under an extension they diverge --
  // see the extensioned leg below).
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString companion =
      FakeDaemonAbi::VolumeFileFor(StrCat(volume_, ".small"), kDaemonSize);
  FILE* f = fopen(companion.c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size())
      << "the small-tier companion was counted as a separate volume";
  EXPECT_EQ(0, remove(companion.c_str()));
}

// --- extensioned volume stems ----------------------------------------------
//
// The cache layer derives the physical volume name by inserting
// "-<generation>-<geometry>" BEFORE the stem's extension: a configured
// "cache.vol" becomes "cache-6-<hash>.vol" on disk.  A matcher that only
// knows the append form ("<stem>-<digit>...") can never find that file, and
// reports a running, healthy daemon as "volume does not exist yet".

TEST_F(DaemonAdapterTest, ExtensionedStemFindsTheDaemonsVolume) {
  volume_ = StrCat(dir_, "/cache.vol");
  DaemonCreatedItsVolume(kDaemonSize);

  const std::vector<GoogleString> files = DaemonAdapter::VolumeFiles(volume_);
  ASSERT_EQ(1u, files.size())
      << "the daemon's volume is unfindable under an extensioned stem";
  EXPECT_EQ(FakeDaemonAbi::VolumeFileFor("cache.vol", kDaemonSize), files[0]);
}

TEST_F(DaemonAdapterTest, ExtensionedStemDoesNotMatchTheAppendForm) {
  // "cache.vol-6-<hash>" is a name the daemon never derives from a
  // "cache.vol" stem; counting it would attach to a file nothing else reads.
  volume_ = StrCat(dir_, "/cache.vol");
  const GoogleString append_form = StrCat(
      volume_, "-6-", Integer64ToString(static_cast<int64>(kDaemonSize)));
  FILE* f = fopen(append_form.c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  EXPECT_TRUE(DaemonAdapter::VolumeFiles(volume_).empty());
  EXPECT_EQ(0, remove(append_form.c_str()));
}

TEST_F(DaemonAdapterTest, ExtensionedVolumePathAttachesToTheDaemonsVolume) {
  // The full startup path on an extensioned stem.  Before the matcher learned
  // the insert-before-extension form this degraded with the misleading
  // "start the daemon first" line while the daemon was running and healthy.
  volume_ = StrCat(dir_, "/cache.vol");
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("ext.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  EXPECT_EQ(0, handler_.TotalMessages());
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(kDaemonSize, last_abi_->observed_volume_size);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, ExtensionedStemStillRefusesASecondVolume) {
  // The mirror has to keep working on the extensioned form: the daemon's
  // volume on disk was made at one size and the daemon now publishes another,
  // so opening lands on a NEW "cache-6-<size>.vol" — a split cache, and the
  // one condition worth refusing a start over.  The refused start removes
  // the file it created here too.
  volume_ = StrCat(dir_, "/cache.vol");
  DaemonCreatedItsVolume(64ULL << 20);
  const std::vector<GoogleString> before = DaemonAdapter::VolumeFiles(volume_);
  const GoogleString socket_path = SocketPath("extsplit.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_EQ(before, DaemonAdapter::VolumeFiles(volume_));
}

TEST_F(DaemonAdapterTest, EveryConsecutiveStartRefusesUntilSizesAgree) {
  // The file a refusal leaves behind is the next start's silent split: the next
  // start over the same directory finds two files, calls them left-overs,
  // and attaches to the file the refusal created.  With the refusal
  // removing its own file, the second check — a fresh adapter, what a
  // restart or a new worker process is — refuses the same way, and the
  // daemon's file is the only one left.
  DaemonCreatedItsVolume(64ULL << 20);
  const GoogleString socket_path = SocketPath("twice.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> first(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, first->StartupCheck());
  first.reset();
  std::unique_ptr<DaemonAdapter> second(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, second->StartupCheck());
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
  // And the recovery the message points at: the daemon's volume for the
  // published size appears (the daemon restarted onto it), and a third
  // start attaches -- the left-over-files branch, then ready.
  DaemonCreatedItsVolume(kDaemonSize);
  std::unique_ptr<DaemonAdapter> third(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, third->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, third->health());
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, ARefusalRemovesOnlyTheFileItCreated) {
  // Present BEFORE the check: the daemon's own volume, a COUNTED
  // left-over volume from an earlier cache size (the refusal fires with a
  // left-over already in the directory, because the published size's file
  // is still absent), and a small-tier companion under the very name the
  // refusal's open is about to create.  The companion is not a volume the
  // scan counts and everything here predates the open, so the refusal's
  // removal must leave all of it exactly where it was, while still taking
  // away what the open created.
  DaemonCreatedItsVolume(64ULL << 20);
  DaemonCreatedItsVolume(32ULL << 20);  // left over from an earlier size
  const GoogleString leftover =
      FakeDaemonAbi::VolumeFileFor(volume_, 32ULL << 20);
  const GoogleString companion =
      FakeDaemonAbi::VolumeFileFor(StrCat(volume_, ".small"), kDaemonSize);
  FILE* companion_file = fopen(companion.c_str(), "a");
  ASSERT_TRUE(companion_file != nullptr);
  fclose(companion_file);
  const std::vector<GoogleString> before = DaemonAdapter::VolumeFiles(volume_);
  ASSERT_EQ(2u, before.size());
  const GoogleString created =
      FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize);
  const GoogleString socket_path = SocketPath("family.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  // The created volume file is gone; both pre-existing volumes remain...
  EXPECT_EQ(before, DaemonAdapter::VolumeFiles(volume_));
  struct stat leftover_stat;
  ASSERT_EQ(0, stat(leftover.c_str(), &leftover_stat))
      << "the refusal removed a COUNTED left-over that existed before it";
  // ...the message's removal clause names only the created file (by its
  // name as the scan lists it, not its full path)...
  const GoogleString dumped = DumpedMessages();
  const size_t created_slash = created.find_last_of('/');
  const GoogleString created_name = created_slash == GoogleString::npos
                                        ? created
                                        : created.substr(created_slash + 1);
  EXPECT_NE(GoogleString::npos,
            dumped.find(StrCat("This start removed the file it created (",
                               created_name, ")")))
      << dumped;
  // ...and the pre-existing companion survived the refusal untouched.
  struct stat companion_stat;
  EXPECT_EQ(0, stat(companion.c_str(), &companion_stat))
      << "the refusal removed a file that existed before the check";
  EXPECT_EQ(0, remove(companion.c_str()));
}

TEST_F(DaemonAdapterTest, ARefusalWhoseRemovalFailsStillRefuses) {
  // The delete can fail in two ways that need OPPOSITE advice.  POSIX,
  // staged here by making the directory non-writable the moment the open
  // has created the file: our own failure, safe to remove by hand.  A
  // sharing violation on Windows — staged by holding the file open
  // without delete sharing — almost certainly means the OPTIMIZER holds
  // it: its live volume, which an operator must leave in place.  The
  // verdict stays refused on both.
  DaemonCreatedItsVolume(64ULL << 20);
  const GoogleString created =
      FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize);
  const GoogleString socket_path = SocketPath("stuck.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
#ifndef _WIN32
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
#endif
#ifdef _WIN32
  HANDLE held = INVALID_HANDLE_VALUE;
  fake_after_open_ = [&](const GoogleString&) {
    std::wstring wide;
    if (Utf8ToWidePath(created, &wide)) {
      held = CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                         0, nullptr);
    }
  };
#else
  fake_after_open_ = [&](const GoogleString&) { chmod(dir_.c_str(), 0500); };
#endif
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  // The file this start created is still there...
  struct stat created_stat;
  EXPECT_EQ(0, stat(created.c_str(), &created_stat))
      << "the undeletable file vanished although its removal failed";
  // ...and the refusal names what to do about it: by hand on POSIX, in
  // place on Windows.
  const GoogleString dumped = DumpedMessages();
#ifdef _WIN32
  EXPECT_NE(GoogleString::npos, dumped.find("was left in place")) << dumped;
  EXPECT_NE(GoogleString::npos, dumped.find("another process holds it open"))
      << dumped;
  EXPECT_NE(GoogleString::npos, dumped.find("Do not remove it")) << dumped;
  EXPECT_EQ(GoogleString::npos, dumped.find("Remove what remains by hand"))
      << dumped;
#else
  EXPECT_NE(GoogleString::npos,
            dumped.find("could not remove the file it created"))
      << dumped;
  EXPECT_NE(GoogleString::npos,
            dumped.find("Remove what is still there by hand"))
      << dumped;
#endif
  EXPECT_NE(GoogleString::npos,
            dumped.find("Start or restart the optimizer daemon"))
      << dumped;
#ifdef _WIN32
  if (held != INVALID_HANDLE_VALUE) {
    CloseHandle(held);
  }
#else
  chmod(dir_.c_str(), 0700);
#endif
}

#ifndef _WIN32
TEST_F(DaemonAdapterTest, ARefusalLeavesAnotherUsersVolumeInPlace) {
  // The guard for the daemon-made direction of the race: what appeared
  // during the check can be the optimizer daemon's own new volume, and a
  // start must not unlink it.  Staged through the uid seam -- an
  // ordinary-user run cannot chown a file to another user, and this case
  // must run in CI, which never runs as root: the seam makes the guard
  // see the test's own file as another user's.  The advice for this case
  // is the opposite of the manual step: the file is most likely the
  // optimizer's live volume, and the next start SHOULD attach to it.
  DaemonCreatedItsVolume(64ULL << 20);
  g_seamed_uid = geteuid() + 1;  // "another user": not us, whoever we are
  DaemonAdapter::SetEffectiveUidForTesting(&SeamedUid);
  const GoogleString socket_path = SocketPath("foreign.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  DaemonAdapter::SetEffectiveUidForTesting(nullptr);
  // The refusal stands and the "other user's" file is still there...
  const GoogleString created =
      FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize);
  struct stat foreign_stat;
  ASSERT_EQ(0, stat(created.c_str(), &foreign_stat));
  // ...the message says what it is, tells the operator to LEAVE it, and
  // never advises removing it by hand.
  const GoogleString dumped = DumpedMessages();
  EXPECT_NE(GoogleString::npos, dumped.find("owned by another user")) << dumped;
  EXPECT_NE(GoogleString::npos,
            dumped.find("most likely the optimizer daemon's new volume"))
      << dumped;
  EXPECT_NE(GoogleString::npos, dumped.find("was left in place")) << dumped;
  EXPECT_NE(GoogleString::npos, dumped.find("Do not remove it")) << dumped;
  EXPECT_EQ(GoogleString::npos, dumped.find("Remove what remains by hand"))
      << dumped;
  EXPECT_EQ(GoogleString::npos,
            dumped.find("This start removed the file it created"))
      << dumped;
}

TEST_F(DaemonAdapterTest, ARefusalNeverUnlinksARealForeignOwnedVolume) {
  // The same guard with a file whose REAL owner is another user: only
  // root can stage that (chown), and under a user-namespace sandbox even
  // root cannot (chown fails) -- so the staging is asserted, not assumed.
  if (geteuid() != 0) {
    GTEST_SKIP() << "a really foreign owner can only be staged as root";
  }
  DaemonCreatedItsVolume(64ULL << 20);
  const GoogleString created =
      FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize);
  fake_after_open_ = [&created](const GoogleString&) {
    ASSERT_EQ(0, chown(created.c_str(), 1, 1))
        << "chown failed (user-namespace sandbox); the staging did not "
           "happen";
  };
  const GoogleString socket_path = SocketPath("foreign2.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  struct stat foreign_stat;
  ASSERT_EQ(0, stat(created.c_str(), &foreign_stat));
  EXPECT_EQ(1u, foreign_stat.st_uid);
  const GoogleString dumped = DumpedMessages();
  EXPECT_NE(GoogleString::npos, dumped.find("owned by another user")) << dumped;
  EXPECT_EQ(GoogleString::npos, dumped.find("Remove what remains by hand"))
      << dumped;
}
#endif  // _WIN32

void MakeDirLike(const GoogleString& path) {
#ifdef _WIN32
  _mkdir(path.c_str());
#else
  mkdir(path.c_str(), 0700);
#endif
}

TEST_F(DaemonAdapterTest, ADirectoryShapedVolumePathTakesTheManualBranch) {
  // A configured path that IS a directory makes the scan count every
  // regular file inside, so a name that appeared during the check can be
  // anything at all and the removal must not touch it: the manual branch,
  // with the file named by its full path.  Staged by creating a regular
  // file inside the directory at the moment the open has created the
  // (beside-the-directory) split file.
  volume_ = StrCat(dir_, "/dirvol");
  MakeDirLike(volume_);
  DaemonCreatedItsVolume(64ULL << 20);
  // The separator the production message joins with (the platform's own),
  // so the full-path assertion compares like with like.
#ifdef _WIN32
  const GoogleString inside = StrCat(volume_, "\\rogue-6-1073741824");
#else
  const GoogleString inside = StrCat(volume_, "/rogue-6-1073741824");
#endif
  fake_after_open_ = [&](const GoogleString&) {
    FILE* f = fopen(inside.c_str(), "a");
    if (f != nullptr) {
      fclose(f);
    }
  };
  const GoogleString socket_path = SocketPath("dirshape.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  // The file inside is untouched...
  struct stat inside_stat;
  ASSERT_EQ(0, stat(inside.c_str(), &inside_stat))
      << "the removal touched a file inside the directory-shaped path";
  // ...and the message names it by its full path and says why.
  const GoogleString dumped = DumpedMessages();
  EXPECT_NE(GoogleString::npos, dumped.find(inside)) << dumped;
  EXPECT_NE(GoogleString::npos,
            dumped.find("does not remove files inside a directory-shaped "
                        "volume path"))
      << dumped;
  EXPECT_NE(GoogleString::npos, dumped.find("remove it by hand")) << dumped;
  EXPECT_EQ(0, remove(inside.c_str()));
}

TEST_F(DaemonAdapterTest,
       AStemMatchingNameInsideADirectoryShapedPathIsUnverified) {
  // The plain-case D1 route: a file that appears INSIDE a directory-shaped
  // configured path under a name that DOES match the stem.  The removal
  // looks for it beside the directory, does not find it there, and reports
  // the removal call as gone -- but the re-scan still lists it from
  // inside.  The message must say the removal could not be CONFIRMED
  // (naming the file), never claim an empty removal, and the file must
  // still be on disk.
  volume_ = StrCat(dir_, "/dirvol");
  MakeDirLike(volume_);
  DaemonCreatedItsVolume(64ULL << 20);
  const GoogleString stem_like = StrCat(volume_, "/dirvol-6-1073741824");
  fake_after_open_ = [&](const GoogleString&) {
    FILE* f = fopen(stem_like.c_str(), "a");
    if (f != nullptr) {
      fclose(f);
    }
  };
  const GoogleString socket_path = SocketPath("dirshape2.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  // The file inside is untouched...
  struct stat inside_stat;
  ASSERT_EQ(0, stat(stem_like.c_str(), &inside_stat))
      << "the removal touched a file inside the directory-shaped path";
  // ...the message says its removal could not be confirmed, by name...
  const GoogleString dumped = DumpedMessages();
  EXPECT_NE(GoogleString::npos,
            dumped.find("could not confirm that the file it created"))
      << dumped;
  EXPECT_NE(GoogleString::npos,
            dumped.find(StrCat("(", "dirvol-6-1073741824", ")")))
      << dumped;
  // ...and it never claims an empty removal or a clean directory.
  EXPECT_EQ(GoogleString::npos, dumped.find("removed the file it created ()"))
      << dumped;
  EXPECT_EQ(GoogleString::npos,
            dumped.find("holds only what it held before this start"))
      << dumped;
  EXPECT_EQ(0, remove(stem_like.c_str()));
}

TEST_F(DaemonAdapterTest, ExtensionedSmallTierCompanionIsNotASecondVolume) {
  // The ".small" exclusion must survive the extensioned form too, on the
  // shape the system actually ships: the small tier's cache path is
  // "<volume path>.small" and fingerprinting splits on the LAST dot, so a
  // "cache.vol" volume gets "cache.vol-6-<size>.small" -- not the
  // append-to-the-volume-name form ("cache-6-<size>.vol.small"), a name the
  // daemon never derives.  Both end in ".small", so the guard kills either;
  // pinning the shipped shape is what keeps this test honest about WHICH
  // name it excludes.
  volume_ = StrCat(dir_, "/cache.vol");
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString companion =
      FakeDaemonAbi::VolumeFileFor(StrCat(volume_, ".small"), kDaemonSize);
  // Pin the shipped shape itself: fingerprinting "<path>.small" splits on
  // the last dot, so the insert lands before ".small", not before ".vol".
  EXPECT_EQ(
      StrCat(dir_, "/cache.vol-6-",
             Integer64ToString(static_cast<int64>(kDaemonSize)), ".small"),
      companion);
  FILE* f = fopen(companion.c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size())
      << "the small-tier companion was counted as a separate volume";
  EXPECT_EQ(0, remove(companion.c_str()));
}

// --- trailing- and leading-dot volume stems --------------------------------
//
// The cache layer's std::filesystem split gives the two dot edges DIFFERENT
// answers: "cache." is stem "cache" + bare-dot extension ".", so the daemon
// writes "cache-<major>-<hash>."; ".vol" is stem ".vol" with NO extension,
// so the daemon appends ".vol-<major>-<hash>".  The matcher must mirror both
// legs.  The leading-dot leg already agreed; the trailing-dot leg did not --
// the matcher read "cache." as extensionless and matched the whole stem as
// the prefix, a name the daemon never writes, so the startup check reported
// a running, healthy daemon as "volume does not exist yet".

TEST_F(DaemonAdapterTest, TrailingDotStemFindsTheDaemonsVolume) {
#ifdef _WIN32
  // A file name cannot END IN A DOT on this platform: the volume file a
  // trailing-dot stem derives ("cache-6-<size>.") cannot exist in that
  // spelling, so the scan's answer is empty by necessity.  The creation
  // attempt below either fails outright or has its dot stripped by the file
  // system; both leave nothing that matches the stem's extension rule,
  // which is what the assertion pins.  (The ATTACHING legs of this shape
  // are a POSIX-only scenario for the same reason.)
  volume_ = StrCat(dir_, "/cache.");
  FILE* f =
      fopen(FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize).c_str(), "a");
  if (f != nullptr) {
    fclose(f);
  }

  const std::vector<GoogleString> files = DaemonAdapter::VolumeFiles(volume_);
  EXPECT_TRUE(files.empty())
      << "a name ending in a dot matched a trailing-dot stem";
#else
  volume_ = StrCat(dir_, "/cache.");
  DaemonCreatedItsVolume(kDaemonSize);

  const std::vector<GoogleString> files = DaemonAdapter::VolumeFiles(volume_);
  ASSERT_EQ(1u, files.size())
      << "the daemon's volume is unfindable under a trailing-dot stem";
  EXPECT_EQ(FakeDaemonAbi::VolumeFileFor("cache.", kDaemonSize), files[0]);
#endif
}

TEST_F(DaemonAdapterTest, TrailingDotVolumePathAttachesToTheDaemonsVolume) {
#ifdef _WIN32
  // The full-startup leg of the trailing-dot stem: the volume file it needs
  // ("cache-6-<size>.") cannot exist in that spelling on this platform (a
  // file name cannot end in a dot), so there is nothing to attach to.  The
  // scan-half of the shape IS assertable and is, one test up.
  GTEST_SKIP() << "a file name cannot end in a dot on this platform";
#else
  // The full startup path on a trailing-dot stem.  Before the matcher
  // mirrored the filesystem split this degraded with the misleading "start
  // the daemon first" line while the daemon was running and healthy.
  volume_ = StrCat(dir_, "/cache.");
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("dot.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kReady, adapter->health());
  EXPECT_EQ(0, handler_.TotalMessages());
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_EQ(kDaemonSize, last_abi_->observed_volume_size);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
#endif
}

TEST_F(DaemonAdapterTest, LeadingDotStemFindsTheDaemonsVolume) {
  // The leading-dot leg already agrees on both sides (a leading dot is part
  // of the stem, not an extension); pin it so the trailing-dot fix cannot
  // regress it.
  volume_ = StrCat(dir_, "/.vol");
  DaemonCreatedItsVolume(kDaemonSize);

  const std::vector<GoogleString> files = DaemonAdapter::VolumeFiles(volume_);
  ASSERT_EQ(1u, files.size())
      << "the daemon's volume is unfindable under a leading-dot stem";
  EXPECT_EQ(FakeDaemonAbi::VolumeFileFor(".vol", kDaemonSize), files[0]);
}

TEST_F(DaemonAdapterTest, UnopenableVolumeTurnsIproOff) {
  fake_open_error_ = 2;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("brk.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
}

#ifdef _WIN32
// --- Windows volume-scan specifics -----------------------------------------
//
// The scan's platform contract: either separator in the configured path (an
// operator writes both spellings), names carried as UTF-8 end to end, and a
// DIRECTORY whose name happens to match the volume shape is not a volume.

TEST_F(DaemonAdapterTest, BackslashStemFindsTheDaemonsVolume) {
  // The daemon's naming is a function of the LEAF ("volume-6-<size>"), so
  // the file is derived through the leaf and then looked for through a
  // backslash stem.
  const GoogleString name = FakeDaemonAbi::VolumeFileFor("volume", kDaemonSize);
  FILE* f = fopen(StrCat(dir_, "\\", name).c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  const std::vector<GoogleString> files =
      DaemonAdapter::VolumeFiles(StrCat(dir_, "\\volume"));
  ASSERT_EQ(1u, files.size());
  EXPECT_EQ(name, files[0]);
}

TEST_F(DaemonAdapterTest, ForwardSlashStemFindsTheDaemonsVolume) {
  // The same leaf through a forward-slash stem, which is how a joined path
  // from the configuration reader may arrive.
  const GoogleString name = FakeDaemonAbi::VolumeFileFor("volume", kDaemonSize);
  FILE* f = fopen(StrCat(dir_, "\\", name).c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  const std::vector<GoogleString> files =
      DaemonAdapter::VolumeFiles(StrCat(dir_, "/volume"));
  ASSERT_EQ(1u, files.size());
  EXPECT_EQ(name, files[0]);
}

TEST_F(DaemonAdapterTest, FindsTheVolumeUnderANonAsciiDirectory) {
  // "cafe" with a COMBINING ACUTE ACCENT in UTF-8: two bytes, both non-ASCII,
  // so the path cannot ride through any ANSI code page on its way to the
  // wide scan and back.
  const GoogleString non_ascii = StrCat(dir_, "/caf\xcc\x81");
  std::wstring wide_dir;
  ASSERT_TRUE(Utf8ToWide(non_ascii, &wide_dir));
  ASSERT_TRUE(CreateDirectoryW(wide_dir.c_str(), nullptr));

  const GoogleString name = FakeDaemonAbi::VolumeFileFor("volume", kDaemonSize);
  std::wstring wide_file;
  ASSERT_TRUE(Utf8ToWide(StrCat(non_ascii, "/", name), &wide_file));
  ASSERT_TRUE(TouchWideFile(wide_file));

  const std::vector<GoogleString> files =
      DaemonAdapter::VolumeFiles(StrCat(non_ascii, "/volume"));
  ASSERT_EQ(1u, files.size());
  EXPECT_EQ(name, files[0]);

  // The fixture's cleanup only removes plain files in its own directory, so
  // a subdirectory fixture cleans up after itself.
  DeleteFileW(wide_file.c_str());
  RemoveDirectoryW(wide_dir.c_str());
}

#ifdef _WIN32
TEST_F(DaemonAdapterTest, ARootLevelStemNamesTheDriveRootNotNothing) {
  // A drive-relative stem whose only separator is the leading one names a
  // file in the root of the current drive; its parent is that root, as
  // "/volume" names "/" on POSIX -- never an empty string, which every
  // probe would read as absent, and never "\\" + "\\*", which spells a
  // UNC prefix.  The root is listable, so the access check answers
  // readable, and the scan comes back empty rather than confused.
  const GoogleString stem = StrCat("\\volume-", IntegerToString(_getpid()));

  EXPECT_EQ(DaemonAdapter::DirAccess::kReadable,
            DaemonAdapter::VolumeDirAccess(stem));
  EXPECT_TRUE(DaemonAdapter::VolumeFiles(stem).empty());
}
#endif  // _WIN32

TEST_F(DaemonAdapterTest, DoesNotCountADirectoryNamedLikeAVolume) {
  // A DIRECTORY whose name matches the volume shape beside the stem must not
  // be counted; only the real file below it is a volume.  The two names
  // differ so the assertion cannot pass on the directory being skipped for
  // some other reason.
  const GoogleString decoy = StrCat(dir_, "/volume-6-111");
  std::wstring wide_decoy;
  ASSERT_TRUE(Utf8ToWide(decoy, &wide_decoy));
  ASSERT_TRUE(CreateDirectoryW(wide_decoy.c_str(), nullptr));
  const GoogleString name = FakeDaemonAbi::VolumeFileFor("volume", kDaemonSize);
  FILE* f = fopen(StrCat(dir_, "/", name).c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  const std::vector<GoogleString> files = DaemonAdapter::VolumeFiles(volume_);
  ASSERT_EQ(1u, files.size())
      << "a directory named like a volume was counted, or the volume was "
         "not found";
  EXPECT_EQ(name, files[0]);

  // The fixture's cleanup only removes plain files, so the decoy directory
  // goes away here.
  RemoveDirectoryW(wide_decoy.c_str());
}
#endif  // _WIN32

// --- the reason behind the error class --------------------------------------
//
// The failure these cases are written from reported "Input/output error" and
// nothing else, for a condition whose actual reason named the fix outright.
// The class alone is not diagnosable, so both open sites report the class WITH
// the peer's own reason wherever the installed library publishes one.

TEST_F(DaemonAdapterTest, TheStartupProbeNamesThePeersReason) {
  fake_open_error_ = 2;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("why.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  // The class, so an operator can still grep for it...
  EXPECT_NE(GoogleString::npos, Messages().find("fake error")) << Messages();
  // ...and the reason, which is the half that says what to do.
  EXPECT_NE(GoogleString::npos, Messages().find("Operation not permitted"))
      << Messages();
}

TEST_F(DaemonAdapterTest, AnOlderPeerWithoutAReasonStillNamesTheErrorClass) {
  // The entry point is bound optionally, so a daemon package from before it
  // was published must still produce the line it always produced -- not an
  // empty reason, and not a refusal to start.
  publishes_last_error_ = false;
  fake_open_error_ = 2;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("old.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_NE(GoogleString::npos,
            Messages().find("cache volume at " + volume_ + ": fake error"))
      << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find("Operation not permitted"))
      << Messages();
}

TEST_F(DaemonAdapterTest, TheRecordCacheOpenNamesThePeersReason) {
  // The field failure was HERE, not at startup: the parent process could open
  // the volume and the dropped children could not, so the startup probe
  // passed and only this open failed.
  const GoogleString socket_path = SocketPath("rec.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;

  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_NE(GoogleString::npos, Messages().find("Operation not permitted"))
      << Messages();
}

TEST_F(DaemonAdapterTest, AnIdlePeerReportingAnEmptyReasonAddsNothing) {
  // The peer clears its buffer on entry to every open, so a library that has
  // the entry point but wrote nothing back into it answers "" rather than
  // nullptr.  Checking only for nullptr would print a bare "()" -- worse than
  // the line this change set out to improve.
  last_error_message_ = "";
  fake_open_error_ = 2;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("idle.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_NE(GoogleString::npos,
            Messages().find("cache volume at " + volume_ + ": fake error"))
      << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find("()")) << Messages();
}

TEST_F(DaemonAdapterTest, AReasonThatRepeatsTheErrorClassIsNotPrintedTwice) {
  // Some codes are their own explanation, and the peer answers with the same
  // sentence StrError returns.  Joining them blindly would print the words
  // twice and read like two different facts.
  last_error_message_ = "fake error";
  fake_open_error_ = 2;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("dup.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(GoogleString::npos, Messages().find("fake error (fake error)"))
      << Messages();
  EXPECT_NE(GoogleString::npos, Messages().find("fake error")) << Messages();
}

TEST_F(DaemonAdapterTest, AnOlderPeerWithoutAReasonStillLogsTheRecordSiteLine) {
  // The symbol-absent case at the site the field failure actually hit.  The
  // startup probe has its own case; this one is the per-process open, which
  // is the one that ran with the web-server user's privileges.
  publishes_last_error_ = false;
  const GoogleString socket_path = SocketPath("recold.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;

  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_NE(GoogleString::npos,
            Messages().find("cache volume at " + volume_ + ": fake error"))
      << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find("()")) << Messages();
  // Still counted, still scheduled: a missing explanation is not a missing
  // retry.
  EXPECT_NE(GoogleString::npos, Messages().find("attempt 1 of")) << Messages();
}

// --- the record-cache retry schedule ----------------------------------------

TEST_F(DaemonAdapterTest, AFailedRecordCacheOpenIsNotRetriedWithinItsBackoff) {
  const GoogleString socket_path = SocketPath("bo1.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;
  const int opens_after_startup = last_abi_->opens;

  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);

  // Every request in the backoff window: no syscall, and no second log line.
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(adapter->RecordCache() == nullptr);
  }
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs - 1;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
}

TEST_F(DaemonAdapterTest, ARecordCacheOpenIsRetriedOnceItsBackoffElapses) {
  const GoogleString socket_path = SocketPath("bo2.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;
  const int opens_after_startup = last_abi_->opens;

  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens_after_startup + 2, last_abi_->opens);

  // The second backoff is the first doubled, so the wait that was long enough
  // a moment ago is not long enough now.
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens_after_startup + 2, last_abi_->opens);
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens_after_startup + 3, last_abi_->opens);

  // One line per attempt, never one per request.
  EXPECT_EQ(3, handler_.MessagesOfType(kWarning));
}

TEST_F(DaemonAdapterTest, ADaemonThatFinishesStartingIsPickedUpOnARetry) {
  // The whole point of the schedule: a worker that came up while the daemon
  // was still starting used to record nothing for the rest of its life.
  const GoogleString socket_path = SocketPath("heal.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;

  ASSERT_TRUE(adapter->RecordCache() == nullptr);
  last_abi_->open_error = kPsOk;
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  void* cache = adapter->RecordCache();
  EXPECT_TRUE(cache != nullptr);

  // And once open, it stays open and is not re-opened.
  const int opens = last_abi_->opens;
  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  EXPECT_EQ(cache, adapter->RecordCache());
  EXPECT_EQ(opens, last_abi_->opens);
  // The failure was transient and healed itself; nothing here is an error.
  EXPECT_EQ(0, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterTest, ADurableFailureGivesUpAfterABoundedNumberOfAttempts) {
  const GoogleString socket_path = SocketPath("give.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;
  const int opens_after_startup = last_abi_->opens;

  for (int i = 0; i < DaemonAdapter::kRecordCacheMaxAttempts; ++i) {
    EXPECT_TRUE(adapter->RecordCache() == nullptr);
    // Past the largest backoff, so each iteration is genuinely an attempt.
    now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  }
  EXPECT_EQ(opens_after_startup + DaemonAdapter::kRecordCacheMaxAttempts,
            last_abi_->opens);

  // A durable failure must not become one open syscall per request forever.
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(adapter->RecordCache() == nullptr);
    now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  }
  EXPECT_EQ(opens_after_startup + DaemonAdapter::kRecordCacheMaxAttempts,
            last_abi_->opens);

  // The give-up is the one line that needs an operator, so it is the only
  // one at error level; every attempt before it could still have healed.
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_EQ(DaemonAdapter::kRecordCacheMaxAttempts - 1,
            handler_.MessagesOfType(kWarning));
  EXPECT_NE(GoogleString::npos, Messages().find("Giving up")) << Messages();
}

TEST_F(DaemonAdapterTest, TheBackoffStopsDoublingAtItsCap) {
  const GoogleString socket_path = SocketPath("cap.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;

  // Six failures, each waited out past any backoff the schedule can produce.
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(adapter->RecordCache() == nullptr);
    now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  }
  // The SEVENTH is the first whose uncapped backoff (the first doubled six
  // times, 64s) exceeds the cap.  Everything below asks one question: did the
  // cap hold, or is the schedule still doubling?
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  const int opens = last_abi_->opens;

  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs - 1;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "retried before the cap had elapsed";

  now_ms_ += 1;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens)
      << "still waiting at the cap, so the backoff is not capped -- an "
         "uncapped doubling would have another 4s to run here";
}

TEST_F(DaemonAdapterTest, ConcurrentCallersMakeOneAttemptPerWindowBetweenThem) {
  // The window is the unit of work, not the call.  Two request threads
  // arriving together is the shape a worker actually sees, and the property
  // that matters is that whichever one takes the attempt, the other finds it
  // already taken -- one open syscall and one log line between them, not two.
  const GoogleString socket_path = SocketPath("race.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  last_abi_->open_error = 2;
  const int opens_after_startup = last_abi_->opens;

  std::atomic<int> at_the_line(0);
  auto hammer = [&]() {
    // Line the threads up so they genuinely contend rather than running one
    // after the other, which would prove nothing.
    at_the_line.fetch_add(1);
    while (at_the_line.load() < 2) {
    }
    for (int i = 0; i < 200; ++i) {
      EXPECT_TRUE(adapter->RecordCache() == nullptr);
    }
  };
  std::thread first(hammer);
  std::thread second(hammer);
  first.join();
  second.join();

  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
}

TEST_F(DaemonAdapterTest, CloseRecordCacheClosesAnOpenHandleOnce) {
  const GoogleString socket_path = SocketPath("close1.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);

  const int closes_at_open = last_abi_->closes;
  adapter->CloseRecordCache();
  EXPECT_EQ(closes_at_open + 1, last_abi_->closes);
}

TEST_F(DaemonAdapterTest, CloseRecordCacheIsIdempotent) {
  const GoogleString socket_path = SocketPath("close2.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);

  const int closes_at_open = last_abi_->closes;
  adapter->CloseRecordCache();
  adapter->CloseRecordCache();
  EXPECT_EQ(closes_at_open + 1, last_abi_->closes);
}

TEST_F(DaemonAdapterTest, CloseRecordCacheWithoutAHandleIsANoOp) {
  const GoogleString socket_path = SocketPath("close3.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);

  const int closes_at_open = last_abi_->closes;
  adapter->CloseRecordCache();  // RecordCache() was never called
  EXPECT_EQ(closes_at_open, last_abi_->closes);
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
}

TEST_F(DaemonAdapterTest, CloseRecordCacheIsFinal) {
  const GoogleString socket_path = SocketPath("close4.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);

  const int opens_at_close = last_abi_->opens;
  adapter->CloseRecordCache();
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  // Past every backoff the retry series can produce: still nothing, and no
  // new open -- a worker that is exiting must not open a fresh handle from a
  // straggling call.
  now_ms_ += 1000 * 1000;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens_at_close, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, TheVolumeGenerationIsKnownOrUnknownNeverGuessed) {
  uint64_t generation = 99;
  // No file: the daemon has never replaced its volume.  Known, and 0.
  EXPECT_TRUE(DaemonAdapter::ReadVolumeGeneration(volume_, &generation));
  EXPECT_EQ(0u, generation);
  DaemonPublishedGeneration(7);
  EXPECT_TRUE(DaemonAdapter::ReadVolumeGeneration(volume_, &generation));
  EXPECT_EQ(7u, generation);
  DaemonPublishedGeneration(18446744073709551615ULL);
  EXPECT_TRUE(DaemonAdapter::ReadVolumeGeneration(volume_, &generation));
  EXPECT_EQ(18446744073709551615ULL, generation);
  // A line that ends in a carriage return and a newline is a complete
  // line too: it is what a writer in text mode produces on Windows -- these
  // helpers included, so there every number above was read in that form.
  // Written here byte for byte, so every platform reads both endings.
  for (const char* text : {"12\n", "12\r\n"}) {
    FILE* f = fopen(StrCat(volume_, ".gen").c_str(), "wb");
    ASSERT_TRUE(f != nullptr);
    fputs(text, f);
    fclose(f);
    generation = 99;
    EXPECT_TRUE(DaemonAdapter::ReadVolumeGeneration(volume_, &generation));
    EXPECT_EQ(12u, generation);
  }
  // Empty, cut short before its newline, not a number, or too large to
  // hold: UNKNOWN -- never "generation 0", which would read as a change.
  for (const char* text : {"", "\n", "12", "12\r", "12\rx\n", "x12\n",
                           " 12\n", "-1\n", "18446744073709551616\n"}) {
    WriteGenerationFile(text);
    generation = 99;
    EXPECT_FALSE(DaemonAdapter::ReadVolumeGeneration(volume_, &generation))
        << "[" << text << "]";
    EXPECT_EQ(0u, generation) << "[" << text << "]";
  }
#ifndef _WIN32
  // Present but not readable as a file at all.
  unlink(StrCat(volume_, ".gen").c_str());
  ASSERT_EQ(0, mkdir(StrCat(volume_, ".gen").c_str(), 0700));
  EXPECT_FALSE(DaemonAdapter::ReadVolumeGeneration(volume_, &generation));
  ASSERT_EQ(0, rmdir(StrCat(volume_, ".gen").c_str()));
#endif
}

TEST_F(DaemonAdapterTest, AReplacedVolumeIsReopened) {
  // A full cache purge replaced the volume file.  The process must end up
  // on the new file without being restarted.
  const GoogleString socket_path = SocketPath("gen1.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int closes = last_abi_->closes;
  const int errors = handler_.MessagesOfType(kError);
  const int warnings = handler_.MessagesOfType(kWarning);

  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens) << "the volume was not opened again";
  EXPECT_EQ(closes, last_abi_->closes)
      << "the old handle was closed while a request may still hold it";
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
  // Routine, not a failure: nothing is logged above the info level.
  EXPECT_EQ(errors, handler_.MessagesOfType(kError));
  EXPECT_EQ(warnings, handler_.MessagesOfType(kWarning));

  // And it settles: the new generation is the one it now holds.
  now_ms_ += 10 * DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, TheStaleHandleIsNotHandedOutBeforeTheReopen) {
  // Between the moment the replacement is seen and the reopen, the accessor
  // that never opens must hand out nothing: a request answered by the origin
  // is safe, one answered from the deleted file is not.
  const GoogleString socket_path = SocketPath("gen2.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  ASSERT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
  const int opens = last_abi_->opens;

  DaemonPublishedGeneration(1);
  // Inside the check interval nothing has been looked at yet.
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr) << "and it stays so";
  EXPECT_EQ(opens, last_abi_->opens) << "the accessor that never opens opened";

  // The opening accessor then reopens, and both hand out the new handle.
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
}

TEST_F(DaemonAdapterTest, AFailedReopenStaysClosedAndRetries) {
  // The new file cannot be opened yet.  The arm stays closed -- it never
  // falls back to the old handle -- and the ordinary retry schedule picks
  // the volume up.
  const GoogleString socket_path = SocketPath("gen3.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);

  DaemonPublishedGeneration(1);
  last_abi_->open_error = 2;
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  // Still inside the first backoff: nothing, and no open.
  const int opens = last_abi_->opens;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens);

  last_abi_->open_error = kPsOk;
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
}

TEST_F(DaemonAdapterTest, AnUnchangedGenerationNeverReopens) {
  const GoogleString socket_path = SocketPath("gen4.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  // The daemon replaced its volume some time BEFORE this process opened it:
  // the number on disk is this process's starting point, not a change.
  DaemonPublishedGeneration(5);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  void* cache = adapter->RecordCache();
  ASSERT_TRUE(cache != nullptr);
  const int opens = last_abi_->opens;
  for (int i = 0; i < 20; ++i) {
    now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
    EXPECT_EQ(cache, adapter->RecordCache());
    EXPECT_EQ(cache, adapter->RecordCacheIfOpen());
  }
  EXPECT_EQ(opens, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, AnUnreadableGenerationIsNotAReplacement) {
  // The generation file cannot be read for a while -- out of descriptors, a
  // file caught half written, anything that is not an answer.  That is
  // "unknown": the handle in use stays in use, nothing is set aside, and
  // none of the bounded replacements is spent on it.
  const GoogleString socket_path = SocketPath("gen8.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  DaemonPublishedGeneration(3);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  void* cache = adapter->RecordCache();
  ASSERT_TRUE(cache != nullptr);
  const int opens = last_abi_->opens;

  for (const char* text : {"", "4", "garbage\n"}) {
    WriteGenerationFile(text);
    for (int i = 0; i < 3; ++i) {
      now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
      EXPECT_EQ(cache, adapter->RecordCacheIfOpen()) << "[" << text << "]";
      EXPECT_EQ(cache, adapter->RecordCache()) << "[" << text << "]";
    }
  }
  EXPECT_EQ(opens, last_abi_->opens);

  // Readable again and the same number: still nothing.
  DaemonPublishedGeneration(3);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_EQ(cache, adapter->RecordCache());
  EXPECT_EQ(opens, last_abi_->opens);

  // Readable and a NEW number: that is a replacement.
  DaemonPublishedGeneration(4);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, SetAsideHandlesAreClosedAtTeardownOnly) {
  const GoogleString socket_path = SocketPath("gen5.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int closes = last_abi_->closes;

  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  DaemonPublishedGeneration(2);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(closes, last_abi_->closes) << "nothing is closed while serving";

  // The current handle and the two set aside.
  adapter->CloseRecordCache();
  EXPECT_EQ(closes + 3, last_abi_->closes);
  adapter->CloseRecordCache();
  EXPECT_EQ(closes + 3, last_abi_->closes) << "closing twice closed twice";
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
}

TEST_F(DaemonAdapterTest, DestroyingTheAdapterClosesSetAsideHandles) {
  const GoogleString socket_path = SocketPath("gen6.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  // The fake is freed with the adapter: count through a pointee that
  // outlives it.
  int closes = 0;
  last_abi_->external_closes = &closes;

  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  ASSERT_EQ(0, closes);
  adapter.reset();
  EXPECT_EQ(2, closes) << "the current handle and the one set aside";
}

TEST_F(DaemonAdapterTest, TheProcessFollowsExactlyTheDocumentedReplacements) {
  // The bound, at its boundary.  kMaxVolumeReplacements replacements are
  // followed; the next one stops the process using the cache until it ends
  // -- nothing handed out, no reopen, one error line -- holding exactly
  // kMaxVolumeReplacements + 1 replaced volumes, all closed at teardown.
  const GoogleString socket_path = SocketPath("gen7.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int errors = handler_.MessagesOfType(kError);
  const int closes = last_abi_->closes;

  uint64_t generation = 0;
  for (int i = 0; i < DaemonAdapter::kMaxVolumeReplacements; ++i) {
    DaemonPublishedGeneration(++generation);
    now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
    ASSERT_TRUE(adapter->RecordCache() != nullptr)
        << "replacement " << (i + 1) << " of "
        << DaemonAdapter::kMaxVolumeReplacements << " was not followed";
    ASSERT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
  }
  EXPECT_EQ(errors, handler_.MessagesOfType(kError));
  const int opens = last_abi_->opens;

  // One more: the process stops.
  DaemonPublishedGeneration(++generation);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "it reopened past the bound";
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  // And it stays stopped, silently, whatever happens next.
  DaemonPublishedGeneration(++generation);
  now_ms_ += 1000 * 1000;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError))
      << "said more than once";
  EXPECT_EQ(closes, last_abi_->closes) << "nothing is closed while running";

  adapter->CloseRecordCache();
  EXPECT_EQ(closes + DaemonAdapter::kMaxVolumeReplacements + 1,
            last_abi_->closes)
      << "the process must hold exactly kMaxVolumeReplacements + 1 replaced "
         "volumes at the bound";
}

TEST_F(DaemonAdapterTest, AReopenWaitsUntilTheDaemonPublishesASize) {
  // The daemon's shared configuration cannot be read at the moment of the
  // reopen: the size is not known, so nothing is opened -- least of all at
  // the size remembered from start-up.
  const GoogleString socket_path = SocketPath("gen12.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;

  last_abi_->republishes_nothing = true;
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "it opened without knowing the size";

  last_abi_->republishes_nothing = false;
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, AnUnreadableGenerationNeverClearsADetectedReplacement) {
  // Once a replacement has been seen, a read that gives no answer is not
  // evidence against it: the old handle stays out of use, nothing is opened
  // under a number nobody could read, and requests go to the origin until a
  // number can be read again.
  const GoogleString socket_path = SocketPath("gen13.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  DaemonPublishedGeneration(3);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int closes = last_abi_->closes;

  DaemonPublishedGeneration(4);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  ASSERT_TRUE(adapter->RecordCacheIfOpen() == nullptr) << "not detected";

  for (const char* text : {"", "4", "garbage\n"}) {
    WriteGenerationFile(text);
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(adapter->RecordCache() == nullptr) << "[" << text << "]";
      EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr)
          << "[" << text << "]";
      now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
    }
  }
  EXPECT_EQ(opens, last_abi_->opens);
  EXPECT_EQ(closes, last_abi_->closes);

  // Readable again: the new volume is opened.
  DaemonPublishedGeneration(4);
  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
}

TEST_F(DaemonAdapterTest, ADetectionIsDroppedOnlyByAKnownMatchingNumber) {
  // The look that raises the alarm runs without the lock.  The decision is
  // taken again under it, and only a number that WAS read and equals the
  // one the handle was opened under puts the handle back in use.
  const GoogleString socket_path = SocketPath("gen14.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  DaemonPublishedGeneration(3);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  void* cache = adapter->RecordCache();
  ASSERT_TRUE(cache != nullptr);
  const int opens = last_abi_->opens;

  DaemonPublishedGeneration(4);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  ASSERT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  DaemonPublishedGeneration(3);
  EXPECT_EQ(cache, adapter->RecordCache());
  EXPECT_EQ(cache, adapter->RecordCacheIfOpen());
  EXPECT_EQ(opens, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, AVolumeIsNotOpenedWhileItsGenerationIsUnknown) {
  // A handle is held against the number read just before its open.  With no
  // number there is nothing to hold it against -- a replacement that came
  // later could never be told from the starting point -- so nothing is
  // opened until the number can be read.
  const GoogleString socket_path = SocketPath("gen15.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens = last_abi_->opens;
  const int warnings = handler_.MessagesOfType(kWarning);

  WriteGenerationFile("not a number\n");
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "opened under an unknown number";
  EXPECT_EQ(warnings + 1, handler_.MessagesOfType(kWarning));

  DaemonPublishedGeneration(5);
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
}

TEST_F(DaemonAdapterTest, AHandleWhoseGenerationMovedDuringTheOpenIsNotUsed) {
  // The number is read before the open and again after it.  A handle is
  // handed out only when both reads answered and agree: otherwise it may be
  // on a file the daemon has already replaced.  Such a handle was never
  // handed out, so it is closed at once and the open is tried again.
  const GoogleString socket_path = SocketPath("gen16.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  DaemonPublishedGeneration(5);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  int opens = last_abi_->opens;
  int closes = last_abi_->closes;

  // Unreadable by the time the open returns.
  last_abi_->after_open = [this](const GoogleString&) {
    WriteGenerationFile("5");
  };
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(closes + 1, last_abi_->closes) << "the unused handle was kept";

  // Readable, but another number by the time the open returns.
  DaemonPublishedGeneration(5);
  last_abi_->after_open = [this](const GoogleString&) {
    DaemonPublishedGeneration(6);
  };
  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 2, last_abi_->opens);
  EXPECT_EQ(closes + 2, last_abi_->closes);

  // The same number before and after: used.
  last_abi_->after_open = nullptr;
  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
  EXPECT_EQ(opens + 3, last_abi_->opens);
  EXPECT_EQ(closes + 2, last_abi_->closes);
}

TEST_F(DaemonAdapterTest, AReopenUsesTheSizeTheDaemonPublishesNow) {
  // The daemon was restarted with another cache size while this server kept
  // running, and the cache was then purged: one volume file, under another
  // name.  The reopen must land on it, at the size the daemon publishes
  // now, not at the size read when the server started.
  const uint64_t kResized = 64ULL << 20;
  const GoogleString socket_path = SocketPath("gen10.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  ASSERT_EQ(kDaemonSize, last_abi_->observed_volume_size);

  last_abi_->republished_size = kResized;
  ASSERT_EQ(0,
            remove(FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize).c_str()));
  DaemonCreatedItsVolume(kResized);
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(kResized, last_abi_->observed_volume_size)
      << "the reopen used the size inherited at start-up";
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size())
      << "the reopen made a volume file";
}

TEST_F(DaemonAdapterTest, AReopenWithNoVolumeFileOpensNothing) {
  // The daemon has not created its new volume (yet).  An open would create
  // one -- a volume of this module's making -- so there is no open at all.
  const GoogleString socket_path = SocketPath("gen11.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int warnings = handler_.MessagesOfType(kWarning);
  const GoogleString volume_file =
      FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize);

  ASSERT_EQ(0, remove(volume_file.c_str()));
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "it opened with no volume file there";
  EXPECT_TRUE(DaemonAdapter::VolumeFiles(volume_).empty())
      << "the reopen created a volume file";
  EXPECT_EQ(warnings + 1, handler_.MessagesOfType(kWarning));

#ifndef _WIN32
  // A directory that carries the volume file's name is not a volume file.
  ASSERT_EQ(0, mkdir(volume_file.c_str(), 0700));
  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "it opened onto a directory";
  ASSERT_EQ(0, rmdir(volume_file.c_str()));
#endif

  // The daemon creates its volume; the next scheduled attempt attaches.
  DaemonCreatedItsVolume(kDaemonSize);
  now_ms_ += DaemonAdapter::kRecordCacheMaxBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AReopenBesideASecondVolumeFileOpensNothing) {
  // Two volume files: this module cannot tell which one the daemon uses, and
  // one of them may be a file another process created by mistake.  Nothing
  // is opened until exactly one is left.
  const uint64_t kResized = 64ULL << 20;
  const GoogleString socket_path = SocketPath("gen17.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;

  last_abi_->republished_size = kResized;
  DaemonCreatedItsVolume(kResized);
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens);

  ASSERT_EQ(0,
            remove(FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize).c_str()));
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(kResized, last_abi_->observed_volume_size);
  EXPECT_EQ(opens + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, AnEarlierFormatVolumeFileDoesNotBlockAReopen) {
  // The daemon keeps a volume file written in an earlier cache format when
  // it purges -- it is the operator's way back to the earlier build.  That
  // file is not a candidate for the daemon's volume and must not keep a
  // process from opening the new one.
  const GoogleString socket_path = SocketPath("gen22.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  const GoogleString earlier = StrCat(volume_, "-5-123");
  FILE* f = fopen(earlier.c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int errors = handler_.MessagesOfType(kError);

  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr)
      << "a kept earlier-format file blocked the reopen";
  EXPECT_TRUE(adapter->RecordCacheIfOpen() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(errors, handler_.MessagesOfType(kError));
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AProcessStartedAfterAResizeAndAPurgeOpensTheCurrentVolume) {
  // The server read the daemon's cache size when it started.  The daemon
  // was then restarted with another size and its cache purged: the file of
  // the old size is gone.  A worker process that starts NOW makes its first
  // open -- at the size the daemon publishes now, onto the file that is
  // there, creating nothing.
  const uint64_t kResized = 64ULL << 20;
  const GoogleString socket_path = SocketPath("gen23.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);

  last_abi_->republished_size = kResized;
  ASSERT_EQ(0,
            remove(FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize).c_str()));
  DaemonCreatedItsVolume(kResized);
  DaemonPublishedGeneration(1);
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(kResized, last_abi_->observed_volume_size)
      << "the first open used the size read when the server started";
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size())
      << "the first open created a volume file";
}

TEST_F(DaemonAdapterTest, AFirstOpenThatCreatedAVolumeFileStopsTheProcess) {
  // The same look before and after as a reopen: a first open that turns out
  // to have made a volume file is not used, the process stops using the
  // cache with one error line, and the file is left where it is.
  const uint64_t kResized = 64ULL << 20;
  const GoogleString socket_path = SocketPath("gen24.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens = last_abi_->opens;
  const int closes = last_abi_->closes;
  const int errors = handler_.MessagesOfType(kError);

  last_abi_->republished_size = kResized;  // and no file for it
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(closes + 1, last_abi_->closes);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size())
      << "a volume file was deleted";
  now_ms_ += 1000 * 1000;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterTest, AFirstOpenWithNoVolumeFileOpensNothing) {
  const GoogleString socket_path = SocketPath("gen25.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens = last_abi_->opens;

  ASSERT_EQ(0,
            remove(FakeDaemonAbi::VolumeFileFor(volume_, kDaemonSize).c_str()));
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens) << "it opened with no volume file there";
  EXPECT_TRUE(DaemonAdapter::VolumeFiles(volume_).empty());

  DaemonCreatedItsVolume(kDaemonSize);
  now_ms_ += DaemonAdapter::kRecordCacheFirstBackoffMs;
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, AFirstOpenBesideALeftOverVolumeOfAnotherSizeStillOpens) {
  // After a resize without a purge the file of the earlier size is still
  // there, and the server starts beside it with a warning.  A worker's
  // first open must work there too: the look after the open is what guards
  // it, not the number of files.
  const GoogleString socket_path = SocketPath("gen26.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  DaemonCreatedItsVolume(32ULL << 20);
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(kDaemonSize, last_abi_->observed_volume_size);
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AReopenThatCreatedAVolumeFileStopsTheProcess) {
  // The one volume file on disk is not the one an open at the published
  // size lands on, so the open creates a second file: a volume the daemon
  // never reads.  The handle is never handed out, the process stops using
  // the cache until it ends, one error line says so -- and the file is left
  // exactly where it is: this module deletes nothing.
  const uint64_t kResized = 64ULL << 20;
  const GoogleString socket_path = SocketPath("gen18.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int closes = last_abi_->closes;
  const int errors = handler_.MessagesOfType(kError);

  last_abi_->republished_size = kResized;  // and no file for it
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(closes + 1, last_abi_->closes)
      << "only the handle that was never handed out is closed";
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size())
      << "a volume file was deleted";
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));

  // And it stays stopped, silently.
  last_abi_->republished_size = 0;
  now_ms_ += 1000 * 1000;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, AVolumeFileThatAppearsDuringAReopenStopsTheProcess) {
  // The volume file was there when it was checked, and a second one came to
  // exist before the open returned -- whoever made it.  This module cannot
  // tell which file the handle is on, so the handle is not used.
  const GoogleString socket_path = SocketPath("gen19.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int errors = handler_.MessagesOfType(kError);

  last_abi_->after_open = [this](const GoogleString&) {
    DaemonCreatedItsVolume(64ULL << 20);
  };
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());

  last_abi_->after_open = nullptr;
  now_ms_ += 1000 * 1000;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
}

#ifndef _WIN32
TEST_F(DaemonAdapterTest, AVolumeFileReplacedDuringAReopenStopsTheProcess) {
  // Same name before and after the open, but not the same file: it was
  // deleted and made again in between, and the open may be what made it.
  const GoogleString socket_path = SocketPath("gen20.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int errors = handler_.MessagesOfType(kError);

  last_abi_->after_open = [this](const GoogleString& name) {
    // Another file under the same name; made beside it and renamed over
    // it, so the two never share a file number.
    const GoogleString other = StrCat(dir_, "/replacement");
    FILE* f = fopen(other.c_str(), "w");
    ASSERT_TRUE(f != nullptr);
    fclose(f);
    ASSERT_EQ(0, rename(other.c_str(), name.c_str()));
  };
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens + 1, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size());
}

TEST_F(DaemonAdapterTest, ADirectoryShapedVolumePathIsNotReopened) {
  // When the configured path is a directory, every file inside it counts as
  // a volume file and none can be named as the daemon's.  A reopen cannot
  // be checked there, so there is none: the old handle goes out of use and
  // the process stops using the cache, with one error line.
  const GoogleString socket_path = SocketPath("gen21.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  const int opens = last_abi_->opens;
  const int errors = handler_.MessagesOfType(kError);

  ASSERT_EQ(0, mkdir(volume_.c_str(), 0700));
  DaemonPublishedGeneration(1);
  now_ms_ += DaemonAdapter::kVolumeGenerationCheckIntervalMs;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  now_ms_ += 1000 * 1000;
  EXPECT_TRUE(adapter->RecordCache() == nullptr);
  EXPECT_EQ(opens, last_abi_->opens);
  EXPECT_EQ(errors + 1, handler_.MessagesOfType(kError));
  ASSERT_EQ(0, rmdir(volume_.c_str()));
}
#endif

TEST_F(DaemonAdapterTest, DestroyingAfterCloseRecordCacheDoesNotCloseAgain) {
  const GoogleString socket_path = SocketPath("close5.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_TRUE(adapter->RecordCache() != nullptr);

  // The fake is freed with the adapter, so the destructor's behaviour is
  // counted into a test-local counter that outlives it.
  int closes_after_death = 0;
  last_abi_->external_closes = &closes_after_death;
  adapter->CloseRecordCache();
  EXPECT_EQ(1, closes_after_death);
  adapter.reset();
  EXPECT_EQ(1, closes_after_death);
}

// --- the non-opening accessor ------------------------------------------------

TEST_F(DaemonAdapterTest, RecordCacheIfOpenOpensNothingAndTouchesNothing) {
  const GoogleString socket_path = SocketPath("ifopen1.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens_after_startup = last_abi_->opens;

  // Nothing is open yet, so a retry would be due for a caller that opens.
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
  EXPECT_EQ(opens_after_startup, last_abi_->opens) << "opened anyway";
  // A following RecordCache() still performs its own open, exactly once:
  // the accessor consumed nothing from the retry bookkeeping.
  EXPECT_TRUE(adapter->RecordCache() != nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, RecordCacheIfOpenReturnsTheOpenHandle) {
  const GoogleString socket_path = SocketPath("ifopen2.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));

  void* cache = adapter->RecordCache();
  ASSERT_TRUE(cache != nullptr);
  EXPECT_EQ(cache, adapter->RecordCacheIfOpen());
}

TEST_F(DaemonAdapterTest, RecordCacheIfOpenIsNullAfterClose) {
  const GoogleString socket_path = SocketPath("ifopen3.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));

  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  adapter->CloseRecordCache();
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
}

TEST_F(DaemonAdapterTest, RecordCacheIfOpenNeverWaitsOnAnOpenInFlight) {
  const GoogleString socket_path = SocketPath("ifopen4.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);

  std::atomic<int> open_gate(0);
  std::atomic<bool> open_reached(false);
  last_abi_->open_gate = &open_gate;
  last_abi_->open_gate_witness = &open_reached;
  std::mutex finished_mutex;
  std::condition_variable finished_cv;
  bool finished = false;
  std::thread opening([&]() { adapter->RecordCache(); });
  // Bounds the OPEN: releases the gate after 1500 ms at the latest, or at
  // once when the measurement below is over.
  std::thread watchdog([&]() {
    std::unique_lock<std::mutex> lock(finished_mutex);
    finished_cv.wait_for(lock, std::chrono::milliseconds(1500),
                         [&]() { return finished; });
    open_gate.store(1);
  });
  // Declared AFTER both threads, so it is destroyed first: on every way out
  // of this body -- a fatal assertion included -- it wakes the watchdog,
  // opens the gate and joins both threads, and no joinable thread is ever
  // destroyed.
  struct FinishAndJoin {
    ~FinishAndJoin() {
      {
        std::lock_guard<std::mutex> lock(*mutex);
        *finished = true;
      }
      cv->notify_all();
      gate->store(1);
      opening->join();
      watchdog->join();
    }
    std::mutex* mutex;
    std::condition_variable* cv;
    bool* finished;
    std::atomic<int>* gate;
    std::thread* opening;
    std::thread* watchdog;
  } finish_and_join{&finished_mutex, &finished_cv, &finished,
                    &open_gate,      &opening,     &watchdog};

  // Bounds the WAIT FOR the open: a RecordCache() that returns without ever
  // reaching the fake's open (a backoff window, a closed or given-up
  // adapter) must fail this case, not spin it into the suite's timeout.
  const std::chrono::steady_clock::time_point reached_by =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!open_reached.load() &&
         std::chrono::steady_clock::now() < reached_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // A is demonstrably inside the slow open, holding the adapter's mutex.
  ASSERT_TRUE(open_reached.load()) << "the fake's open was never reached";
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  void* answer = adapter->RecordCacheIfOpen();
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  EXPECT_TRUE(answer == nullptr);
  EXPECT_LT(elapsed_ms, 200) << elapsed_ms;
}

TEST_F(DaemonAdapterTest, RecordCacheIfOpenIsNullOutsideReady) {
  // Not configured at all.
  DaemonAdapter unconfigured("", "", &handler_);
  EXPECT_TRUE(unconfigured.RecordCacheIfOpen() == nullptr);

  // Configured but unavailable: health never reaches kReady, so there is no
  // handle to report.
  library_available_ = false;
  const GoogleString socket_path = SocketPath("ifopen5.sock");
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_TRUE(adapter->RecordCacheIfOpen() == nullptr);
}

TEST_F(DaemonAdapterTest, TheIfOpenFactoryNeverOpensAndTheIfReadyOneOpens) {
  const GoogleString socket_path = SocketPath("ifopen6.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  DaemonRecordRequest request;
  HttpOptions http_options;
  MockTimer timer(new NullMutex, 0);
  const int opens_after_startup = last_abi_->opens;

  EXPECT_TRUE(MakeDaemonIproRecorderIfOpen(adapter.get(), request, http_options,
                                           &timer, &handler_) == nullptr);
  EXPECT_EQ(opens_after_startup, last_abi_->opens) << "the factory opened";

  std::unique_ptr<IproRecorder> ready_recorder(MakeDaemonIproRecorderIfReady(
      adapter.get(), request, http_options, &timer, &handler_));
  EXPECT_TRUE(ready_recorder != nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);

  // With the handle open, both factories build a recorder, and neither opens
  // again.
  std::unique_ptr<IproRecorder> open_recorder(MakeDaemonIproRecorderIfOpen(
      adapter.get(), request, http_options, &timer, &handler_));
  EXPECT_TRUE(open_recorder != nullptr);
  std::unique_ptr<IproRecorder> ready_recorder2(MakeDaemonIproRecorderIfReady(
      adapter.get(), request, http_options, &timer, &handler_));
  EXPECT_TRUE(ready_recorder2 != nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);
}

TEST_F(DaemonAdapterTest, TheIfOpenServeFactoryNeverOpensAndNeedsAHandle) {
  const GoogleString socket_path = SocketPath("serveifopen.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens_after_startup = last_abi_->opens;
  const int64 readers_before = DaemonServeReadersConstructed();

  // (a) No handle open: nullptr, no reader constructed, no open performed.
  EXPECT_TRUE(MakeDaemonServeReaderIfOpen(adapter.get()) == nullptr);
  EXPECT_EQ(readers_before, DaemonServeReadersConstructed());
  EXPECT_EQ(opens_after_startup, last_abi_->opens) << "the factory opened";

  // (b) With the handle open: a reader, constructed +1, still just the one
  // open (the explicit RecordCache(), which is not the factory's doing).
  ASSERT_TRUE(adapter->RecordCache() != nullptr);
  std::unique_ptr<DaemonServeReader> reader(
      MakeDaemonServeReaderIfOpen(adapter.get()));
  EXPECT_TRUE(reader != nullptr);
  EXPECT_EQ(readers_before + 1, DaemonServeReadersConstructed());
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);

  // (d) After the close: nullptr again, and no new reader.  The reader goes
  // first: a close comes after the last reader is destroyed.
  reader.reset();
  adapter->CloseRecordCache();
  EXPECT_TRUE(MakeDaemonServeReaderIfOpen(adapter.get()) == nullptr);
  EXPECT_EQ(readers_before + 1, DaemonServeReadersConstructed());
}

TEST_F(DaemonAdapterTest, TheIfOpenServeFactoryNeedsTheReadyVerdict) {
  // (c) Health not ready and no handle: nullptr.  These two adapters never
  // held a handle, so the handle check alone would refuse them too; the case
  // that pins the health check itself is the next test.
  DaemonAdapter unconfigured("", "", &handler_);
  EXPECT_TRUE(MakeDaemonServeReaderIfOpen(&unconfigured) == nullptr);

  const GoogleString socket_path = SocketPath("serveifopen-dead.sock");
  DaemonCreatedItsVolume(kDaemonSize);
  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_TRUE(MakeDaemonServeReaderIfOpen(adapter.get()) == nullptr);
}

TEST_F(DaemonAdapterTest,
       TheIfOpenServeFactoryNeedsTheReadyVerdictEvenWithAHandle) {
  // (c) proper.  A startup check that runs again drops the verdict on entry
  // and leaves the published handle alone, so "a handle with no ready
  // verdict" is a state the adapter can be in.  The factory must refuse it.
  const GoogleString socket_path = SocketPath("serveifopen-drop.sock");
  std::unique_ptr<DaemonAdapter> adapter;
  {
    ListeningSocket listener(socket_path);
    ASSERT_TRUE(listener.bound());
    adapter.reset(ReadyAdapter(socket_path));
    ASSERT_TRUE(adapter->RecordCache() != nullptr);
  }  // The socket goes away.
  DaemonAdapter::ResetSocketVerdictsForTesting();
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_EQ(DaemonHealth::kUnavailable, adapter->health());
  ASSERT_TRUE(adapter->RecordCacheIfOpen() != nullptr)
      << "the handle is still published";
  const int64 readers_before = DaemonServeReadersConstructed();
  EXPECT_TRUE(MakeDaemonServeReaderIfOpen(adapter.get()) == nullptr);
  EXPECT_EQ(readers_before, DaemonServeReadersConstructed());
}

TEST_F(DaemonAdapterTest, TheIfReadyServeFactoryOpens) {
  // The IfReady side of the shared helper: with no handle it performs the
  // open (one more than startup's) and builds the reader.
  const GoogleString socket_path = SocketPath("serveifready.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());
  std::unique_ptr<DaemonAdapter> adapter(ReadyAdapter(socket_path));
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens_after_startup = last_abi_->opens;
  const int64 readers_before = DaemonServeReadersConstructed();

  std::unique_ptr<DaemonServeReader> reader(
      MakeDaemonServeReaderIfReady(adapter.get()));
  EXPECT_TRUE(reader != nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);
  EXPECT_EQ(readers_before + 1, DaemonServeReadersConstructed());

  // A second call with the handle already open builds another reader and
  // does not open again.
  std::unique_ptr<DaemonServeReader> reader2(
      MakeDaemonServeReaderIfReady(adapter.get()));
  EXPECT_TRUE(reader2 != nullptr);
  EXPECT_EQ(opens_after_startup + 1, last_abi_->opens);
  EXPECT_EQ(readers_before + 2, DaemonServeReadersConstructed())
      << "the second call must build its own reader";
}

TEST_F(DaemonAdapterTest, TheIfReadyServeFactoryNeedsTheReadyVerdict) {
  DaemonAdapter unconfigured("", "", &handler_);
  EXPECT_TRUE(MakeDaemonServeReaderIfReady(&unconfigured) == nullptr);

  // With the verdict dropped after a handle was opened, the opening factory
  // refuses too, builds nothing and opens nothing.
  const GoogleString socket_path = SocketPath("serveifready-drop.sock");
  std::unique_ptr<DaemonAdapter> adapter;
  {
    ListeningSocket listener(socket_path);
    ASSERT_TRUE(listener.bound());
    adapter.reset(ReadyAdapter(socket_path));
    ASSERT_TRUE(adapter->RecordCache() != nullptr);
  }
  DaemonAdapter::ResetSocketVerdictsForTesting();
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_EQ(DaemonHealth::kUnavailable, adapter->health());
  ASSERT_TRUE(last_abi_ != nullptr);
  const int opens_before = last_abi_->opens;
  const int64 readers_before = DaemonServeReadersConstructed();
  EXPECT_TRUE(MakeDaemonServeReaderIfReady(adapter.get()) == nullptr);
  EXPECT_EQ(opens_before, last_abi_->opens) << "the factory opened";
  EXPECT_EQ(readers_before, DaemonServeReadersConstructed());
}

// --- the RAM tier -----------------------------------------------------------

TEST_F(DaemonAdapterTest, VolumeIsOpenedWithTheRamTierOff) {
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("ram.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  ASSERT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_TRUE(last_abi_ != nullptr);
  ASSERT_EQ(1, last_abi_->opens);
  // The fake offers an 8 MiB tier, so inheriting the peer's default would fail
  // this rather than pass it by luck.
  EXPECT_EQ(0u, last_abi_->observed_ram_cache_size);
}

TEST_F(DaemonAdapterTest, InheritedSizingForcesTheRamTierOffEvenOnRefusal) {
  PsCacheConfig config;
  memset(&config, 0, sizeof(config));
  config.ram_cache_size = 64 << 20;
  EXPECT_FALSE(DaemonAdapter::ApplyInheritedSizing(&config, 0));
  EXPECT_EQ(0u, config.ram_cache_size);
}

TEST_F(DaemonAdapterTest, InheritedSizingRefusesAnUnknownSize) {
  PsCacheConfig config;
  memset(&config, 0, sizeof(config));
  config.volume_size = 12345;
  EXPECT_FALSE(DaemonAdapter::ApplyInheritedSizing(&config, 0));
  EXPECT_NE(0u, config.volume_size) << "0 was substituted as a size";
  EXPECT_TRUE(DaemonAdapter::ApplyInheritedSizing(&config, kDaemonSize));
  EXPECT_EQ(kDaemonSize, config.volume_size);
}

TEST_F(DaemonAdapterTest, TheRamTierMirrorIsZero) {
  EXPECT_EQ(0u, DaemonAdapter::kMirroredRamCacheSizeBytes);
}

// --- the config-init buffer -------------------------------------------------

TEST_F(DaemonAdapterTest, TheConfigInitBufferIsLargerThanTheStruct) {
  // The peer's plain initializer writes ITS struct's worth before anything
  // here can check the size it stamped, so the only protection against a
  // daemon whose struct has grown is slack the module allocated.
  EXPECT_GT(kCacheConfigProbeBytes, sizeof(PsCacheConfig));
}

TEST_F(DaemonAdapterTest, AGrownPeerStructDoesNotEscapeTheBuffer) {
  // A future daemon whose struct is bigger than this build's, using the
  // size-UNAWARE initializer: it writes past sizeof(PsCacheConfig), and must
  // still land inside what this module owns.
  sized_init_ = false;
  DaemonCreatedItsVolume(kDaemonSize);
  const GoogleString socket_path = SocketPath("grown.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  // The peer's struct is bigger than this build's, so its size-unaware
  // initializer writes past sizeof(PsCacheConfig). Chosen to overrun the bare
  // struct while still landing inside the probe buffer -- which is the whole
  // property. Under ASan this is the case that would report a stack overflow
  // if the adapter ever handed the initializer a bare struct.
  grown_struct_bytes_ = sizeof(PsCacheConfig) * 3;
  ASSERT_GT(grown_struct_bytes_, sizeof(PsCacheConfig));
  ASSERT_LE(grown_struct_bytes_, kCacheConfigProbeBytes);

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  ASSERT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  ASSERT_TRUE(last_abi_ != nullptr);
  // The peer genuinely wrote past a bare struct. Nothing here catches the
  // overrun by inspection -- ASan does, by reporting a stack-buffer-overflow
  // on this test if the adapter ever passes storage that is merely
  // sizeof(PsCacheConfig). This assertion is what stops the case going quiet
  // if the fake is later changed to write less.
  EXPECT_GT(last_abi_->observed_write_bytes, sizeof(PsCacheConfig))
      << "the fake did not overrun the bare struct, so this proved nothing";
}

// --- the REAL loader ---------------------------------------------------------
//
// Everything above injects a loader, so none of it executes LoadDaemonAbi:
// not the symbol binding, not the version gate, and not the layout handshake.
// These drive the real thing against a real shared object.

class LoadDaemonAbiTest : public testing::Test {
 protected:
  void SetUp() override {
    // Each case sets what it needs; start from a known state so a leftover
    // from a previous case cannot make a later one pass.
    for (const char* v :
         {"PS_STUB_MAJOR", "PS_STUB_MINOR", "PS_STUB_STRUCT_SIZE",
          "PS_STUB_SCRIBBLE", "PS_STUB_VOLUME_SIZE", "PS_STUB_GENERATION"}) {
      unsetenv(v);
    }
  }

  static GoogleString StubPath(const GoogleString& name) {
    return StubLibraryPath(name.c_str());
  }

  GoogleString error_;
};

TEST_F(LoadDaemonAbiTest, LoadsAWellBehavedLibrary) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_EQ(kRequiredAbiMajor, abi->VersionMajor());
  EXPECT_TRUE(abi->HasSizedCacheConfigInit());
  EXPECT_TRUE(abi->PublishesVolumeSize());
}

TEST_F(LoadDaemonAbiTest, TheSizedInitializerIsToldThisBuildsStructSize) {
  // The defect this pins: handing the sized spelling the size of the probe
  // BUFFER makes it describe a struct the caller does not have, and the
  // handshake then rejects every library that implements the contract
  // correctly -- so no daemon version can ever be usable.
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;

  alignas(std::max_align_t) unsigned char storage[kCacheConfigProbeBytes];
  memset(storage, kCacheConfigProbeCanary, sizeof(storage));
  PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(storage);
  abi->CacheConfigInit(config);
  EXPECT_EQ(sizeof(PsCacheConfig), config->struct_size);
}

TEST_F(LoadDaemonAbiTest, LoadsALibraryWithoutTheSizedInitializer) {
  // An older daemon package. It must still load: the sized initializer is an
  // optional improvement, not a requirement.
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub_unsized.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasSizedCacheConfigInit());
}

TEST_F(LoadDaemonAbiTest, BindsTheGenerationReaderWhenPresent) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->PublishesGeneration());
  EXPECT_EQ(DaemonAdapter::kCacheDirGeneration,
            abi->SharedConfigGeneration("/unused"));
  setenv("PS_STUB_GENERATION", "1", 1);
  EXPECT_EQ(1u, abi->SharedConfigGeneration("/unused"));
}

TEST_F(LoadDaemonAbiTest, LoadsALibraryWithoutTheGenerationReader) {
  // A pre-H1 daemon package exports no cache_dir_generation reader. It must
  // still load: the generation is bound OPTIONALLY, and its absence is the
  // adapter's legacy-layout case, not a refusal.
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub_no_generation.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->PublishesGeneration());
  EXPECT_EQ(0u, abi->SharedConfigGeneration("/unused"));
}

TEST_F(LoadDaemonAbiTest, BindsTheLastErrorReporterWhenPresent) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  ASSERT_TRUE(abi->LastErrorMessage() != nullptr);
  EXPECT_STREQ("stub reason", abi->LastErrorMessage());
}

TEST_F(LoadDaemonAbiTest, LoadsALibraryWithoutTheLastErrorReporter) {
  // A daemon package from before the per-failure explanation was published.
  // Bound OPTIONALLY, so it must still load; the whole cost of its absence is
  // that an error line carries the error class alone -- which is the line
  // this module emitted before the reporter existed.
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub_no_last_error.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->LastErrorMessage() == nullptr);
}

TEST_F(LoadDaemonAbiTest, RefusesAMismatchedMajorVersion) {
  setenv("PS_STUB_MAJOR", "2", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("API")) << error_;
}

TEST_F(LoadDaemonAbiTest, RefusesAMinorBelowTheFloor) {
  // The floor is a REFUSAL, not a degrade, and this is the case that says so.
  // A daemon one minor short does not RESERVE the flag bit the record arm
  // stamps and the serve arm falls through on, and there is no safe answer for
  // that: the byte's unknown bits ride through verbatim, so this module would
  // be the only thing that believed 0x08 meant anything while some later 1.x
  // could allocate it for something else. The minor below THAT is missing the
  // store-side `Vary: Accept` predicate, whose absence is the
  // silent-wrong-bytes outcome. Either way the library is refused at startup,
  // in the operator's log, rather than used with one rule quietly missing.
  setenv("PS_STUB_MINOR", "7", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("1.7")) << error_;
  EXPECT_NE(GoogleString::npos,
            error_.find(StrCat(IntegerToString(kRequiredAbiMajor), ".",
                               IntegerToString(kRequiredAbiMinor))))
      << error_;
}

TEST_F(LoadDaemonAbiTest, AcceptsALaterMinor) {
  // The minor is a FLOOR, not an equality: a newer daemon must still load, or
  // every daemon release would strand every module build before it.
  setenv("PS_STUB_MINOR", "9", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi != nullptr) << error_;
}

TEST_F(LoadDaemonAbiTest, RefusesALibraryMissingTheVaryAcceptPredicate) {
  // The library REPORTS a version at or above the floor and does not export
  // what that version publishes. This is the only case that distinguishes a
  // REQUIRED binding from an optional one -- against a truthful library the
  // two are indistinguishable -- so without it "the Vary-Accept predicate is
  // in the floor" would be a comment rather than a property. An optional
  // binding
  // would load this library and then call a null pointer on the first
  // response, or silently answer "does not vary on Accept" for every one.
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub_no_varies_accept.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("ps_vary_varies_accept")) << error_;
}

TEST_F(LoadDaemonAbiTest, TheVersionIsAskedBeforeTheRestOfTheSurface) {
  // Ordering, and it is diagnostics rather than safety: both paths refuse. A
  // daemon below the floor is missing entry points BY DEFINITION, so binding
  // first makes it fail on whichever symbol it happens to lack -- which reads
  // to an operator as a broken package rather than an old one. This library is
  // both too old and missing the symbol; the message must be the version one,
  // which names what it has, what is needed, and therefore what to do.
  setenv("PS_STUB_MINOR", "6", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub_no_varies_accept.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("reports API 1.6")) << error_;
  EXPECT_EQ(GoogleString::npos, error_.find("does not export")) << error_;
}

TEST_F(LoadDaemonAbiTest, RefusesAnUnsizedLibraryWithADifferentLayout) {
  // On the unsized path the peer stamps its OWN sizeof, so a disagreement is
  // real and the library must be refused before any field is used.
  setenv("PS_STUB_STRUCT_SIZE", "96", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub_unsized.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("layout")) << error_;
}

TEST_F(LoadDaemonAbiTest, ASizedLibraryEchoingASizeIsNotTreatedAsAMismatch) {
  // The mirror image of the case above, and the reason the two paths are
  // checked differently: on the sized path struct_size is whatever we passed
  // in, so it can never disagree and must never be read as disagreement.
  setenv("PS_STUB_STRUCT_SIZE", "96", 1);  // ignored by the sized spelling
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi != nullptr) << error_;
}

TEST_F(LoadDaemonAbiTest, RefusesASizedLibraryThatWritesPastTheStatedSize) {
  // What IS checkable on the sized path: the promise that made it preferable.
  setenv("PS_STUB_SCRIBBLE", "16", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("wrote past")) << error_;
}

TEST_F(LoadDaemonAbiTest, ReportsAMissingLibraryWithoutCrashing) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi("/nonexistent/libpagespeed.so", &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_FALSE(error_.empty());
}

#ifdef _WIN32
TEST_F(LoadDaemonAbiTest, ATextFileNamedAsALibraryIsRefusedWithItsPath) {
  // Errors are values, on every refusal shape: a file that is no library at
  // all returns nullptr with the path AND a rendered reason in the message,
  // not a crash, not a dialog.
  const GoogleString path = StrCat(GTestTempDir(), "\\not-a-library.dll");
  FILE* f = fopen(path.c_str(), "wb");
  ASSERT_TRUE(f != nullptr) << path;
  ASSERT_EQ(4, static_cast<int>(fwrite("text", 1, 4, f)));
  fclose(f);
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi(path, &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find(path)) << error_;
  EXPECT_NE(GoogleString::npos, error_.find("cannot load")) << error_;
  EXPECT_GT(error_.size(), path.size() + 20)
      << "the message must carry a rendered reason, not the path alone: "
      << error_;
  EXPECT_EQ(0, _unlink(path.c_str()));
}

TEST_F(LoadDaemonAbiTest, AWrongArchitectureLibraryIsRefusedWithItsPath) {
  // A 32-bit system library that is NOT a KnownDLL stands in for the wrong
  // architecture (a KnownDLL can be satisfied from the already-mapped 64-bit
  // copy on some builds, turning the case red for the wrong reason).  The
  // directory comes from the system, not from a hardcoded drive.
  char dir[MAX_PATH];
  const UINT length = GetSystemWow64DirectoryA(dir, sizeof(dir));
  if (length == 0 || length >= sizeof(dir)) {
    GTEST_SKIP() << "no 32-bit system directory on this machine to stand in "
                    "as the wrong architecture";
  }
  const GoogleString path = StrCat(dir, "\\wininet.dll");
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    GTEST_SKIP() << "no 32-bit system library at " << path
                 << " to stand in as the wrong architecture";
  }
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi(path, &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find(path)) << error_;
  EXPECT_NE(GoogleString::npos, error_.find("cannot load")) << error_;
  EXPECT_GT(error_.size(), path.size() + 20)
      << "the message must carry a rendered reason, not the path alone: "
      << error_;
}

TEST_F(LoadDaemonAbiTest, ABareLibraryNameIsRefusedBeforeAnySearch) {
  // The loader searches NO directories on this platform: a bare name is
  // refused with its own sentence, before the API is touched.
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi("libdaemon_stub.so", &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("must be absolute")) << error_;
}

TEST_F(LoadDaemonAbiTest, ARelativeLibraryPathIsRefusedBeforeAnySearch) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi("system\\libdaemon_stub.so", &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("must be absolute")) << error_;
}

TEST_F(LoadDaemonAbiTest, AHighByteRelativePathIsRefusedWithoutUB) {
  // A valid UTF-8 relative path whose first byte is >= 0x80: the gate must
  // refuse it for its form (and never index a locale table out of range).
  const char* path = "\xE6\x97\xA5\xE6\x9C\xAC/lib.dll";
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi(path, &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("must be absolute")) << error_;
  EXPECT_NE(GoogleString::npos, error_.find(path)) << error_;
}

TEST_F(LoadDaemonAbiTest, AUncPathPassesTheAbsoluteFormGate) {
  // Both absolute FORMS are accepted: drive-letter (every loader case above)
  // and UNC.  This path fails for its target, not for its form.
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi("\\\\localhost\\C$\\no-such-directory\\x.dll", &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_EQ(GoogleString::npos, error_.find("must be absolute"))
      << "a UNC path must not be refused for its form: " << error_;

  // The same gate takes the forward-slash UNC form: the API treats the
  // separators as equivalent, so refusing it would misname a loadable path.
  error_.clear();
  std::unique_ptr<DaemonAbi> abi2(
      LoadDaemonAbi("//localhost/C$/no-such-directory/x.dll", &error_));
  EXPECT_TRUE(abi2 == nullptr);
  EXPECT_EQ(GoogleString::npos, error_.find("must be absolute"))
      << "the forward-slash UNC form must not be refused for its form: "
      << error_;
}

TEST_F(LoadDaemonAbiTest, AnEmptyPathIsItsOwnMessage) {
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi("", &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("path is empty")) << error_;
}

TEST_F(LoadDaemonAbiTest, InvalidUtf8IsItsOwnMessage) {
  const char path[] = {'D', ':', '\\', 'x', '\xFF', 'y', '\0'};
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi(path, &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("not valid UTF-8")) << error_;
}
#endif  // _WIN32

}  // namespace

}  // namespace net_instaweb
