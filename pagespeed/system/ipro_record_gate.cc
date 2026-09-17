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

#include "pagespeed/system/ipro_record_gate.h"

#include "pagespeed/system/in_place_resource_recorder.h"

namespace net_instaweb {

IproDisposition IproDispositionFor(DaemonHealth health) {
  switch (health) {
    case DaemonHealth::kNotConfigured:
      return IproDisposition::kClassic;
    case DaemonHealth::kReady:
      return IproDisposition::kDaemonSubstrate;
    case DaemonHealth::kUnavailable:
      return IproDisposition::kOff;
  }
  // Unreachable for a well-formed enum, and the safe answer if the enum ever
  // grows: never silently resume classic recording behind an unknown state.
  return IproDisposition::kOff;
}

InPlaceResourceRecorder* MakeIproRecorderIfClassic(
    IproDisposition disposition, const RequestContextPtr& request_context,
    StringPiece url, StringPiece fragment,
    const RequestHeaders::Properties& request_properties,
    int max_response_bytes, int max_concurrent_recordings, HTTPCache* cache,
    Statistics* statistics, MessageHandler* handler) {
  if (disposition != IproDisposition::kClassic) {
    return nullptr;
  }
  return new InPlaceResourceRecorder(
      request_context, url, fragment, request_properties, max_response_bytes,
      max_concurrent_recordings, cache, statistics, handler);
}

}  // namespace net_instaweb
