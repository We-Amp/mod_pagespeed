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

#ifndef PAGESPEED_SYSTEM_IPRO_RECORD_GATE_H_
#define PAGESPEED_SYSTEM_IPRO_RECORD_GATE_H_

#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/system/daemon_health.h"

namespace net_instaweb {

class HTTPCache;
class InPlaceResourceRecorder;
class MessageHandler;
class Statistics;

// What a serving seam should do with an in-place request.
enum class IproDisposition {
  // Record and serve exactly as this module always has.
  kClassic,

  // The optimizer daemon owns the in-place cache for this server.  The
  // classic recorder is NOT constructed: the daemon's own record path, over
  // the shared volume, is the writer, and a second writer producing entries
  // in a different keyspace would be duplicated work nothing reads.
  kDaemonSubstrate,

  // A daemon was configured and is not usable.  In-place optimization is off.
  // The classic recorder is NOT constructed here either -- reactivating it
  // would fill a cache no serving path consults, and would hide from the
  // operator that their daemon is missing.
  kOff,
};

IproDisposition IproDispositionFor(DaemonHealth health);

// The ONE place in this tree from which a serving seam may construct the
// classic in-place recorder.
//
// Returns nullptr for every disposition other than kClassic, WITHOUT
// constructing a recorder first and discarding it -- non-instantiation is the
// contract, not non-use, and InPlaceResourceRecorder::num_constructed() is
// what pins the difference.  Callers own the returned recorder in the same
// way they did when they built it themselves.
InPlaceResourceRecorder* MakeIproRecorderIfClassic(
    IproDisposition disposition, const RequestContextPtr& request_context,
    StringPiece url, StringPiece fragment,
    const RequestHeaders::Properties& request_properties,
    int max_response_bytes, int max_concurrent_recordings, HTTPCache* cache,
    Statistics* statistics, MessageHandler* handler);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_IPRO_RECORD_GATE_H_
