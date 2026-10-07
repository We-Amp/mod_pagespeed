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

// Unit tests for the per-site daemon startup-check gate and the
// process-wide registry behind it.
//
// The registry owns ONE adapter per distinct (library, socket, volume)
// triple for the life of the process, which is what keeps a configuration
// rebuild from re-loading the client library.  The adapter itself is faked
// at the library binder, exactly as the shared adapter suite fakes it; the
// volumes are real files in the test's temp directory, so the startup
// check's mirror genuinely runs.  The one case that needs no fake at all --
// the REAL binder against a library that is not there -- is here too: the
// production path must spell the absolute path it tried.

#include "pagespeed/iis/iis_daemon_check.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/iis/iis_event_log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"

#ifdef _WIN32
#include <process.h>  // getpid
#endif

namespace net_instaweb {

namespace {

// A fake peer that models the one behaviour the startup check's mirror is
// written against (the same model the shared adapter suite uses): the
// volume's FILENAME is a function of the size it is opened with, and opening
// creates the file if it is not there.  Only the startup half is modelled;
// every other entry point is a no-op the check never reaches, except that
// CacheOpen can be told to fail, which is how a test drives the adapter's
// record-cache logging path after a ready verdict.
class MirrorDaemonAbi : public DaemonAbi {
 public:
  MirrorDaemonAbi(uint64_t published_size) : published_size_(published_size) {}

  int VersionMajor() const override { return kRequiredAbiMajor; }
  int VersionMinor() const override { return kRequiredAbiMinor; }

  void CacheConfigInit(PsCacheConfig* config) const override {
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
  }
  bool HasSizedCacheConfigInit() const override { return true; }
  uint64_t SharedConfigVolumeSize(const char*) const override {
    return published_size_;
  }
  bool PublishesVolumeSize() const override { return true; }
  uint32_t SharedConfigGeneration(const char*) const override {
    return DaemonAdapter::kCacheDirGeneration;
  }
  bool PublishesGeneration() const override { return true; }

  int CacheOpen(const PsCacheConfig* config, void** out) const override {
    if (open_error_ != kPsOk) {
      return open_error_;
    }
    // The peer's naming: "<stem>-6-<size>" beside the configured stem.
    const GoogleString stem(config->volume_path);
    const size_t slash = stem.find_last_of("/\\");
    const GoogleString parent =
        slash == GoogleString::npos ? "." : stem.substr(0, slash);
    const GoogleString name =
        StrCat(stem.substr(slash == GoogleString::npos ? 0 : slash + 1), "-6-",
               Integer64ToString(static_cast<int64>(config->volume_size)));
    FILE* f = fopen(StrCat(parent, "/", name).c_str(), "a");
    if (f != nullptr) {
      fclose(f);
    }
    *out = const_cast<MirrorDaemonAbi*>(this);  // Placeholder.
    return kPsOk;
  }
  void CacheClose(void*) const override {}
  const char* StrError(int) const override { return "fake error"; }
  const char* LastErrorMessage() const override { return "fake reason"; }

  // Makes every later CacheOpen fail with this error.
  void set_open_error(int error) { open_error_ = error; }

  // Everything below is never reached by StartupCheck.
  void WriteParamsInit(PsWriteParams*) const override {}
  void NotifyParamsInit(PsNotifyParams*) const override {}
  int CacheWriteOriginal(void*, const char*, const char*, const char*,
                         const PsWriteParams*, void**) const override {
    return kPsErrInvalidArg;
  }
  int WriteData(void*, const void*, size_t) const override {
    return kPsErrInvalidArg;
  }
  int WriteClose(void*) const override { return kPsErrInvalidArg; }
  void WriteAbort(void*) const override {}
  int VaryUncacheable(const char*) const override { return 0; }
  int VaryVariesAccept(const char*) const override { return 0; }
  int ParseCacheControl(const char*, PsCacheControl*) const override {
    return kPsErrInvalidArg;
  }
  uint32_t AgeAdjustedInsertTime(uint32_t, uint32_t) const override {
    return 0;
  }
  uint32_t Classify(const char*, const char*, const char*,
                    const char*) const override {
    return 0;
  }
  int ClassifyContentType(const char*) const override { return 0; }
  int OptionContextSignature(const char*, size_t, char*,
                             size_t) const override {
    return kPsErrInvalidArg;
  }
  int NotifyWorker(const char*, const PsNotifyParams*) const override {
    return kPsErrInvalidArg;
  }
  int CacheReadBest(void*, const char*, const char*, const char*, uint32_t,
                    void**) const override {
    return kPsErrNotFound;
  }
  int CacheReadAlternate(void*, const char*, const char*, const char*, uint8_t,
                         void**) const override {
    return kPsErrNotFound;
  }
  int ReadContent(const void*, const uint8_t**, size_t*) const override {
    return kPsErrNotFound;
  }
  uint32_t ReadMask(const void*) const override { return 0; }
  uint8_t ReadFlags(const void*) const override { return 0; }
  int ReadContentType(const void*) const override { return 0; }
  const char* ReadOriginContentType(const void*) const override {
    return nullptr;
  }
  uint32_t ReadCacheInsertedAt(const void*) const override { return 0; }
  uint32_t ReadOriginMaxAge(const void*) const override { return 0; }
  uint32_t ReadOriginSMaxAge(const void*) const override { return 0; }
  uint16_t ReadOriginCcFlags(const void*) const override { return 0; }
  uint32_t ReadOriginLastModified(const void*) const override { return 0; }
  const char* ReadOriginEtag(const void*) const override { return nullptr; }
  const uint8_t* ReadOriginHtmlHash(const void*) const override {
    return nullptr;
  }
  int ReadIsWorkerProcessed(const void*) const override { return 0; }
  uint32_t ReadOriginContentLength(const void*) const override { return 0; }
  void ReadFree(void*) const override {}
  int EvaluateFreshness(const PsFreshnessInput*, const PsFreshnessConfig*,
                        PsFreshnessResult*) const override {
    return kPsErrInvalidArg;
  }
  int BuildCacheControl(const PsCacheControlInput*, char*, size_t, size_t*,
                        uint32_t*) const override {
    return kPsErrInvalidArg;
  }
  int ServeStatsOpen(const char*, void**) const override {
    return kPsErrNotFound;
  }
  void ServeStatsRecordServeClass(void*, int, uint32_t) const override {}
  void ServeStatsRecordHit(void*, int, uint64_t, uint64_t,
                           uint32_t) const override {}
  void ServeStatsClose(void*) const override {}

 private:
  uint64_t published_size_;
  int open_error_ = kPsOk;
};

// The binder hands the adapter a fresh fake and counts calls: the registry
// must bind once per distinct key, which is what keeps the loader reference
// count flat across configuration rebuilds.
class CountingBinder {
 public:
  explicit CountingBinder(uint64_t published_size)
      : published_size_(published_size) {}

  DaemonAbi* operator()(StringPiece, GoogleString*) {
    ++calls_;
    last_ = new MirrorDaemonAbi(published_size_);
    return last_;
  }

  int calls() const { return calls_; }
  // The fake handed to the adapter on the last call; the adapter owns it,
  // the test keeps the pointer to flip its failure modes.
  MirrorDaemonAbi* last() const { return last_; }

 private:
  uint64_t published_size_;
  std::atomic<int> calls_{0};
  MirrorDaemonAbi* last_ = nullptr;
};

// A site-side handler that records what it received, standing in for the
// factory's handler a real context registers with the registry.
class RecordingHandler : public MessageHandler {
 public:
  bool SawContaining(const GoogleString& needle) const {
    for (const GoogleString& message : messages_) {
      if (message.find(needle) != GoogleString::npos) {
        return true;
      }
    }
    return false;
  }

  int count() const { return static_cast<int>(messages_.size()); }

  GoogleString Dump() const {
    GoogleString out;
    for (const GoogleString& message : messages_) {
      StrAppend(&out, out.empty() ? "" : " | ", message);
    }
    return out;
  }

 protected:
  void MessageSImpl(MessageType type, const GoogleString& message) override {
    messages_.push_back(StrCat("[", MessageTypeToString(type), "] ", message));
  }

  void FileMessageSImpl(MessageType type, const char* /*filename*/,
                        int /*line*/, const GoogleString& message) override {
    MessageSImpl(type, message);
  }

 private:
  std::vector<GoogleString> messages_;
};

#ifdef _WIN32

// UTF-8 to UTF-16 and back for the Windows-API pieces of these tests.
std::wstring ToWide(const GoogleString& utf8) {
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                      &wide[0], length);
  return wide;
}

GoogleString ToUtf8(const std::wstring& wide) {
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                        static_cast<int>(wide.size()), nullptr,
                                        0, nullptr, nullptr);
  GoogleString utf8(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      &utf8[0], bytes, nullptr, nullptr);
  return utf8;
}

// A one-instance pipe server under a name unique to this test: the startup
// check's probe waits for a listening instance, opens it and closes it
// again at once, which is what makes a genuinely READY verdict
// constructible in a unit test.
class PipeServer {
 public:
  explicit PipeServer(const GoogleString& base_name) {
    handle_ = CreateNamedPipeW(
        ToWide(StrCat("\\\\.\\pipe\\", base_name)).c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
        nullptr);
  }

  ~PipeServer() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

#endif  // _WIN32

// A pipe base name unique to this process and call site: the machine's pipe
// namespace is shared with everything else running, and a fixed name would
// make the not-ready verdicts depend on no other test (or daemon) having
// created that pipe.
GoogleString UniquePipeName() {
  static std::atomic<int> next{0};
  return StrCat("iis_daemon_check_test_", getpid(), "_", next++);
}

void RemoveDirRecursively(const GoogleString& path) {
#ifdef _WIN32
  WIN32_FIND_DATAA data;
  HANDLE find = FindFirstFileA(StrCat(path, "/*").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(data.cFileName, ".") != 0 &&
          strcmp(data.cFileName, "..") != 0) {
        const GoogleString child = StrCat(path, "/", data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          RemoveDirRecursively(child);
        } else {
          DeleteFileA(child.c_str());
        }
      }
    } while (FindNextFileA(find, &data));
    FindClose(find);
  }
  RemoveDirectoryA(path.c_str());
#else
  DIR* directory = opendir(path.c_str());
  if (directory != nullptr) {
    while (struct dirent* entry = readdir(directory)) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      const GoogleString child = StrCat(path, "/", entry->d_name);
      if (unlink(child.c_str()) != 0) {
        RemoveDirRecursively(child);
      }
    }
    closedir(directory);
  }
  rmdir(path.c_str());
#endif
}

class IisDaemonCheckTest : public testing::Test {
 protected:
  void SetUp() override {
    // A directory unique to THIS test (name + pid): Windows reuses process
    // ids and a --gtest_repeat run reuses the process, so a stale directory
    // from an earlier run would leave volume files behind that the mirror
    // then attaches to instead of creating its own.  Removed again in
    // TearDown.
    const testing::TestInfo* info =
        testing::UnitTest::GetInstance()->current_test_info();
    dir_ = StrCat(GTestTempDir(), "/iis_daemon_check_", getpid(), "_",
                  info == nullptr ? "test" : info->name());
    RemoveDirRecursively(dir_);
    mkdir_like();
    registry_ = IisDaemonCheckRegistry::Get();
    registry_->ResetForTesting();
    // The announcement and socket-verdict latches are process-wide by
    // design; these tests assert on messages, so each starts clean.
    DaemonAdapter::ResetAnnouncementsForTesting();
    DaemonAdapter::ResetSocketVerdictsForTesting();
    SetEventLogSinkForTesting(&CountSink);
    log_count = 0;
    last_message.clear();
  }

  void TearDown() override {
    SetEventLogSinkForTesting(nullptr);
    registry_->ResetForTesting();
    RemoveDirRecursively(dir_);
  }

  void mkdir_like() {
#ifdef _WIN32
    _mkdir(dir_.c_str());
#else
    mkdir(dir_.c_str(), 0700);
#endif
  }

  // Creates a volume file for `stem` at `size` using the naming the fake
  // peer models, so a startup check that agrees with the size attaches and
  // one that disagrees creates a second file (the split).
  void CreateVolume(const GoogleString& stem, uint64_t size) {
    const size_t slash = stem.find_last_of("/\\");
    const GoogleString parent =
        slash == GoogleString::npos ? "." : stem.substr(0, slash);
    const GoogleString name =
        StrCat(stem.substr(slash == GoogleString::npos ? 0 : slash + 1), "-6-",
               Integer64ToString(static_cast<int64>(size)));
    FILE* f = fopen(StrCat(parent, "/", name).c_str(), "a");
    ASSERT_TRUE(f != nullptr);
    fclose(f);
  }

  // Installs `binder` as the registry's library binder.
  void InstallBinder(CountingBinder* binder) {
    registry_->set_binder(
        [binder](StringPiece path, GoogleString* error) -> DaemonAbi* {
          return (*binder)(path, error);
        });
  }

  static void CountSink(EventLogLevel level, const char* message) {
    ++log_count;
    last_message = message;
    last_level = level;
  }

  // File-scope sink state (the fixture default-constructs).
  static std::atomic<int> log_count;
  static GoogleString last_message;
  static EventLogLevel last_level;

  IisDaemonCheckRegistry* registry_;
  GoogleString dir_;
};

std::atomic<int> IisDaemonCheckTest::log_count{0};
GoogleString IisDaemonCheckTest::last_message;
EventLogLevel IisDaemonCheckTest::last_level = EventLogLevel::kInfo;

TEST_F(IisDaemonCheckTest, NeitherOptionTouchesTheBinderOrLogsAnything) {
  // With both daemon options unset the gate function must not reach
  // the registry at all -- the binder firing here is the failure.
  registry_->set_binder([](StringPiece, GoogleString*) -> DaemonAbi* {
    ADD_FAILURE() << "the library binder ran although neither daemon option "
                     "is set";
    return nullptr;
  });

  RecordingHandler site;
  IisDaemonCheckRegistry::Result result = CheckSite("", "", &site);

  EXPECT_EQ(nullptr, result.adapter);
  EXPECT_FALSE(result.split);
  EXPECT_EQ(0, registry_->binder_calls());
  EXPECT_EQ(0, site.count());
  EXPECT_EQ(0, log_count.load());
}

TEST_F(IisDaemonCheckTest, OneOptionAloneIsUnavailableWithOneLineAndNoLoad) {
  // Half a configuration is the shared adapter's loud mistake, answered
  // BEFORE any library loads, exactly as on the Apache port: health
  // kUnavailable, in-place optimization off, one announcement.
  CountingBinder binder(0);
  InstallBinder(&binder);
  RecordingHandler site;

  IisDaemonCheckRegistry::Result result =
      CheckSite(UniquePipeName(), "", &site);

  ASSERT_NE(nullptr, result.adapter);
  EXPECT_FALSE(result.split);
  EXPECT_EQ(DaemonHealth::kUnavailable, result.adapter->health());
  EXPECT_EQ(0, binder.calls());
  EXPECT_EQ(1, site.count());
  EXPECT_TRUE(site.SawContaining(
      "both the daemon socket path and the daemon volume path must be set"))
      << site.Dump();

  // And the other half alone: the same answer, from its own adapter.
  IisDaemonCheckRegistry::Result volume_only =
      CheckSite("", StrCat(dir_, "/half"), &site);
  ASSERT_NE(nullptr, volume_only.adapter);
  EXPECT_NE(result.adapter, volume_only.adapter);
  EXPECT_FALSE(volume_only.split);
  EXPECT_EQ(DaemonHealth::kUnavailable, volume_only.adapter->health());
  EXPECT_EQ(0, binder.calls());
  // Its line is word for word the one just announced, and the adapter
  // announces a distinct line once per process: still one.
  EXPECT_EQ(1, site.count()) << site.Dump();
  EXPECT_EQ(0, log_count.load());
}

TEST_F(IisDaemonCheckTest, SameKeySharesOneAdapterAndBindsOnce) {
  // The fake publishes this size and the disk agrees (the matching file
  // already exists), so the check attaches; the pipe is absent, so the
  // verdict is kUnavailable -- same key twice is still ONE adapter and ONE
  // bind, which is what a configuration rebuild asks for.
  const uint64_t published = 1ULL << 30;
  const GoogleString volume = StrCat(dir_, "/volume");
  const GoogleString pipe_name = UniquePipeName();
  CreateVolume(volume, published);
  CountingBinder binder(published);
  InstallBinder(&binder);
  RecordingHandler site;

  IisDaemonCheckRegistry::Result first = CheckSite(pipe_name, volume, &site);
  ASSERT_NE(nullptr, first.adapter);
  EXPECT_EQ(DaemonHealth::kUnavailable, first.adapter->health());
  // A second ask for the same key -- what a configuration rebuild does.
  IisDaemonCheckRegistry::Result second = CheckSite(pipe_name, volume, &site);
  ASSERT_NE(nullptr, second.adapter);
  EXPECT_EQ(DaemonHealth::kUnavailable, second.adapter->health());

  EXPECT_EQ(1, binder.calls());
  EXPECT_EQ(first.adapter, second.adapter);
  EXPECT_FALSE(first.split);
  EXPECT_FALSE(second.split);
}

TEST_F(IisDaemonCheckTest, DifferentKeysGetDifferentAdapters) {
  const uint64_t published = 1ULL << 30;
  CreateVolume(StrCat(dir_, "/one"), published);
  CreateVolume(StrCat(dir_, "/two"), published);
  CountingBinder binder(published);
  InstallBinder(&binder);
  RecordingHandler site;

  IisDaemonCheckRegistry::Result a =
      CheckSite(UniquePipeName(), StrCat(dir_, "/one"), &site);
  IisDaemonCheckRegistry::Result b =
      CheckSite(UniquePipeName(), StrCat(dir_, "/two"), &site);

  ASSERT_NE(nullptr, a.adapter);
  ASSERT_NE(nullptr, b.adapter);
  EXPECT_EQ(DaemonHealth::kUnavailable, a.adapter->health());
  EXPECT_EQ(DaemonHealth::kUnavailable, b.adapter->health());
  EXPECT_EQ(2, binder.calls());
  EXPECT_NE(a.adapter, b.adapter);
}

#ifdef _WIN32

TEST_F(IisDaemonCheckTest, APresentDaemonWithAMatchingVolumeIsReady) {
  // The one genuinely READY case: a pipe server under a unique name, and a
  // volume file whose size matches what the peer publishes.  The mirror
  // attaches and the socket answers, so the verdict is kReady.
  const uint64_t published = 1ULL << 30;
  const GoogleString volume = StrCat(dir_, "/ready");
  CreateVolume(volume, published);
  const GoogleString pipe_name = UniquePipeName();
  PipeServer server(pipe_name);
  ASSERT_TRUE(server.valid());
  CountingBinder binder(published);
  InstallBinder(&binder);
  RecordingHandler site;

  IisDaemonCheckRegistry::Result result = CheckSite(pipe_name, volume, &site);

  ASSERT_NE(nullptr, result.adapter);
  EXPECT_FALSE(result.split);
  EXPECT_EQ(DaemonHealth::kReady, result.adapter->health());
  EXPECT_EQ(1, binder.calls());
}

#endif  // _WIN32

TEST_F(IisDaemonCheckTest, TheSplitLogsOneEventEntryHoweverOftenItIsSeen) {
  // A volume on disk at one size, a daemon that publishes another: the
  // mirror's open creates a second file and the check refuses.
  const uint64_t published = 1ULL << 30;
  CreateVolume(StrCat(dir_, "/skew"), 64ULL << 20);
  CountingBinder binder(published);
  InstallBinder(&binder);
  RecordingHandler site;

  const GoogleString volume = StrCat(dir_, "/skew");
  const GoogleString pipe_name = UniquePipeName();
  IisDaemonCheckRegistry::Result first = CheckSite(pipe_name, volume, &site);
  ASSERT_TRUE(first.split);
  ASSERT_NE(nullptr, first.adapter);
  EXPECT_EQ(DaemonHealth::kUnavailable, first.adapter->health());
  EXPECT_EQ(1, log_count.load());
  EXPECT_EQ(EventLogLevel::kError, last_level);
  EXPECT_NE(GoogleString::npos,
            last_message.find("created a second volume file"));
  EXPECT_NE(GoogleString::npos,
            last_message.find("engages no PageSpeed at all"));
  // The entry's appended report names the file the check's own open
  // created: on IIS the site's log is out of reach once the site has
  // refused, so this is where an operator learns whether the check
  // removed it, or which file must be removed by hand.
  EXPECT_NE(GoogleString::npos, last_message.find("skew-6-1073741824"))
      << last_message;

  // Rebuilt configuration, another site with the same paths: the SAME
  // verdict, and NO second event-log entry.
  IisDaemonCheckRegistry::Result again = CheckSite(pipe_name, volume, &site);
  EXPECT_TRUE(again.split);
  EXPECT_EQ(1, log_count.load());
}

TEST_F(IisDaemonCheckTest, TheSplitIsOneEntryPerDistinctMessage) {
  // Two DIFFERENT volume paths that both split: two entries (the messages
  // differ), then no more.
  const uint64_t published = 1ULL << 30;
  CreateVolume(StrCat(dir_, "/s1"), 64ULL << 20);
  CreateVolume(StrCat(dir_, "/s2"), 64ULL << 20);
  CountingBinder binder(published);
  InstallBinder(&binder);
  RecordingHandler site;
  const GoogleString pipe_name = UniquePipeName();

  const IisDaemonCheckRegistry::Result results[] = {
      CheckSite(pipe_name, StrCat(dir_, "/s1"), &site),
      CheckSite(pipe_name, StrCat(dir_, "/s2"), &site),
      CheckSite(pipe_name, StrCat(dir_, "/s1"), &site)};
  for (const IisDaemonCheckRegistry::Result& result : results) {
    EXPECT_TRUE(result.split);
    ASSERT_NE(nullptr, result.adapter);
    EXPECT_EQ(DaemonHealth::kUnavailable, result.adapter->health());
  }
  EXPECT_EQ(results[0].adapter, results[2].adapter);
  EXPECT_EQ(2, log_count.load());
}

TEST_F(IisDaemonCheckTest, TwentyRebuildsLeaveTheHandleCountFlat) {
  // Twenty configuration-rebuild asks for the same key: the registry hands
  // back the SAME adapter, so nothing accumulates.  The load-bearing
  // assertion is binder.calls() == 1 -- on Windows a repeated library load
  // only raises the loader's reference count, which creates no kernel
  // handle, so GetProcessHandleCount cannot see the growth this registry
  // exists to prevent; it is kept as a coarse "nothing else leaked" check,
  // with the tolerance covering unrelated churn in the test process.
  const uint64_t published = 1ULL << 30;
  CreateVolume(StrCat(dir_, "/flat"), published);
  CountingBinder binder(published);
  InstallBinder(&binder);
  RecordingHandler site;
  const GoogleString volume = StrCat(dir_, "/flat");
  const GoogleString pipe_name = UniquePipeName();
  const IisDaemonCheckRegistry::Result first =
      CheckSite(pipe_name, volume, &site);
  ASSERT_NE(nullptr, first.adapter);
  EXPECT_FALSE(first.split);
  // Nothing listens on the pipe: the volume matches, the daemon is absent.
  EXPECT_EQ(DaemonHealth::kUnavailable, first.adapter->health());

#ifdef _WIN32
  DWORD before = 0;
  HANDLE process = GetCurrentProcess();
  GetProcessHandleCount(process, &before);
#endif
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(first.adapter, CheckSite(pipe_name, volume, &site).adapter);
  }
  EXPECT_EQ(1, binder.calls());
  EXPECT_EQ(DaemonHealth::kUnavailable, first.adapter->health());
#ifdef _WIN32
  DWORD after = 0;
  GetProcessHandleCount(process, &after);
  // Flat within the noise of the process; a per-rebind leak would be +20.
  EXPECT_LE(after, before + 2) << before << " -> " << after;
#endif
}

TEST_F(IisDaemonCheckTest, UnusableDaemonIsNotASplitAndLogsNothing) {
  // The daemon publishes no size: the adapter degrades with its own
  // announcement, the site keeps serving, no event-log entry, no split.
  CountingBinder binder(0);
  InstallBinder(&binder);
  RecordingHandler site;

  IisDaemonCheckRegistry::Result result =
      CheckSite(UniquePipeName(), StrCat(dir_, "/absent"), &site);

  ASSERT_NE(nullptr, result.adapter);
  EXPECT_EQ(DaemonHealth::kUnavailable, result.adapter->health());
  EXPECT_FALSE(result.split);
  EXPECT_EQ(0, log_count.load());
}

#ifdef _WIN32

TEST_F(IisDaemonCheckTest, TheDefaultLibraryPathIsAbsoluteBesideTheBinary) {
  const GoogleString path = DefaultDaemonLibraryPath();

  // Fully qualified in the loader's sense: a drive root or a UNC path,
  // never a bare name or anything relative.
  const bool drive =
      path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
  const bool unc = path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
                   path[0] == path[1];
  EXPECT_TRUE(drive || unc) << path;
  EXPECT_TRUE(StringCaseEndsWith(path, "\\pagespeed.dll") ||
              StringCaseEndsWith(path, "/pagespeed.dll"))
      << path;

  // Its directory is the directory of the running binary (in this unit
  // test that is the test executable itself; in the module it is the
  // module's own DLL, resolved the same way).
  wchar_t wide[MAX_PATH * 4];
  const DWORD written = GetModuleFileNameW(nullptr, wide, MAX_PATH * 4);
  ASSERT_GT(written, 0u);
  ASSERT_LT(written, static_cast<DWORD>(MAX_PATH * 4));
  const GoogleString binary = ToUtf8(std::wstring(wide, written));
  const size_t slash = binary.find_last_of("\\/");
  ASSERT_NE(GoogleString::npos, slash);
  EXPECT_TRUE(
      StringCaseEqual(path.substr(0, slash + 1), binary.substr(0, slash + 1)))
      << path << " vs " << binary;
}

#endif  // _WIN32

TEST_F(IisDaemonCheckTest, TheRealBinderNamesTheAbsolutePathWhenMissing) {
  // No fake: the production binder runs, and no client library sits beside
  // the test binary, so the load fails and the announcement must spell the
  // ABSOLUTE path it tried (the refusal of a non-absolute path names the
  // bare name instead, which is what makes this test fail against that
  // behaviour).
  RecordingHandler site;

  IisDaemonCheckRegistry::Result result =
      CheckSite(UniquePipeName(), StrCat(dir_, "/no-library"), &site);

  ASSERT_NE(nullptr, result.adapter);
  EXPECT_FALSE(result.split);
  EXPECT_EQ(DaemonHealth::kUnavailable, result.adapter->health());
  EXPECT_TRUE(site.SawContaining(DefaultDaemonLibraryPath()))
      << "messages seen: " << site.Dump();
}

#ifdef _WIN32

TEST_F(IisDaemonCheckTest, ARebuiltContextsHandlerReceivesTheAdaptersMessages) {
  // The adapter outlives the site context that asked for it, but a site's
  // handler dies with its context's factory.  This stages the rebuild:
  // register a handler, get the ready adapter, then "edit the
  // configuration" -- deregister the old handler (what Shutdown does) and
  // delete it -- and the adapter's later logging must reach the handler of
  // the rebuilt context only, not the deleted one.
  const uint64_t published = 1ULL << 30;
  const GoogleString volume = StrCat(dir_, "/handoff");
  CreateVolume(volume, published);
  const GoogleString pipe_name = UniquePipeName();
  PipeServer server(pipe_name);
  ASSERT_TRUE(server.valid());
  CountingBinder binder(published);
  InstallBinder(&binder);

  RecordingHandler* first = new RecordingHandler;
  IisDaemonCheckRegistry::Result ready = CheckSite(pipe_name, volume, first);
  ASSERT_NE(nullptr, ready.adapter);
  ASSERT_EQ(DaemonHealth::kReady, ready.adapter->health());

  // The configuration edit: the old context's Shutdown deregisters its
  // factory's handler, and the factory (with the handler) is deleted while
  // the registry's adapter stays.
  registry_->UnregisterSiteHandler(first);
  delete first;

  RecordingHandler rebuilt;
  IisDaemonCheckRegistry::Result again = CheckSite(pipe_name, volume, &rebuilt);
  EXPECT_EQ(ready.adapter, again.adapter);  // same key, reused

  // Drive the adapter's logging path: a failed record-cache open logs
  // through the registry's forwarding handler.
  ASSERT_NE(nullptr, binder.last());
  binder.last()->set_open_error(kPsErrNotFound);
  EXPECT_EQ(nullptr, again.adapter->RecordCache());
  EXPECT_TRUE(rebuilt.SawContaining(
      "nothing will be recorded for in-place optimization"))
      << rebuilt.Dump();
}

#endif  // _WIN32

}  // namespace

}  // namespace net_instaweb
