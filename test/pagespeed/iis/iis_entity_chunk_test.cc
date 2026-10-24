// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Reading the response entity chunks OnSendResponse feeds to the rewriter:
// chunk types the module cannot read, and file-handle chunks whose file
// changes size between the length lookup and the read. The reads run
// against real temporary files opened the way IIS hands them over
// (overlapped I/O).

#include "pagespeed/iis/iis_entity_chunk.h"

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
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
  EXPECT_FALSE(
      ResolveFileChunkLength(FileChunk(0, 1ULL << 33), 0xffffffffULL, &length));
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

// Request memory stand-in for ConvertFileChunksToMemory: owns what it hands
// out, and can be told to fail.
struct TestAllocator {
  static void* Allocate(void* context, DWORD size) {
    TestAllocator* self = static_cast<TestAllocator*>(context);
    if (self->fail) {
      return nullptr;
    }
    self->buffers.emplace_back(new char[size]);
    return self->buffers.back().get();
  }
  bool fail = false;
  std::vector<std::unique_ptr<char[]>> buffers;
};

HTTP_DATA_CHUNK MemoryChunk(const char* text) {
  HTTP_DATA_CHUNK chunk;
  ZeroMemory(&chunk, sizeof(chunk));
  chunk.DataChunkType = HttpDataChunkFromMemory;
  chunk.FromMemory.pBuffer = const_cast<char*>(text);
  chunk.FromMemory.BufferLength = static_cast<ULONG>(strlen(text));
  return chunk;
}

TEST_F(IisEntityChunkTest, FileChunksBecomeMemoryChunks) {
  WriteContent(9000);
  HTTP_DATA_CHUNK chunks[4] = {
      MemoryChunk("<html>"), FileChunk(100, HTTP_BYTE_RANGE_TO_EOF),
      FileChunk(9000, HTTP_BYTE_RANGE_TO_EOF), MemoryChunk("</html>")};
  TestAllocator allocator;
  ASSERT_TRUE(ConvertFileChunksToMemory(chunks, 4, ~0ULL,
                                        &TestAllocator::Allocate, &allocator));
  for (const HTTP_DATA_CHUNK& chunk : chunks) {
    EXPECT_EQ(HttpDataChunkFromMemory, chunk.DataChunkType);
  }
  EXPECT_EQ(6u, chunks[0].FromMemory.BufferLength);
  ASSERT_EQ(8900u, chunks[1].FromMemory.BufferLength);
  const char* data = static_cast<const char*>(chunks[1].FromMemory.pBuffer);
  for (ULONGLONG i = 0; i < 8900; ++i) {
    ASSERT_EQ(ContentAt(100 + i), data[i]) << "at " << i;
  }
  // An empty range still points at a buffer.
  EXPECT_EQ(0u, chunks[2].FromMemory.BufferLength);
  EXPECT_NE(nullptr, chunks[2].FromMemory.pBuffer);
  EXPECT_EQ(7u, chunks[3].FromMemory.BufferLength);
}

// The module reads file chunks with the request buffer limit only (a DWORD),
// so a large static file, such as a resource being recorded for in-place
// optimization, is converted in full.
TEST_F(IisEntityChunkTest, LargeFileChunkIsConvertedInFull) {
  const ULONGLONG kSize = 3 << 20;
  WriteContent(kSize);
  HTTP_DATA_CHUNK chunks[1] = {FileChunk(0, HTTP_BYTE_RANGE_TO_EOF)};
  TestAllocator allocator;
  ASSERT_TRUE(ConvertFileChunksToMemory(chunks, 1, MAXDWORD,
                                        &TestAllocator::Allocate, &allocator));
  ASSERT_EQ(HttpDataChunkFromMemory, chunks[0].DataChunkType);
  ASSERT_EQ(kSize, chunks[0].FromMemory.BufferLength);
  const char* data = static_cast<const char*>(chunks[0].FromMemory.pBuffer);
  for (ULONGLONG i = 0; i < kSize; i += 4093) {
    ASSERT_EQ(ContentAt(i), data[i]) << "at " << i;
  }
  EXPECT_EQ(ContentAt(kSize - 1), data[kSize - 1]);
}

// When any file chunk cannot be read in full, none is converted, so the
// response can still go out exactly as it came.
TEST_F(IisEntityChunkTest, AFailedReadLeavesEveryChunkUnchanged) {
  WriteContent(5000);
  TestAllocator allocator;

  // The file is shorter than the second chunk's range.
  HTTP_DATA_CHUNK short_file[2] = {FileChunk(0, 100), FileChunk(0, 10000)};
  EXPECT_FALSE(ConvertFileChunksToMemory(short_file, 2, ~0ULL,
                                         &TestAllocator::Allocate, &allocator));
  EXPECT_EQ(HttpDataChunkFromFileHandle, short_file[0].DataChunkType);
  EXPECT_EQ(HttpDataChunkFromFileHandle, short_file[1].DataChunkType);

  // The range starts past the end of the file.
  HTTP_DATA_CHUNK past_end[2] = {FileChunk(0, 100),
                                 FileChunk(6000, HTTP_BYTE_RANGE_TO_EOF)};
  EXPECT_FALSE(ConvertFileChunksToMemory(past_end, 2, ~0ULL,
                                         &TestAllocator::Allocate, &allocator));
  EXPECT_EQ(HttpDataChunkFromFileHandle, past_end[0].DataChunkType);

  // The range is larger than the limit.
  HTTP_DATA_CHUNK too_large[1] = {FileChunk(0, HTTP_BYTE_RANGE_TO_EOF)};
  EXPECT_FALSE(ConvertFileChunksToMemory(too_large, 1, 4999,
                                         &TestAllocator::Allocate, &allocator));
  EXPECT_EQ(HttpDataChunkFromFileHandle, too_large[0].DataChunkType);

  // No memory.
  allocator.fail = true;
  HTTP_DATA_CHUNK no_memory[1] = {FileChunk(0, 100)};
  EXPECT_FALSE(ConvertFileChunksToMemory(no_memory, 1, ~0ULL,
                                         &TestAllocator::Allocate, &allocator));
  EXPECT_EQ(HttpDataChunkFromFileHandle, no_memory[0].DataChunkType);
}

// OnSendResponse runs on every response the module looks at. Nothing it
// meets while reading a response body (a chunk type it cannot read, a file
// it cannot read) may hit an unconditional CHECK(false); the response is
// passed on, or the connection reset once rewriting has started.
TEST(IisSendResponseSourceTest, OnSendResponseHasNoCheckFalse) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = RunfilePath("pagespeed/iis/iis_http_module.cpp");
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
  EXPECT_NE(StringPiece::npos, body.find("ConvertFileChunksToMemory("));
  EXPECT_NE(StringPiece::npos, body.find("ResetConnection()"));
}

}  // namespace
}  // namespace net_instaweb
