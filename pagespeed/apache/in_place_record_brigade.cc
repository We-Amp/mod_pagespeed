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

#include "pagespeed/apache/in_place_record_brigade.h"

#include <cstddef>

#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/ipro_recorder.h"
// clang-format off: http_protocol.h requires apache_httpd_includes.h
// (httpd.h) first; alphabetical include sorting breaks them.
#include "pagespeed/apache/apache_httpd_includes.h"
#include "http_protocol.h"  // NOLINT - for AP_BUCKET_IS_ERROR
// clang-format on

namespace net_instaweb {

apr_status_t FeedBrigadeToInPlaceRecorder(
    apr_bucket_brigade* bb, IproRecorder* recorder,
    const std::function<void()>& offer_preliminary_headers) {
  bool first = true;

  // Iterate through all buckets, saving content in the recorder.  Stop early
  // at EOS, or once the recorder no longer wants the body.
  for (apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
       !(bucket == APR_BRIGADE_SENTINEL(bb) || APR_BUCKET_IS_EOS(bucket) ||
         !recorder->wants_body());
       bucket = APR_BUCKET_NEXT(bucket)) {
    if (!APR_BUCKET_IS_METADATA(bucket)) {
      if (first) {
        first = false;
        offer_preliminary_headers();
      }

      // The headers may have just decided it (HTML, an encoded origin):
      // then not even this bucket is read.
      if (!recorder->wants_body()) {
        break;
      }

      // Content bucket.
      const char* buf = nullptr;
      size_t bytes = 0;
      // Note: Each call to apr_bucket_read() on a FILE bucket will pull in
      // some of the file into a HEAP bucket. Since we do not pass those
      // buckets to the next filter until the end of this function, we are
      // basically buffering up the entire size of the file into memory.
      //
      // Apache documentation says not to do this because of the memory issues:
      //   http://httpd.apache.org/docs/developer/output-filters.html#filtering
      // ... but since our whole point here is to load the resource into
      // memory, it seems reasonable.  It is also why the walk stops as soon
      // as the recorder no longer wants the body.
      //
      // TODO(sligocki): Should we do an APR_NONBLOCK_READ? mod_content_length
      // seems to do that, but has to deal with APR_STATUS_IS_EAGAIN() and
      // splitting the brigade, etc.
      apr_status_t return_code =
          apr_bucket_read(bucket, &buf, &bytes, APR_BLOCK_READ);
      if (return_code != APR_SUCCESS) {
        recorder->Fail();
        return return_code;
      }
      StringPiece contents(buf, bytes);
      recorder->Write(contents, recorder->handler());
    } else if (APR_BUCKET_IS_FLUSH(bucket)) {
      recorder->Flush(recorder->handler());
    } else if (AP_BUCKET_IS_ERROR(bucket)) {
      // The handler or an earlier filter ended the response abnormally (for
      // example a proxied backend that failed mid-transfer).  What has been
      // buffered is not the whole response and must never be recorded.
      recorder->Fail();
    }
  }
  return APR_SUCCESS;
}

}  // namespace net_instaweb
