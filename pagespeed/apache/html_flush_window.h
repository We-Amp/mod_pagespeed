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

#ifndef PAGESPEED_APACHE_HTML_FLUSH_WINDOW_H_
#define PAGESPEED_APACHE_HTML_FLUSH_WINDOW_H_

#include <algorithm>
#include <cstddef>

namespace net_instaweb {

// How much HTML is parsed between flushes of the driver, whether the response
// arrives plain or compressed. A flush lets the driver release the events it
// has rendered, so this is what keeps the memory held for one page from
// growing with the page. A page of at most this many bytes is never flushed
// by the window.
constexpr size_t kHtmlFlushWindowBytes = 1 << 20;

// Whether a flush of the driver would release anything. While events are held
// back until the type of the document is known it would not.
template <class Driver>
bool FlushWouldRelease(const Driver& driver) {
  return !driver.EventsAreHeldBack();
}

// Cuts the bytes handed to the parser into windows and flushes between them.
//
// The input of one call can be any size (a single bucket can hold a whole
// page), so it is cut into pieces that never carry the window over its limit.
// The flush comes before the first piece that follows a full window, so a
// response that ends exactly at the limit is not flushed at all. A flush that
// cannot be done yet (see CanFlush) is retried before the next piece; the
// bytes are still counted meanwhile.
//
// Sink must provide:
//   void Parse(const char* data, size_t size);
//   bool CanFlush() const;   // a flush would release something
//   void Flush();
class HtmlFlushWindow {
 public:
  HtmlFlushWindow() : since_flush_(0) {}

  template <class Sink>
  void Add(Sink* sink, const char* data, size_t size) {
    while (size > 0) {
      if (since_flush_ >= kHtmlFlushWindowBytes && sink->CanFlush()) {
        sink->Flush();
        since_flush_ = 0;
      }
      size_t room = since_flush_ < kHtmlFlushWindowBytes
                        ? kHtmlFlushWindowBytes - since_flush_
                        : kHtmlFlushWindowBytes;
      size_t piece = std::min(size, room);
      sink->Parse(data, piece);
      since_flush_ += piece;
      data += piece;
      size -= piece;
    }
  }

  // A flush happened some other way (the origin asked for one).
  void Flushed() { since_flush_ = 0; }

  size_t since_flush() const { return since_flush_; }

 private:
  size_t since_flush_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_HTML_FLUSH_WINDOW_H_
