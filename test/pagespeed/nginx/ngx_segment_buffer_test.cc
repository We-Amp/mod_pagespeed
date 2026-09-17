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

#include "pagespeed/nginx/ngx_segment_buffer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const size_t kThreshold = NgxSegmentBuffer::kCoalesceThresholdBytes;

// Concatenates all buffered segments, mirroring what the nginx-side drain
// puts on the wire.
GoogleString Concatenate(const NgxSegmentBuffer& buffer) {
  GoogleString out;
  for (const NgxSegmentBuffer::Segment& segment : buffer.segments()) {
    NgxSegmentBuffer::SegmentValue(segment).AppendToString(&out);
  }
  return out;
}

// The bytes of segment i, for expectation brevity.
StringPiece SegmentAt(const NgxSegmentBuffer& buffer, size_t i) {
  return NgxSegmentBuffer::SegmentValue(buffer.segments()[i]);
}

// Deterministic pseudo-random generator (avoid <random> for portability of
// the exact sequence).
class Lcg {
 public:
  explicit Lcg(uint64_t seed) : state_(seed) {}
  uint64_t Next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_ >> 33;
  }

 private:
  uint64_t state_;
};

GoogleString MakeChunk(size_t size, uint64_t salt) {
  GoogleString chunk;
  chunk.reserve(size);
  for (size_t i = 0; i < size; i++) {
    chunk.push_back(static_cast<char>('a' + ((salt + i) % 26)));
  }
  return chunk;
}

TEST(NgxSegmentBufferTest, StartsEmpty) {
  NgxSegmentBuffer buffer;
  EXPECT_TRUE(buffer.empty());
  EXPECT_TRUE(buffer.segments().empty());
}

TEST(NgxSegmentBufferTest, EmptyAppendIsIgnored) {
  NgxSegmentBuffer buffer;
  buffer.Append("");
  buffer.Append(StringPiece());
  EXPECT_TRUE(buffer.empty());
  buffer.Append("x");
  buffer.Append("");
  ASSERT_EQ(1u, buffer.segments().size());
  EXPECT_EQ("x", SegmentAt(buffer, 0));
}

TEST(NgxSegmentBufferTest, SingleWriteIsSingleSegment) {
  NgxSegmentBuffer buffer;
  GoogleString body = MakeChunk(1024 * 1024, 7);  // 1MB in one write.
  buffer.Append(body);
  ASSERT_EQ(1u, buffer.segments().size());
  EXPECT_EQ(body, SegmentAt(buffer, 0));
}

TEST(NgxSegmentBufferTest, SmallWritesCoalesce) {
  NgxSegmentBuffer buffer;
  GoogleString expected;
  const size_t kWriteSize = 100;
  const size_t kNumWrites = 1000;  // 100KB total.
  for (size_t i = 0; i < kNumWrites; i++) {
    GoogleString chunk = MakeChunk(kWriteSize, i);
    buffer.Append(chunk);
    expected += chunk;
  }
  EXPECT_EQ(expected, Concatenate(buffer));
  // Coalescing must keep the segment count near total/threshold, not near
  // the number of writes.
  size_t total = kWriteSize * kNumWrites;
  EXPECT_LE(buffer.segments().size(), total / kThreshold + 1);
  // Every segment except the last must have reached the threshold, and no
  // segment may be empty.
  for (size_t i = 0; i < buffer.segments().size(); i++) {
    EXPECT_FALSE(SegmentAt(buffer, i).empty());
    if (i + 1 < buffer.segments().size()) {
      EXPECT_GE(SegmentAt(buffer, i).size(), kThreshold);
    }
  }
}

TEST(NgxSegmentBufferTest, LargeWriteCoalescesIntoSmallTail) {
  NgxSegmentBuffer buffer;
  buffer.Append("abc");
  GoogleString big = MakeChunk(1024 * 1024, 3);
  buffer.Append(big);
  // Tail was below the threshold, so the large write coalesces into it.
  ASSERT_EQ(1u, buffer.segments().size());
  EXPECT_EQ("abc" + big, SegmentAt(buffer, 0));
}

TEST(NgxSegmentBufferTest, FullTailStartsNewSegment) {
  NgxSegmentBuffer buffer;
  GoogleString first = MakeChunk(kThreshold, 1);  // Exactly at threshold.
  buffer.Append(first);
  buffer.Append("x");
  ASSERT_EQ(2u, buffer.segments().size());
  EXPECT_EQ(first, SegmentAt(buffer, 0));
  EXPECT_EQ("x", SegmentAt(buffer, 1));
}

TEST(NgxSegmentBufferTest, TakeSegmentsTransfersAndEmpties) {
  NgxSegmentBuffer buffer;
  GoogleString first = MakeChunk(kThreshold, 1);
  GoogleString second = MakeChunk(1024 * 1024, 2);
  buffer.Append(first);
  buffer.Append(second);
  ASSERT_EQ(2u, buffer.segments().size());

  std::deque<NgxSegmentBuffer::Segment> taken = buffer.TakeSegments();
  EXPECT_TRUE(buffer.empty());
  EXPECT_TRUE(buffer.segments().empty());
  ASSERT_EQ(2u, taken.size());
  EXPECT_EQ(first, NgxSegmentBuffer::SegmentValue(taken[0]));
  EXPECT_EQ(second, NgxSegmentBuffer::SegmentValue(taken[1]));

  // The buffer keeps accepting writes after a take.
  buffer.Append("tail");
  ASSERT_EQ(1u, buffer.segments().size());
  EXPECT_EQ("tail", SegmentAt(buffer, 0));

  // An empty buffer yields an empty deque.
  buffer.Clear();
  EXPECT_TRUE(buffer.TakeSegments().empty());
}

// The drain moves large taken segments into heap holders; the move must
// transfer the bytes rather than copy them (the holder keeps the original
// heap allocation).
TEST(NgxSegmentBufferTest, TakenSegmentMovesWithoutCopy) {
  NgxSegmentBuffer buffer;
  GoogleString big = MakeChunk(64 * 1024, 9);
  buffer.Append(big);

  std::deque<NgxSegmentBuffer::Segment> taken = buffer.TakeSegments();
  ASSERT_EQ(1u, taken.size());
  GoogleString* segment = std::get_if<GoogleString>(&taken.front());
  ASSERT_TRUE(segment != nullptr);
  const char* data = segment->data();
  GoogleString holder(std::move(*segment));
  EXPECT_EQ(data, holder.data());
  EXPECT_EQ(big, holder);
}

TEST(NgxSegmentBufferTest, AppendSharedStoresViewWithoutCopy) {
  NgxSegmentBuffer buffer;
  GoogleString bytes = MakeChunk(64 * 1024, 4);
  SharedString storage(bytes);
  SharedString view(storage);
  view.RemovePrefix(5);
  view.RemoveSuffix(7);
  buffer.AppendShared(view);
  ASSERT_EQ(1u, buffer.segments().size());
  StringPiece stored = SegmentAt(buffer, 0);
  EXPECT_EQ(view.Value(), stored);
  // Same storage, not a copy.
  EXPECT_EQ(view.Value().data(), stored.data());
}

TEST(NgxSegmentBufferTest, AppendNeverCoalescesIntoSharedTail) {
  NgxSegmentBuffer buffer;
  buffer.AppendShared(SharedString("shared bytes"));
  buffer.Append("owned");
  ASSERT_EQ(2u, buffer.segments().size());
  EXPECT_EQ("shared bytes", SegmentAt(buffer, 0));
  EXPECT_EQ("owned", SegmentAt(buffer, 1));
  EXPECT_EQ("shared bytesowned", Concatenate(buffer));
}

TEST(NgxSegmentBufferTest, EmptySharedViewIsIgnored) {
  NgxSegmentBuffer buffer;
  buffer.AppendShared(SharedString());
  buffer.AppendShared(SharedString(""));
  EXPECT_TRUE(buffer.empty());
}

// Models the drain handoff: a trimmed shared view survives TakeSegments and
// keeps the backing storage alive after every other reference is gone.
TEST(NgxSegmentBufferTest, SharedSegmentKeepsBackingStorageAlive) {
  NgxSegmentBuffer buffer;
  GoogleString expected;
  {
    GoogleString bytes = MakeChunk(256 * 1024, 11);
    expected = bytes.substr(100, bytes.size() - 100 - 200);
    SharedString storage(bytes);
    SharedString view(storage);
    view.RemovePrefix(100);
    view.RemoveSuffix(200);
    buffer.AppendShared(view);
  }  // 'storage' and 'view' are gone; only the buffered segment remains.

  std::deque<NgxSegmentBuffer::Segment> taken = buffer.TakeSegments();
  ASSERT_EQ(1u, taken.size());
  SharedString* segment = std::get_if<SharedString>(&taken.front());
  ASSERT_TRUE(segment != nullptr);
  // The drain's heap holder is a reference copy of the segment.
  SharedString holder(*segment);
  taken.clear();
  EXPECT_EQ(expected, holder.Value());
}

TEST(NgxSegmentBufferTest, MixedSegmentsDrainInWriteOrder) {
  NgxSegmentBuffer buffer;
  GoogleString big = MakeChunk(kThreshold, 3);
  buffer.Append("a");
  buffer.AppendShared(SharedString(big));
  buffer.Append("b");
  EXPECT_EQ("a" + big + "b", Concatenate(buffer));

  std::deque<NgxSegmentBuffer::Segment> taken = buffer.TakeSegments();
  EXPECT_TRUE(buffer.empty());
  ASSERT_EQ(3u, taken.size());
  EXPECT_TRUE(std::holds_alternative<GoogleString>(taken[0]));
  EXPECT_TRUE(std::holds_alternative<SharedString>(taken[1]));
  EXPECT_TRUE(std::holds_alternative<GoogleString>(taken[2]));
}

// Models successive flush windows: bytes written between drains must come out
// exactly once, in order, and a drain must deliver everything buffered so far.
TEST(NgxSegmentBufferTest, DrainBetweenFlushWindows) {
  NgxSegmentBuffer buffer;
  buffer.Append("first ");
  buffer.Append("window");
  EXPECT_EQ("first window", Concatenate(buffer));
  buffer.Clear();
  EXPECT_TRUE(buffer.empty());

  buffer.Append("second window");
  EXPECT_EQ("second window", Concatenate(buffer));
  buffer.Clear();
  EXPECT_TRUE(buffer.empty());  // Empty-body/final-drain case.
}

TEST(NgxSegmentBufferTest, RandomWriteSizesPreserveContent) {
  NgxSegmentBuffer buffer;
  Lcg lcg(12345);
  GoogleString expected;
  // Sizes from 1 byte to ~40KB, crossing the threshold both ways.
  for (int i = 0; i < 500; i++) {
    size_t size = 1 + (lcg.Next() % (40 * 1024));
    GoogleString chunk = MakeChunk(size, lcg.Next());
    buffer.Append(chunk);
    expected += chunk;
  }
  EXPECT_EQ(expected, Concatenate(buffer));
  for (const NgxSegmentBuffer::Segment& segment : buffer.segments()) {
    EXPECT_FALSE(NgxSegmentBuffer::SegmentValue(segment).empty());
  }
}

// Mirrors the NgxBaseFetch locking discipline: a PSOL thread appends under a
// mutex (HandleWrite) while the nginx thread concurrently drains under the
// same mutex (CollectAccumulatedWrites). All bytes must come out exactly
// once, in order.
TEST(NgxSegmentBufferTest, InterleavedAppendAndDrain) {
  NgxSegmentBuffer buffer;
  std::mutex mutex;

  Lcg sizes(999);
  GoogleString expected;
  const int kNumWrites = 20000;
  // Precompute the chunks so the writer thread holds the lock only for
  // Append, like HandleWrite does.
  std::vector<GoogleString> chunks;
  chunks.reserve(kNumWrites);
  for (int i = 0; i < kNumWrites; i++) {
    size_t size = 1 + (sizes.Next() % 3000);
    chunks.push_back(MakeChunk(size, sizes.Next()));
    expected += chunks.back();
  }

  std::thread writer([&]() {
    for (const GoogleString& chunk : chunks) {
      std::lock_guard<std::mutex> lock(mutex);
      buffer.Append(chunk);
    }
  });

  GoogleString drained;
  while (drained.size() < expected.size()) {
    std::lock_guard<std::mutex> lock(mutex);
    drained += Concatenate(buffer);
    buffer.Clear();
  }
  writer.join();
  {
    // Final drain for anything appended after the loop exited.
    std::lock_guard<std::mutex> lock(mutex);
    drained += Concatenate(buffer);
    buffer.Clear();
  }

  EXPECT_EQ(expected, drained);
}

}  // namespace
}  // namespace net_instaweb
