// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Reading the response entity chunks OnSendResponse feeds to the rewriter:
// chunk types the module cannot read, and file-handle chunks whose file
// changes size between the length lookup and the read. The reads run
// against real temporary files opened the way IIS hands them over
// (overlapped I/O).

#include "pagespeed/iis/iis_entity_chunk.h"

#include <string>
#include <thread>
#include <vector>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

constexpr char kGuardByte = '\x5a';
constexpr size_t kGuardSize = 64 * 1024;

class IisEntityChunkTest : public testing::Test {
 protected:
  void SetUp() override {
    char dir[MAX_PATH];
    ASSERT_NE(0u, GetTempPathA(MAX_PATH, dir));
    char name[MAX_PATH];
    ASSERT_NE(0u, GetTempFileNameA(dir, "psc", 0, name));
    path_ = name;
    file_ = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, file_);
  }

  void TearDown() override {
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
    }
    DeleteFileA(path_.c_str());
  }

  // Byte i of the file content.
  static char ContentAt(ULONGLONG i) { return static_cast<char>('a' + i % 23); }

  void WriteContent(ULONGLONG size) {
    std::string content(static_cast<size_t>(size), '\0');
    for (ULONGLONG i = 0; i < size; ++i) {
      content[static_cast<size_t>(i)] = ContentAt(i);
    }
    OVERLAPPED ovl;
    ZeroMemory(&ovl, sizeof(ovl));
    ovl.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    if (!WriteFile(file_, content.data(), static_cast<DWORD>(size), &written,
                   &ovl)) {
      ASSERT_EQ(static_cast<DWORD>(ERROR_IO_PENDING), GetLastError());
      ASSERT_TRUE(GetOverlappedResult(file_, &ovl, &written, TRUE));
    }
    CloseHandle(ovl.hEvent);
    ASSERT_EQ(size, written);
  }

  void Resize(ULONGLONG size) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size);
    ASSERT_TRUE(SetFilePointerEx(file_, pos, nullptr, FILE_BEGIN));
    ASSERT_TRUE(SetEndOfFile(file_));
  }

  HTTP_DATA_CHUNK FileChunk(ULONGLONG start, ULONGLONG length) {
    HTTP_DATA_CHUNK chunk;
    ZeroMemory(&chunk, sizeof(chunk));
    chunk.DataChunkType = HttpDataChunkFromFileHandle;
    chunk.FromFileHandle.FileHandle = file_;
    chunk.FromFileHandle.ByteRange.StartingOffset.QuadPart = start;
    chunk.FromFileHandle.ByteRange.Length.QuadPart = length;
    return chunk;
  }

  // Reads `length` bytes of `chunk` into a buffer followed by guard bytes,
  // on a separate thread so a read that never finishes fails the test.
  // Returns false if the read did not finish.
  bool ReadGuarded(const HTTP_DATA_CHUNK& chunk, ULONGLONG length,
                   std::vector<char>* buffer, ULONGLONG* bytes_read,
                   HRESULT* hr) {
    buffer->assign(static_cast<size_t>(length) + kGuardSize, kGuardByte);
    HANDLE done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    // The worker owns copies of everything it touches, so a read that does
    // not finish can be left behind safely.
    struct Job {
      HTTP_DATA_CHUNK chunk;
      std::vector<char> buffer;
      ULONGLONG length;
      ULONGLONG bytes_read = 0;
      HRESULT hr = E_FAIL;
      HANDLE done;
    };
    Job* job = new Job{chunk, *buffer, length};
    job->done = done;
    std::thread worker([job]() {
      job->hr = ReadFileChunkInto(job->chunk, job->buffer.data(), job->length,
                                  &job->bytes_read);
      SetEvent(job->done);
    });
    if (WaitForSingleObject(done, 10000) != WAIT_OBJECT_0) {
      worker.detach();
      return false;
    }
    worker.join();
    CloseHandle(done);
    *buffer = job->buffer;
    *bytes_read = job->bytes_read;
    *hr = job->hr;
    delete job;
    return true;
  }

  void ExpectGuardIntact(const std::vector<char>& buffer, ULONGLONG length) {
    for (size_t i = static_cast<size_t>(length); i < buffer.size(); ++i) {
      ASSERT_EQ(kGuardByte, buffer[i]) << "written past the buffer at " << i;
    }
  }

  void ExpectContent(const std::vector<char>& buffer, ULONGLONG start,
                     ULONGLONG count) {
    for (ULONGLONG i = 0; i < count; ++i) {
      ASSERT_EQ(ContentAt(start + i), buffer[static_cast<size_t>(i)])
          << "at " << i;
    }
  }

  std::string path_;
  HANDLE file_ = INVALID_HANDLE_VALUE;
};

TEST_F(IisEntityChunkTest, FragmentCacheChunksAreNotReadable) {
  HTTP_DATA_CHUNK chunks[3];
  ZeroMemory(chunks, sizeof(chunks));
  chunks[0].DataChunkType = HttpDataChunkFromMemory;
  chunks[1].DataChunkType = HttpDataChunkFromFileHandle;
  chunks[2].DataChunkType = HttpDataChunkFromFragmentCache;
  EXPECT_TRUE(IsReadableEntityChunk(chunks[0]));
  EXPECT_TRUE(IsReadableEntityChunk(chunks[1]));
  EXPECT_FALSE(IsReadableEntityChunk(chunks[2]));
  EXPECT_TRUE(AllEntityChunksReadable(chunks, 2));
  EXPECT_FALSE(AllEntityChunksReadable(chunks, 3));
  chunks[2].DataChunkType = HttpDataChunkFromFragmentCacheEx;
  EXPECT_FALSE(AllEntityChunksReadable(chunks, 3));
  EXPECT_TRUE(AllEntityChunksReadable(chunks, 0));
}

TEST_F(IisEntityChunkTest, ResolvesRangeToEndOfFile) {
  WriteContent(9000);
  ULONGLONG length = 0;
  ASSERT_TRUE(ResolveFileChunkLength(FileChunk(100, HTTP_BYTE_RANGE_TO_EOF),
                                     ~0ULL, &length));
  EXPECT_EQ(8900u, length);
  ASSERT_TRUE(ResolveFileChunkLength(FileChunk(9000, HTTP_BYTE_RANGE_TO_EOF),
                                     ~0ULL, &length));
  EXPECT_EQ(0u, length);
  ASSERT_TRUE(ResolveFileChunkLength(FileChunk(10, 20), ~0ULL, &length));
  EXPECT_EQ(20u, length);
}

TEST_F(IisEntityChunkTest, RefusesRangeStartingPastEndOfFile) {
  WriteContent(50);
  ULONGLONG length = 12345;
  EXPECT_FALSE(ResolveFileChunkLength(FileChunk(100, HTTP_BYTE_RANGE_TO_EOF),
                                      ~0ULL, &length));
}

TEST_F(IisEntityChunkTest, RefusesRangeLargerThanTheBufferLimit) {
  WriteContent(9000);
  ULONGLONG length = 0;
  EXPECT_FALSE(ResolveFileChunkLength(FileChunk(0, HTTP_BYTE_RANGE_TO_EOF),
                                      8999, &length));
  EXPECT_FALSE(ResolveFileChunkLength(FileChunk(0, 1ULL << 33), 0xffffffffULL,
                                      &length));
}

TEST_F(IisEntityChunkTest, ReadsAnUnalignedRange) {
  WriteContent(20000);
  std::vector<char> buffer;
  ULONGLONG got = 0;
  HRESULT hr = E_FAIL;
  ASSERT_TRUE(ReadGuarded(FileChunk(4000, 9000), 9000, &buffer, &got, &hr));
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(9000u, got);
  ExpectContent(buffer, 4000, 9000);
  ExpectGuardIntact(buffer, 9000);
}

// The file is cut short after its length was looked up, at an offset in the
// middle of a page.
TEST_F(IisEntityChunkTest, FileShrunkBelowAnUnalignedStart) {
  WriteContent(9000);
  ULONGLONG length = 0;
  ASSERT_TRUE(ResolveFileChunkLength(FileChunk(100, HTTP_BYTE_RANGE_TO_EOF),
                                     ~0ULL, &length));
  ASSERT_EQ(8900u, length);
  Resize(50);

  std::vector<char> buffer;
  ULONGLONG got = 0;
  HRESULT hr = E_FAIL;
  ASSERT_TRUE(ReadGuarded(FileChunk(100, length), length, &buffer, &got, &hr));
  EXPECT_LT(got, length);
  ExpectGuardIntact(buffer, length);
}

// The file is cut short at a page boundary and inside a later page.
TEST_F(IisEntityChunkTest, FileShrunkPartWayThroughTheRange) {
  for (ULONGLONG new_size : {4096ULL, 5000ULL}) {
    Resize(0);
    WriteContent(10000);
    std::vector<char> buffer;
    ULONGLONG got = 0;
    HRESULT hr = E_FAIL;
    Resize(new_size);
    ASSERT_TRUE(ReadGuarded(FileChunk(0, 10000), 10000, &buffer, &got, &hr))
        << "the read did not finish for size " << new_size;
    EXPECT_EQ(new_size, got);
    ExpectContent(buffer, 0, got);
    ExpectGuardIntact(buffer, 10000);
  }
}

// The file grows after its length was looked up: only the looked-up length
// is read.
TEST_F(IisEntityChunkTest, FileGrownAfterTheLengthLookup) {
  WriteContent(5000);
  ULONGLONG length = 0;
  ASSERT_TRUE(ResolveFileChunkLength(FileChunk(0, HTTP_BYTE_RANGE_TO_EOF),
                                     ~0ULL, &length));
  ASSERT_EQ(5000u, length);
  Resize(0);
  WriteContent(9000);

  std::vector<char> buffer;
  ULONGLONG got = 0;
  HRESULT hr = E_FAIL;
  ASSERT_TRUE(ReadGuarded(FileChunk(0, length), length, &buffer, &got, &hr));
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(5000u, got);
  ExpectContent(buffer, 0, got);
  ExpectGuardIntact(buffer, length);
}

// OnSendResponse runs on every response the module looks at. Nothing it
// meets while reading a response body (a chunk type it cannot read, a file
// it cannot read) may stop the worker process; the response is passed on
// instead.
TEST(IisSendResponseSourceTest, OnSendResponseHasNoFatalAssertions) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/iis/iis_http_module.cpp");
  ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  size_t begin = source.find("IisHttpModule::OnSendResponse(");
  ASSERT_NE(GoogleString::npos, begin);
  size_t end = source.find("IisHttpModule::OnAsyncCompletion(", begin);
  ASSERT_NE(GoogleString::npos, end);
  StringPiece body(source.data() + begin, end - begin);
  // CHECK aborts in every build; DCHECK only in debug builds.
  for (size_t pos = body.find("CHECK(false)"); pos != StringPiece::npos;
       pos = body.find("CHECK(false)", pos + 1)) {
    EXPECT_TRUE(pos > 0 && body[pos - 1] == 'D')
        << "fatal assertion at offset " << pos << " of OnSendResponse";
  }
  EXPECT_EQ(StringPiece::npos, body.find("LOG(FATAL)"));
  EXPECT_EQ(StringPiece::npos, body.find("ReadFileChunk(currentchunk"));
}

}  // namespace
}  // namespace net_instaweb
