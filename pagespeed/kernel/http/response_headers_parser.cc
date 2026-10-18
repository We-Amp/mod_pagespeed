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

#include "pagespeed/kernel/http/response_headers_parser.h"

#include "base/logging.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

void ResponseHeadersParser::Clear() {
  parsing_http_ = false;
  parsing_value_ = false;
  headers_complete_ = false;
  skipping_overlong_line_ = false;
  dropped_overlong_line_ = false;
}

// TODO(jmaessen): http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.2
// I bet we're doing this wrong:
//  Header fields can be extended over multiple lines by preceding each extra
//  line with at least one SP or HT.
int ResponseHeadersParser::ParseChunk(const StringPiece& text,
                                      MessageHandler* handler) {
  CHECK(!headers_complete_);
  int num_consumed = 0;
  int num_bytes = text.size();

  for (; num_consumed < num_bytes; ++num_consumed) {
    char c = text[num_consumed];
    if (skipping_overlong_line_) {
      // Drop the remainder of a line that exceeded the size cap without
      // buffering it, then resume with the next header line.
      if (c == '\n') {
        skipping_overlong_line_ = false;
        parsing_value_ = false;
      }
      continue;
    }
    if ((c == '/') && (parse_name_ == "HTTP")) {
      if (response_headers_->has_major_version()) {
        handler->MessageS(kError, "Multiple HTTP Lines");
      } else {
        parsing_http_ = true;
        parsing_value_ = true;
      }
    } else if (!parsing_value_ && (c == ':')) {
      parsing_value_ = true;
    } else if (c == '\r') {
      // Just ignore CRs for now, and break up headers on newlines for
      // simplicity.  It's not clear to me if it's important that we
      // reject headers that lack the CR in front of the LF.
    } else if (c == '\n') {
      if (parse_name_.empty()) {
        // blank line.  This marks the end of the headers.
        ++num_consumed;
        headers_complete_ = true;
        if (dropped_overlong_line_) {
          // At least one header line was dropped, so the parsed headers
          // are not the ones the origin sent: the dropped line may have
          // carried the directives that made the response uncacheable
          // (Cache-Control, Vary).  Serve the response, but make sure no
          // cache stores it.
          response_headers_->Add(HttpAttributes::kCacheControl,
                                 HttpAttributes::kNoCacheMaxAge0);
        }
        response_headers_->ComputeCaching();
        break;
      }
      if (parsing_http_) {
        response_headers_->ParseFirstLineHelper(parse_value_);
        parsing_http_ = false;
      } else {
        response_headers_->Add(parse_name_, parse_value_);
      }
      parsing_value_ = false;
      parse_name_.clear();
      parse_value_.clear();
    } else if (parsing_value_) {
      // Skip leading whitespace (form feeds don't count, unlike html)
      if (!parse_value_.empty() || !IsHtmlSpace(c) || c == '\f') {
        if (parse_name_.size() + parse_value_.size() >= kMaxHeaderLineBytes) {
          DropOverlongLine(handler);
        } else {
          parse_value_ += c;
        }
      }
    } else {
      if (parse_name_.size() + parse_value_.size() >= kMaxHeaderLineBytes) {
        DropOverlongLine(handler);
      } else {
        parse_name_ += c;
      }
    }
  }
  return num_consumed;
}

void ResponseHeadersParser::DropOverlongLine(MessageHandler* handler) {
  handler->MessageS(kError, StrCat("Header line exceeds ",
                                   Integer64ToString(kMaxHeaderLineBytes),
                                   " bytes and is dropped")
                                .c_str());
  // Reset the line state and consume the rest of it without storing it.
  // The header count cap keeps the stored set bounded; this keeps each
  // individual line bounded as well.  Remember the drop so the response
  // can be kept out of every cache when the headers complete.
  parsing_http_ = false;
  skipping_overlong_line_ = true;
  dropped_overlong_line_ = true;
  parse_name_.clear();
  parse_value_.clear();
}

}  // namespace net_instaweb
