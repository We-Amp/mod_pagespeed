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

#ifndef PAGESPEED_SYSTEM_DAEMON_HEALTH_H_
#define PAGESPEED_SYSTEM_DAEMON_HEALTH_H_

namespace net_instaweb {

// How this server relates to the optimizer daemon.  Resolved once, at
// startup, and read (never recomputed, never logged from) on the serving
// path.
//
// This lives in a header of its own, with no includes, on purpose: the
// serving seams need the verdict and nothing else.  Routing them through the
// adapter's header instead would put the adapter -- and its run-time library
// binding, and the link flag that needs -- into every port that has a serving
// seam, including the ones whose substrate change has not been written yet.
enum class DaemonHealth {
  // Neither daemon path is configured.  Nothing about serving changes; the
  // classic in-place path stays exactly as it shipped.
  kNotConfigured,

  // A daemon was configured and this server cannot safely use it.  In-place
  // optimization is OFF for the duration of the process, announced once,
  // loudly.  It does NOT fall back to the classic recorder: recording into a
  // cache that nothing serves from is invisible work, and an operator who
  // configured a daemon has to be told the daemon is unusable rather than
  // shown a server that looks healthy.
  kUnavailable,

  // The daemon is configured, its library binds, it published the size of its
  // cache volume, and opening that volume at that size landed on the file the
  // daemon is already using rather than creating a second one.
  kReady,
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HEALTH_H_
