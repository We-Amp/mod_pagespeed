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

#include "ngx_segment_buffer.h"

#include <variant>

namespace net_instaweb {

void NgxSegmentBuffer::Append(StringPiece sp) {
  if (sp.empty()) {
    return;
  }
  GoogleString* tail = segments_.empty()
                           ? nullptr
                           : std::get_if<GoogleString>(&segments_.back());
  if (tail == nullptr || tail->size() >= kCoalesceThresholdBytes) {
    segments_.emplace_back(std::in_place_type<GoogleString>, sp.data(),
                           sp.size());
  } else {
    tail->append(sp.data(), sp.size());
  }
}

void NgxSegmentBuffer::AppendShared(const SharedString& view) {
  if (view.empty()) {
    return;
  }
  segments_.emplace_back(view);
}

StringPiece NgxSegmentBuffer::SegmentValue(const Segment& segment) {
  if (const GoogleString* owned = std::get_if<GoogleString>(&segment)) {
    return StringPiece(*owned);
  }
  return std::get<SharedString>(segment).Value();
}

}  // namespace net_instaweb
