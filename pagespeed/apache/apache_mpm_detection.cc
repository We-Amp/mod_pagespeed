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

#include "pagespeed/apache/apache_mpm_detection.h"

#include "ap_mpm.h"
#include "apr_errno.h"
#include "pagespeed/system/optimization_thread_policy.h"

namespace net_instaweb {

MpmThreadInfo QueryMpmThreadInfo() {
  // max_threads defaults to 1 so a non-threaded MPM, which is not asked for a
  // thread count, reads as single-threaded.
  MpmThreadInfo info = {false, AP_MPMQ_NOT_SUPPORTED, 1};

  int result = 0;
  if (ap_mpm_query(AP_MPMQ_IS_THREADED, &result) != APR_SUCCESS) {
    return info;  // The MPM hasn't been loaded yet; query_ok stays false.
  }
  info.is_threaded = result;
  info.query_ok = true;

  if (result == AP_MPMQ_STATIC || result == AP_MPMQ_DYNAMIC) {
    int threads = 1;
    if (ap_mpm_query(AP_MPMQ_MAX_THREADS, &threads) != APR_SUCCESS) {
      info.query_ok = false;
      return info;
    }
    info.max_threads = threads;
  }
  return info;
}

bool IsThreadedFromMpmInfo(const MpmThreadInfo& info) {
  if (!info.query_ok) {
    return false;  // Assume non-thready by default.
  }
  if (info.is_threaded != AP_MPMQ_STATIC &&
      info.is_threaded != AP_MPMQ_DYNAMIC) {
    return false;
  }
  return info.max_threads > 1;
}

const int kMpmProcessConcurrencyQuery = AP_MPMQ_MAX_DAEMONS;

MpmProcessInfo QueryMpmProcessInfo() {
  MpmProcessInfo info = {false, 0};
  int daemons = 0;
  if (ap_mpm_query(kMpmProcessConcurrencyQuery, &daemons) != APR_SUCCESS) {
    return info;  // The MPM hasn't been loaded yet; query_ok stays false.
  }
  info.query_ok = true;
  info.max_daemons = daemons;
  return info;
}

int ProcessConcurrencyFromMpmInfo(const MpmProcessInfo& info) {
  if (!info.query_ok || info.max_daemons <= 0) {
    // Either the MPM could not answer, or it answered 0 because we asked
    // before check_config computed the child count.  Neither is a number to
    // divide by, and the design record D1 says not to guess: report "unknown" and let
    // the policy take its one-thread floor.
    return kUnknownProcessConcurrency;
  }
  return info.max_daemons;
}

}  // namespace net_instaweb
