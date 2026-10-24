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

#include "pagespeed/iis/iis_inplace_resource_handler.h"

#include <vector>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {
const char kPageSpeedMarker[] = ".pagespeed.";
const size_t kPageSpeedMarkerLen = sizeof(kPageSpeedMarker) - 1;


}  // namespace

bool IisInPlaceResourceHandler::IsPageSpeedUrl(const StringPiece& url) {
  return url.find(kPageSpeedMarker) != StringPiece::npos;
}

bool IisInPlaceResourceHandler::DecodeUrl(const StringPiece& url,
                                           GoogleString* original_url,
                                           GoogleString* filter_id,
                                           GoogleString* hash) {
  // Find .pagespeed. marker
  size_t marker_pos = url.find(kPageSpeedMarker);
  if (marker_pos == StringPiece::npos) {
    return false;
  }

  // Get base path before .pagespeed.
  StringPiece base = url.substr(0, marker_pos);

  // Get the rest after .pagespeed.
  StringPiece rest = url.substr(marker_pos + kPageSpeedMarkerLen);

  // Parse FILTER_ID.HASH[.EXT]
  // The format is: cf.AbCdEf.css or jm.XyZ123.js
  std::vector<StringPiece> parts;
  SplitStringPieceToVector(rest, ".", &parts, false);

  if (parts.size() < 2) {
    return false;
  }

  filter_id->assign(parts[0].data(), parts[0].size());
  hash->assign(parts[1].data(), parts[1].size());

  // Reconstruct original URL
  if (parts.size() > 2) {
    // Has extension
    original_url->assign(base.data(), base.size());
    StrAppend(original_url, ".", parts[2].as_string());
  } else {
    original_url->assign(base.data(), base.size());
  }

  return true;
}

GoogleString IisInPlaceResourceHandler::EncodeUrl(
    const StringPiece& original_url,
    const StringPiece& filter_id,
    const StringPiece& hash) {
  // Find the extension
  size_t dot_pos = original_url.rfind('.');

  if (dot_pos == StringPiece::npos) {
    // No extension
    return StrCat(original_url, kPageSpeedMarker, filter_id, ".", hash);
  }

  StringPiece base = original_url.substr(0, dot_pos);
  StringPiece ext = original_url.substr(dot_pos);

  return StrCat(base, kPageSpeedMarker, filter_id, ".", hash, ext);
}

}  // namespace net_instaweb
