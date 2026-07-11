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

//
// A queue of owned response-body segments for the nginx port (M0 of the
// zero-copy serving ladder). PSOL worker threads append bytes; the nginx
// thread drains whole segments and clears the queue. Compared to accumulating
// into one contiguous GoogleString, this avoids the O(n^2)-ish realloc+copy
// behavior of repeated append() on large bodies, and drops the single
// contiguous full-body heap copy.
//
// This class is NOT thread-safe: the caller (NgxBaseFetch) serializes all
// access with its own mutex, appending on PSOL threads and draining on the
// nginx thread.
//
// It deliberately has no nginx dependencies so it can be unit tested
// standalone (test/pagespeed/nginx), and so the drain can take ownership of
// whole segments instead of copying them (TakeSegments).

#ifndef NGX_SEGMENT_BUFFER_H_
#define NGX_SEGMENT_BUFFER_H_

#include <cstddef>
#include <deque>
#include <variant>

#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class NgxSegmentBuffer {
 public:
  // A segment either owns its bytes (Append copies them into a
  // GoogleString) or is a refcounted view over shared heap storage
  // (AppendShared; no byte copy).
  using Segment = std::variant<GoogleString, SharedString>;

  // Writes are coalesced into the tail segment while the tail is below this
  // size; once the tail has reached it, the next write starts a new segment.
  // Rationale: HTML rewriting emits many small writes, which must not become
  // thousands of tiny segments; 16KB caps how many bytes a single tail append
  // may re-copy on reallocation, and is a small multiple of the typical
  // ngx_pagesize (the drain splits segments into page-sized ngx_buf_t's
  // anyway, so segment boundaries need not align with pages).
  static constexpr size_t kCoalesceThresholdBytes = 16 * 1024;

  NgxSegmentBuffer() {}

  // Appends a copy of sp: into the tail segment if the tail is an owned
  // segment below kCoalesceThresholdBytes, otherwise as a new owned segment
  // (writes never coalesce into a shared tail). Empty writes are ignored,
  // so no stored segment is ever empty.
  void Append(StringPiece sp);

  // Appends a refcounted view over shared heap storage as its own segment;
  // no byte copy. The bytes stay valid for as long as any copy of the view
  // is alive, which SharedString guarantees for owned heap storage. Empty
  // views are ignored.
  void AppendShared(const SharedString& view);

  // The bytes of a segment, whichever alternative it holds.
  static StringPiece SegmentValue(const Segment& segment);

  bool empty() const { return segments_.empty(); }

  // Buffered segments, in write order. The concatenation of their bytes is
  // exactly the bytes appended since the last Clear(), in order.
  const std::deque<Segment>& segments() const { return segments_; }

  // Transfers all buffered segments to the caller, leaving this buffer
  // empty. The drain uses this to move large segments into nginx-owned
  // storage without copying their bytes.
  std::deque<Segment> TakeSegments() {
    std::deque<Segment> taken;
    taken.swap(segments_);
    return taken;
  }

  // Releases all buffered segments.
  void Clear() { segments_.clear(); }

 private:
  std::deque<Segment> segments_;

  NgxSegmentBuffer(const NgxSegmentBuffer&) = delete;
  NgxSegmentBuffer& operator=(const NgxSegmentBuffer&) = delete;
};

}  // namespace net_instaweb

#endif  // NGX_SEGMENT_BUFFER_H_
