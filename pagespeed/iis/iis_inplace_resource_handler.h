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

#ifndef PAGESPEED_IIS_IIS_INPLACE_RESOURCE_HANDLER_H_
#define PAGESPEED_IIS_IIS_INPLACE_RESOURCE_HANDLER_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Handles In-Place Resource Optimization (IPRO) requests.
// IPRO allows PageSpeed to optimize resources (CSS, JS, images) without
// modifying the HTML that references them. The optimized resources are
// served from URLs containing ".pagespeed." markers.
class IisInPlaceResourceHandler {
 public:
  // URL format: /path/to/file.pagespeed.FILTER_ID.HASH.EXTENSION
  // Example: /styles/main.pagespeed.cf.AbCdEf.css
  //
  // Filter IDs:
  //   cf - CSS filter (combine, minify)
  //   jm - JavaScript minify
  //   jc - JavaScript combine
  //   ic - Image compress/convert
  //   ce - Cache extend

  // Decode a .pagespeed. URL to extract the original resource info.
  // Returns true if the URL is a valid PageSpeed resource URL.
  static bool DecodeUrl(const StringPiece& url,
                        GoogleString* original_url,
                        GoogleString* filter_id,
                        GoogleString* hash);

  // Check if a URL is a PageSpeed resource URL (contains .pagespeed.)
  static bool IsPageSpeedUrl(const StringPiece& url);

  // Encode an original URL into a PageSpeed resource URL.
  static GoogleString EncodeUrl(const StringPiece& original_url,
                                const StringPiece& filter_id,
                                const StringPiece& hash);

  IisInPlaceResourceHandler(const IisInPlaceResourceHandler&) = delete;
  IisInPlaceResourceHandler& operator=(const IisInPlaceResourceHandler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_INPLACE_RESOURCE_HANDLER_H_
