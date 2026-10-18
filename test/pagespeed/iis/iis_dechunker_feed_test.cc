// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The chunked transfer-coding parser OnSendResponse runs over the origin's
// response body before it reaches the rewriter: chunk extensions on the
// size line, and the size accounting for a single very large feed.

#include "pagespeed/iis/dechunker.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "test/pagespeed/kernel/base/gtest.h"

namespace {

// Feeds |input| to |dechunker| in pieces of at most |piece| bytes and
// returns the decoded body. |valid| reports whether every feed was accepted.
std::string Decode(Dechunker* dechunker, const std::string& input,
                   size_t piece, bool* valid) {
  std::vector<char> copy(input.begin(), input.end());
  std::string body;
  *valid = true;
  for (size_t pos = 0; pos < copy.size(); pos += piece) {
    size_t len = std::min(piece, copy.size() - pos);
    if (!dechunker->FeedChunk(copy.data() + pos, len)) {
      *valid = false;
      break;
    }
    while (dechunker->ParseNext()) {
      char* block = nullptr;
      size_t block_len = 0;
      if (dechunker->GetDataBlock(&block, &block_len)) {
        body.append(block, block_len);
      }
    }
  }
  return body;
}

TEST(IisDechunkerFeedTest, PlainChunks) {
  Dechunker dechunker(true);
  bool valid = false;
  EXPECT_EQ("Hello World",
            Decode(&dechunker, "5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n", 64,
                   &valid));
  EXPECT_TRUE(valid);
}

TEST(IisDechunkerFeedTest, ChunkExtensionIsSkipped) {
  const std::string payload(26, 'x');
  const std::string input =
      "1a;foo=bar\r\n" + payload + "\r\n3;a;b=\"c d\"\r\nend\r\n0;last\r\n\r\n";
  Dechunker dechunker(true);
  bool valid = false;
  EXPECT_EQ(payload + "end", Decode(&dechunker, input, input.size(), &valid));
  EXPECT_TRUE(valid);
}

TEST(IisDechunkerFeedTest, ChunkExtensionAfterWhitespace) {
  Dechunker dechunker(true);
  bool valid = false;
  EXPECT_EQ("Hello", Decode(&dechunker, "5 ;name=value\r\nHello\r\n0\r\n\r\n",
                            64, &valid));
  EXPECT_TRUE(valid);
}

TEST(IisDechunkerFeedTest, ChunkExtensionSplitAcrossFeeds) {
  const std::string input =
      "4;ext=value\r\nabcd\r\n2;x\r\nef\r\n0\r\n\r\n";
  for (size_t piece = 1; piece <= input.size(); ++piece) {
    Dechunker dechunker(true);
    bool valid = false;
    // Feeds that only carry the bytes after the last chunk are refused once
    // the parser has seen the end of the body, so |valid| is not checked.
    EXPECT_EQ("abcdef", Decode(&dechunker, input, piece, &valid))
        << "piece size " << piece;
  }
}

TEST(IisDechunkerFeedTest, GarbageAfterSizeStillInvalidates) {
  Dechunker dechunker(true);
  bool valid = true;
  EXPECT_EQ("", Decode(&dechunker, "5x\r\nHello\r\n0\r\n\r\n", 64, &valid));
  char more[] = "5\r\nHello\r\n";
  EXPECT_FALSE(dechunker.FeedChunk(more, sizeof(more) - 1));
}

// A single feed longer than INT_MAX bytes inside one chunk: the parser must
// hand back exactly the bytes it was given, never the remainder of the
// declared chunk size. Only the first page of the feed is committed; the
// parser reads one byte of chunk data and passes the rest through by
// pointer and length.
TEST(IisDechunkerFeedTest, FeedLongerThanIntMaxStaysInBounds) {
  const size_t kFeed = static_cast<size_t>(0x80000002);
  const size_t kReserve = kFeed + 0x10000;
  char* region = static_cast<char*>(
      VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_NOACCESS));
  ASSERT_NE(nullptr, region);
  ASSERT_NE(nullptr, VirtualAlloc(region, 4096, MEM_COMMIT, PAGE_READWRITE));
  memset(region, 'y', 4096);

  Dechunker dechunker(true);
  char header[] = "fffffff0\r\n";
  ASSERT_TRUE(dechunker.FeedChunk(header, sizeof(header) - 1));
  EXPECT_FALSE(dechunker.ParseNext());

  ASSERT_TRUE(dechunker.FeedChunk(region, kFeed));
  ASSERT_TRUE(dechunker.ParseNext());
  char* block = nullptr;
  size_t block_len = 0;
  ASSERT_TRUE(dechunker.GetDataBlock(&block, &block_len));
  EXPECT_EQ(region, block);
  EXPECT_EQ(kFeed, block_len);
  EXPECT_FALSE(dechunker.ParseNext());
  EXPECT_FALSE(dechunker.HasData());

  EXPECT_TRUE(VirtualFree(region, 0, MEM_RELEASE));
}

}  // namespace
