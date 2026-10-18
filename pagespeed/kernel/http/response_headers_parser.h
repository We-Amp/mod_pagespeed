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

#ifndef PAGESPEED_KERNEL_HTTP_RESPONSE_HEADERS_PARSER_H_
#define PAGESPEED_KERNEL_HTTP_RESPONSE_HEADERS_PARSER_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class MessageHandler;
class ResponseHeaders;

// Parses a stream of HTTP header text into a ResponseHeaders instance.
class ResponseHeadersParser {
 public:
  // Maximum accumulated bytes of one header line (name plus value).  A
  // longer line is dropped rather than buffered, bounding what a single
  // crafted response can make us hold, while the remaining headers of
  // the response still parse.
  static const size_t kMaxHeaderLineBytes = 64 * 1024;

  explicit ResponseHeadersParser(ResponseHeaders* rh) : response_headers_(rh) {
    Clear();
  }

  void Clear();

  // Parse a chunk of HTTP response header.  Returns number of bytes consumed.
  int ParseChunk(const StringPiece& text, MessageHandler* handler);

  bool headers_complete() const { return headers_complete_; }
  void set_headers_complete(bool x) { headers_complete_ = x; }

 private:
  // Reports the cap and puts the parser into line-skipping mode for the
  // current, overlong header line.
  void DropOverlongLine(MessageHandler* handler);

  ResponseHeaders* response_headers_;

  bool parsing_http_;
  bool parsing_value_;
  bool headers_complete_;
  bool skipping_overlong_line_;
  GoogleString parse_name_;
  GoogleString parse_value_;

  ResponseHeadersParser(const ResponseHeadersParser&) = delete;
  ResponseHeadersParser& operator=(const ResponseHeadersParser&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTTP_RESPONSE_HEADERS_PARSER_H_
