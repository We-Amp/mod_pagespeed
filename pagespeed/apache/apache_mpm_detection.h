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

// Apache MPM threading detection, split from ApacheRewriteDriverFactory so the
// decision can be unit-tested without an Apache runtime.
//
// Note on timing: the values below are only trustworthy once httpd has
// processed its configuration.  Asked earlier -- during the LoadModule read
// pass, which is when the factory is constructed -- a threaded MPM reports
// zero threads, and if the MPM module itself has not been loaded yet the query
// cannot be answered at all.  Callers must therefore run detection from
// post-config.

#ifndef PAGESPEED_APACHE_APACHE_MPM_DETECTION_H_
#define PAGESPEED_APACHE_APACHE_MPM_DETECTION_H_

namespace net_instaweb {

// What ap_mpm_query() told us about the running MPM.
struct MpmThreadInfo {
  // False if ap_mpm_query() could not answer, which happens when the MPM
  // module has not been loaded yet.
  bool query_ok;
  // AP_MPMQ_NOT_SUPPORTED / AP_MPMQ_STATIC / AP_MPMQ_DYNAMIC.
  int is_threaded;
  // Threads per child process, as configured.  Only meaningful once the
  // configuration has been processed; it reads as 0 before that.
  int max_threads;
};

// Asks the running MPM about its threading model.
MpmThreadInfo QueryMpmThreadInfo();

// True iff the MPM described by info runs more than one request thread per
// child process.  Pure; exposed for testing.
bool IsThreadedFromMpmInfo(const MpmThreadInfo& info);

// What ap_mpm_query() told us about how many child processes httpd will run.
struct MpmProcessInfo {
  // False if ap_mpm_query() could not answer.
  bool query_ok;
  // AP_MPMQ_MAX_DAEMONS: the configured child-process count.  This is exactly
  // the design record process-concurrency divisor on all three POSIX MPMs, without
  // any arithmetic of our own: prefork answers with MaxRequestWorkers, worker
  // and event answer with MaxRequestWorkers / ThreadsPerChild.  Reads as 0
  // before httpd's check_config phase has run.
  int max_daemons;
};

// The ap_mpm_query() key QueryMpmProcessInfo() asks with.  Exposed only so
// that the *choice* of key is pinned by a test instead of living as one
// identifier inside one call: which key supplies the design record divisor is the
// decision, not an implementation detail, and every plausible alternative
// compiles and returns a number.
//
//   AP_MPMQ_MAX_DAEMONS         the configured child count -- what we want.
//   AP_MPMQ_MAX_DAEMON_USED     the high-water mark of children actually
//                               started, which is 0 at post-config.
//   AP_MPMQ_HARD_LIMIT_DAEMONS  the compile-time ceiling, unrelated to the
//                               configuration.
//   AP_MPMQ_MAX_THREADS         threads per child, a different quantity
//                               entirely -- and dividing by it is exactly the
//                               oversubscription bug this policy replaces.
extern const int kMpmProcessConcurrencyQuery;

// Asks the running MPM how many children it is configured for.  Only
// trustworthy from post-config onwards: the MPMs compute this in their
// check_config hook, which httpd runs after the configuration tree and before
// post-config.
MpmProcessInfo QueryMpmProcessInfo();

// the design record process-concurrency divisor implied by info, or
// kUnknownProcessConcurrency (-1) when httpd could not tell us -- in which
// case the policy takes the one-thread floor rather than guessing.  Pure;
// exposed for testing.
int ProcessConcurrencyFromMpmInfo(const MpmProcessInfo& info);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_MPM_DETECTION_H_
