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

#include "test/pagespeed/opt/logging/log_record_test_helper.h"

namespace net_instaweb {

Matcher<ImageRewriteInfo> LogImageRewriteActivityMatcher(
    const Matcher<const char*>& id, const Matcher<const GoogleString&>& url,
    const Matcher<RewriterApplication::Status>& status,
    const Matcher<bool>& is_image_inlined,
    const Matcher<bool>& is_critical_image,
    const Matcher<bool>& is_url_rewritten, const Matcher<int>& size,
    const Matcher<bool>& try_low_res_src_insertion,
    const Matcher<bool>& low_res_src_inserted,
    const Matcher<ImageType>& low_res_image_type,
    const Matcher<int>& low_res_data_size) {
  // AllOf can only take 10 arguments. So split the 11 arguments into 7+4.
  return AllOf(
      AllOf(Field(&ImageRewriteInfo::id_, id),
            Field(&ImageRewriteInfo::url_, url),
            Field(&ImageRewriteInfo::status_, status),
            Field(&ImageRewriteInfo::is_image_inlined_, is_image_inlined),
            Field(&ImageRewriteInfo::is_critical_image_, is_critical_image),
            Field(&ImageRewriteInfo::size_, size)),
      AllOf(
          Field(&ImageRewriteInfo::try_low_res_src_insertion_,
                try_low_res_src_insertion),
          Field(&ImageRewriteInfo::low_res_src_inserted_, low_res_src_inserted),
          Field(&ImageRewriteInfo::low_res_image_type_, low_res_image_type),
          Field(&ImageRewriteInfo::low_res_data_size_, low_res_data_size)));
}

}  // namespace net_instaweb
