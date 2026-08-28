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

// Startup behaviour of the optimizer-daemon adapter.
//
// The peer is faked at exactly ONE seam: the client-library binder.  A unit
// test cannot install a daemon package, and a fake that reproduced the
// daemon's cache would be a second implementation of the contract under test.
// So the fake stands in only for what a real peer would REPORT -- its ABI
// version, its cache-volume sizing, whether its volume opens -- and every
// other input is real: real filesystem paths, a real AF_UNIX socket for the
// reachability probe, and the adapter's real decision logic throughout.

#include "pagespeed/system/daemon_adapter.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {

namespace {

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
    return publishes_size_ ? published_size_ : 0;
  }

  bool PublishesVolumeSize() const override { return publishes_size_; }

  uint32_t SharedConfigGeneration(const char* volume_path) const override {
    return publishes_generation ? published_generation : 0;
  }

  bool PublishesGeneration() const override { return publishes_generation; }

  int CacheOpen(const PsCacheConfig* config, void** out_cache) const override {
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
    *out_cache = const_cast<FakeDaemonAbi*>(this);
    return kPsOk;
  }

  void CacheClose(void* cache) const override { ++closes; }
  const char* StrError(int error) const override { return "fake error"; }

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
  void ServeStatsClose(void* /*handle*/) const override {}

  // `<stem>-6-<size>` — the shape DaemonAdapter::VolumeFiles recognises.
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

 private:
  GoogleString volume_path_;
  uint64_t published_size_;
  bool publishes_size_;
  bool sized_init_;
};

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

const uint64_t kDaemonSize = 2ULL << 30;  // NOT the peer's compiled-in default

class DaemonAdapterTest : public testing::Test {
 protected:
  DaemonAdapterTest() : handler_(new NullMutex) {
    // A SHORT directory, on purpose.  AF_UNIX paths are capped at ~108 bytes
    // and the test runner's temp directory is routinely longer than that on
    // its own, which would make every socket case fail for a reason that has
    // nothing to do with the adapter.
    dir_ = StrCat("/tmp/mps-daemon-", IntegerToString(getpid()));
    mkdir(dir_.c_str(), 0700);
    volume_ = StrCat(dir_, "/volume");
    // Process-wide by design, so each test starts from a clean slate.
    DaemonAdapter::ResetAnnouncementsForTesting();
  }

  ~DaemonAdapterTest() override {
    for (const GoogleString& name : DaemonAdapter::VolumeFiles(volume_)) {
      unlink(StrCat(dir_, "/", name).c_str());
    }
    rmdir(dir_.c_str());
  }

  // Stand in for the daemon having already created its volume at `size`.
  void DaemonCreatedItsVolume(uint64_t size) {
    FILE* f = fopen(FakeDaemonAbi::VolumeFileFor(volume_, size).c_str(), "a");
    ASSERT_TRUE(f != nullptr);
    fclose(f);
  }

  DaemonAdapter* MakeAdapter(const GoogleString& socket_path) {
    DaemonAdapter* adapter = new DaemonAdapter(socket_path, volume_, &handler_);
    adapter->set_abi_loader(
        [this](StringPiece path, GoogleString* error) -> DaemonAbi* {
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
          last_abi_ = abi;
          return abi;
        });
    return adapter;
  }

  GoogleString SocketPath(const GoogleString& name) {
    return StrCat(dir_, "/", name);
  }

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
  bool library_available_ = true;
  bool publishes_size_ = true;
  bool sized_init_ = true;
  uint64_t published_size_ = kDaemonSize;
  size_t grown_struct_bytes_ = sizeof(PsCacheConfig);
  int fake_open_error_ = kPsOk;
  bool publishes_generation_ = true;
  uint32_t published_generation_ = DaemonAdapter::kCacheDirGeneration;
  FakeDaemonAbi* last_abi_ = nullptr;
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
  EXPECT_NE(GoogleString::npos,
            Messages().find("start the pagespeed-optimizer daemon"))
      << Messages();
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
  EXPECT_EQ(DaemonAdapter::DirAccess::kAbsent,
            DaemonAdapter::VolumeDirAccess(
                StrCat(dir_, "/no-such-parent/volume")));
  EXPECT_EQ(DaemonAdapter::DirAccess::kReadable,
            DaemonAdapter::VolumeDirAccess(volume_));
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
  ASSERT_EQ(0, chmod(dir_.c_str(), 0000));
  EXPECT_EQ(DaemonAdapter::DirAccess::kDenied,
            DaemonAdapter::VolumeDirAccess(volume_));
  EXPECT_EQ(0, chmod(dir_.c_str(), 0700));
}

TEST_F(DaemonAdapterTest, AnUnlistableCacheDirectoryNamesTheGroup) {
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
  EXPECT_NE(GoogleString::npos,
            Messages().find("not in the `pagespeed` group"))
      << Messages();
  // And the volume scan must not have misread EACCES as "no volume yet" --
  // the absent message would have sent the operator to start the daemon.
  EXPECT_EQ(GoogleString::npos, Messages().find("start the daemon first"))
      << Messages();
}

TEST_F(DaemonAdapterTest, AnUnreadableSharedConfigNamesTheGroup) {
  // Same distinction one state earlier: the shared config cannot be read, so
  // the size is unknown -- but the cause is permissions, not a daemon that
  // has never run.
  if (geteuid() == 0) {
    GTEST_SKIP() << "permission bits do not bind root";
  }
  published_size_ = 0;
  const GoogleString socket_path = SocketPath("cfgdenied.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  ASSERT_EQ(0, chmod(dir_.c_str(), 0000));
  EXPECT_EQ(DaemonStartupStatus::kOk, adapter->StartupCheck());
  EXPECT_EQ(0, chmod(dir_.c_str(), 0700));
  EXPECT_EQ(DaemonHealth::kUnavailable, adapter->health());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_NE(GoogleString::npos,
            Messages().find("not in the `pagespeed` group"))
      << Messages();
  EXPECT_EQ(GoogleString::npos, Messages().find("may not be running yet"))
      << Messages();
}

TEST_F(DaemonAdapterTest, APermissionDeniedSocketNamesTheGroup) {
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
  EXPECT_NE(GoogleString::npos,
            Messages().find("not in the `pagespeed` group"))
      << Messages();
  EXPECT_EQ(GoogleString::npos,
            Messages().find("start the pagespeed-optimizer daemon"))
      << Messages();
  // The listener's destructor unlinks the socket at scope exit, after this --
  // so remove it explicitly, or the rmdir below fails on a non-empty dir.
  EXPECT_EQ(0, unlink(socket_path.c_str()));
  EXPECT_EQ(0, rmdir(subdir.c_str()));
}

// --- the cache-directory generation handshake ------------------------------
//
// Since the privilege drop the daemon's cache lives in a versioned cold-start
// directory (vN) and the daemon publishes N in its shared config as
// cache_dir_generation.  A skew is a loud handshake failure; an absent field
// is the legacy layout, tolerated and announced once.

TEST_F(DaemonAdapterTest, TheCompiledGenerationIsOne) {
  EXPECT_EQ(1u, DaemonAdapter::kCacheDirGeneration);
}

TEST_F(DaemonAdapterTest, AGenerationMismatchIsALoudHandshakeFailure) {
  // v2 vs v1: the two layouts share nothing, so the substrate goes down
  // loudly -- and the volume is NEVER opened at the other generation's
  // geometry.
  published_generation_ = 2;
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
  // "generation 0" refusing against a build for generation 1.
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
  // one condition worth refusing a start over, because it is silent.
  DaemonCreatedItsVolume(64ULL << 20);
  const GoogleString socket_path = SocketPath("split.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
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
  unlink(companion.c_str());
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
  unlink(append_form.c_str());
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
  // one condition worth refusing a start over.
  volume_ = StrCat(dir_, "/cache.vol");
  DaemonCreatedItsVolume(64ULL << 20);
  const GoogleString socket_path = SocketPath("extsplit.sock");
  ListeningSocket listener(socket_path);
  ASSERT_TRUE(listener.bound());

  std::unique_ptr<DaemonAdapter> adapter(MakeAdapter(socket_path));
  EXPECT_EQ(DaemonStartupStatus::kRefuseToStart, adapter->StartupCheck());
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
  EXPECT_EQ(2u, DaemonAdapter::VolumeFiles(volume_).size());
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
  EXPECT_EQ(StrCat(dir_, "/cache.vol-6-",
                   Integer64ToString(static_cast<int64>(kDaemonSize)),
                   ".small"),
            companion);
  FILE* f = fopen(companion.c_str(), "a");
  ASSERT_TRUE(f != nullptr);
  fclose(f);

  EXPECT_EQ(1u, DaemonAdapter::VolumeFiles(volume_).size())
      << "the small-tier companion was counted as a separate volume";
  unlink(companion.c_str());
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
  volume_ = StrCat(dir_, "/cache.");
  DaemonCreatedItsVolume(kDaemonSize);

  const std::vector<GoogleString> files = DaemonAdapter::VolumeFiles(volume_);
  ASSERT_EQ(1u, files.size())
      << "the daemon's volume is unfindable under a trailing-dot stem";
  EXPECT_EQ(FakeDaemonAbi::VolumeFileFor("cache.", kDaemonSize), files[0]);
}

TEST_F(DaemonAdapterTest, TrailingDotVolumePathAttachesToTheDaemonsVolume) {
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
    return StrCat(GTestSrcDir(), "/test/pagespeed/system/", name);
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
  EXPECT_EQ(1u, abi->SharedConfigGeneration("/unused"));
  setenv("PS_STUB_GENERATION", "2", 1);
  EXPECT_EQ(2u, abi->SharedConfigGeneration("/unused"));
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

}  // namespace

}  // namespace net_instaweb
