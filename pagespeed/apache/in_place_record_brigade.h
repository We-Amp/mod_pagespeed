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

#ifndef PAGESPEED_APACHE_IN_PLACE_RECORD_BRIGADE_H_
#define PAGESPEED_APACHE_IN_PLACE_RECORD_BRIGADE_H_

#include <functional>

#include "apr_buckets.h"  // NOLINT

namespace net_instaweb {

class IproRecorder;

// The body walk of the in-place recording filter: feeds the content of one
// brigade to `recorder`, without passing the brigade on or changing which
// buckets it holds (beyond what reading a bucket does to it).
//
// `offer_preliminary_headers` runs once, just before the first data bucket
// of the brigade, so the recorder can decide from the headers before any
// body byte is read.
//
// The walk stops at EOS, and as soon as the recorder no longer wants the
// body (IproRecorder::wants_body): a response it will not record -- HTML,
// decided at the headers, above all -- costs no bucket reads, so a FILE
// bucket is not turned into heap buckets holding the whole file.  An ERROR
// bucket, seen while the walk runs, fails the recording.
//
// Returns APR_SUCCESS, or the status of a bucket read that failed, after
// failing the recording; the caller logs it and returns it.
apr_status_t FeedBrigadeToInPlaceRecorder(
    apr_bucket_brigade* bb, IproRecorder* recorder,
    const std::function<void()>& offer_preliminary_headers);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_IN_PLACE_RECORD_BRIGADE_H_
