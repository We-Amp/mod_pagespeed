// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/file_key_directory_cache.h"

#include <memory>

#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mem_file_system.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/base/null_thread_system.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

// 43 'A's are base64url for 32 zero bytes: a structurally valid Ed25519 key.
const char kKeyA[] =
    "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"a\","
    "\"x\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}]}";
const char kKeyB[] =
    "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"b\","
    "\"x\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}]}";
const char kPath[] = "/etc/keys.json";

class FileKeyDirectoryCacheTest : public testing::Test {
 protected:
  FileKeyDirectoryCacheTest()
      : timer_(threads_.NewMutex(), MockTimer::kApr_5_2010_ms),
        fs_(&threads_, &timer_),
        cache_(256 * 1024) {}

  void WriteKeys(const char* doc) {
    ASSERT_TRUE(fs_.WriteFile(kPath, doc, &handler_));
  }

  KeyLookupResult::Status Lookup(StringPiece kid) {
    return cache_.Get(&fs_, kPath, &handler_)->GetKey("", kid, 0).status;
  }

  NullThreadSystem threads_;
  MockTimer timer_;
  MemFileSystem fs_;
  NullMessageHandler handler_;
  FileKeyDirectoryCache cache_;
};

TEST_F(FileKeyDirectoryCacheTest, UnchangedFileIsReadOnce) {
  WriteKeys(kKeyA);
  fs_.ClearStats();
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(KeyLookupResult::kFound, Lookup("a"));
  }
  EXPECT_EQ(1, fs_.num_input_file_opens());
}

TEST_F(FileKeyDirectoryCacheTest, ChangedFileIsReadAgain) {
  WriteKeys(kKeyA);
  EXPECT_EQ(KeyLookupResult::kFound, Lookup("a"));
  // Same size, later modification time.
  timer_.AdvanceMs(2000);
  WriteKeys(kKeyB);
  fs_.ClearStats();
  EXPECT_EQ(KeyLookupResult::kNotFound, Lookup("a"));
  EXPECT_EQ(KeyLookupResult::kFound, Lookup("b"));
  EXPECT_EQ(1, fs_.num_input_file_opens());
}

TEST_F(FileKeyDirectoryCacheTest, ResizedFileIsReadAgain) {
  WriteKeys(kKeyA);
  EXPECT_EQ(KeyLookupResult::kFound, Lookup("a"));
  // Same modification time (the clock did not move), different size.
  WriteKeys("{}");
  fs_.ClearStats();
  EXPECT_EQ(KeyLookupResult::kNotFound, Lookup("a"));
  EXPECT_EQ(1, fs_.num_input_file_opens());
}

TEST_F(FileKeyDirectoryCacheTest, RemovedFileYieldsEmptyDirectory) {
  WriteKeys(kKeyA);
  EXPECT_EQ(KeyLookupResult::kFound, Lookup("a"));
  ASSERT_TRUE(fs_.RemoveFile(kPath, &handler_));
  EXPECT_EQ(KeyLookupResult::kNotFound, Lookup("a"));
  // And it is picked up again once it is back.
  WriteKeys(kKeyA);
  EXPECT_EQ(KeyLookupResult::kFound, Lookup("a"));
}

TEST_F(FileKeyDirectoryCacheTest, OversizedFileYieldsEmptyDirectory) {
  FileKeyDirectoryCache small_cache(16);
  WriteKeys(kKeyA);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            small_cache.Get(&fs_, kPath, &handler_)->GetKey("", "a", 0).status);
}

TEST_F(FileKeyDirectoryCacheTest, NoPathOrFileSystemYieldsEmptyDirectory) {
  EXPECT_EQ(KeyLookupResult::kNotFound,
            cache_.Get(&fs_, "", &handler_)->GetKey("", "a", 0).status);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            cache_.Get(nullptr, kPath, &handler_)->GetKey("", "a", 0).status);
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
