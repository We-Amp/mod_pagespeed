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

// Windows semantics of StdioFileSystem::RenameFile.
//
// FileSystem::WriteFileAtomic (and through it PurgeContext, which persists
// cache.purge) requires POSIX rename semantics: renaming onto an existing
// path atomically replaces it. MemFileSystem honors that contract, and so
// does POSIX rename(2) -- but the Universal CRT's rename() never replaces an
// existing target, so on Windows every cache.purge update after the first
// write was silently lost (EEXIST). Additionally, Windows readers that hold
// the target open without FILE_SHARE_DELETE (any CRT fopen) block a
// replacing rename transiently, so the implementation must retry briefly.
//
// This lives in the IIS test tree (not test/pagespeed/kernel/base) because
// //test/pagespeed/iis/... is the unit-test tree the Windows CI lane builds;
// the cross-platform replace contract is covered by
// FileSystemTest::TestRenameReplace.

#include <windows.h>

#include <thread>

#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class WinRenameTest : public testing::Test {
 protected:
  void SetUp() override {
    const char* tmp = getenv("TEST_TMPDIR");
    ASSERT_TRUE(tmp != nullptr) << "bazel always sets TEST_TMPDIR";
    dir_ = StrCat(tmp, "\\win_rename_test");
    file_system_.RecursivelyMakeDir(dir_, &handler_);
    src_ = StrCat(dir_, "\\src.txt");
    dst_ = StrCat(dir_, "\\dst.txt");
  }

  void WriteFile(const GoogleString& path, StringPiece contents) {
    ASSERT_TRUE(file_system_.WriteFile(path.c_str(), contents, &handler_));
  }

  GoogleString ReadFile(const GoogleString& path) {
    GoogleString out;
    EXPECT_TRUE(file_system_.ReadFile(path.c_str(), &out, &handler_));
    return out;
  }

  StdioFileSystem file_system_;
  GoogleMessageHandler handler_;
  GoogleString dir_, src_, dst_;
};

// The baseline that already worked: renaming onto a path that does not
// exist.
TEST_F(WinRenameTest, RenameToNewPath) {
  WriteFile(src_, "new");
  file_system_.RemoveFile(dst_.c_str(), &handler_);
  EXPECT_TRUE(file_system_.RenameFile(src_.c_str(), dst_.c_str(), &handler_));
  EXPECT_EQ("new", ReadFile(dst_));
}

// The #505 defect: renaming onto an EXISTING closed file must replace it
// (POSIX contract WriteFileAtomic depends on). UCRT rename() fails EEXIST.
TEST_F(WinRenameTest, RenameReplacesExistingFile) {
  WriteFile(src_, "new");
  WriteFile(dst_, "old");
  EXPECT_TRUE(file_system_.RenameFile(src_.c_str(), dst_.c_str(), &handler_));
  EXPECT_EQ("new", ReadFile(dst_));
  EXPECT_FALSE(file_system_.Exists(src_.c_str(), &handler_).is_true());
}

// A reader holding the target open without FILE_SHARE_DELETE (what any CRT
// fopen does, e.g. PurgeContext polling cache.purge) blocks a replacing
// rename with ERROR_ACCESS_DENIED until it closes. The rename must ride out
// a brief reader window instead of failing on the first attempt.
TEST_F(WinRenameTest, RenameReplacesTargetBrieflyHeldByReader) {
  WriteFile(src_, "new");
  WriteFile(dst_, "old");
  HANDLE reader =
      CreateFileA(dst_.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE,  // no FILE_SHARE_DELETE
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(INVALID_HANDLE_VALUE, reader);
  std::thread closer([reader]() {
    Sleep(100);
    CloseHandle(reader);
  });
  EXPECT_TRUE(file_system_.RenameFile(src_.c_str(), dst_.c_str(), &handler_));
  closer.join();
  EXPECT_EQ("new", ReadFile(dst_));
}

// The retry must be bounded: a reader that never lets go fails the rename
// (with the source left in place) rather than hanging the caller, which may
// hold a named lock.
TEST_F(WinRenameTest, RenameGivesUpOnPersistentReader) {
  WriteFile(src_, "new");
  WriteFile(dst_, "old");
  HANDLE reader =
      CreateFileA(dst_.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(INVALID_HANDLE_VALUE, reader);
  ULONGLONG start_ms = GetTickCount64();
  EXPECT_FALSE(file_system_.RenameFile(src_.c_str(), dst_.c_str(), &handler_));
  ULONGLONG elapsed_ms = GetTickCount64() - start_ms;
  EXPECT_LT(elapsed_ms, 10000u) << "retry budget must be bounded";
  CloseHandle(reader);
  EXPECT_EQ("old", ReadFile(dst_));
  EXPECT_EQ("new", ReadFile(src_));  // source survives a failed rename
}

}  // namespace
}  // namespace net_instaweb
