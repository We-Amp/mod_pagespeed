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

#ifndef IIS_PROXY_FETCH_COMPLETION_H_
#define IIS_PROXY_FETCH_COMPLETION_H_

namespace net_instaweb {

// Finishes the ProxyFetch for a request whose final response chunk has been
// seen (the "leave" path in OnSendResponse), in a way that is safe against the
// re-entrant completion hazard that caused an intermittent w3wp use-after-free
// crash under AppVerifier/Page-Heap.
//
// THE HAZARD: ProxyFetch::Done() can run the PSOL rewrite to completion
// SYNCHRONOUSLY. When it does, it calls back into
// IisModuleBaseFetch::HandleDone() -> IHttpContext::IndicateCompletion(), which
// re-drives the IIS pipeline (re-entering OnSendResponse) and ultimately tears
// the request down -> IisModuleRequestContext::CleanupStoredContext() ->
// `delete` of the IisInnerRequestContext. So `inner` may be a dangling pointer
// the instant Done() returns (or even during it).
//
// THE INVARIANT: capture the ProxyFetch and clear ALL per-context state on
// `inner` BEFORE calling Done(), then call Done() LAST and never touch `inner`
// again. Clearing proxy_fetch()/leave() first also makes any re-entrant
// OnSendResponse bail safely (it sees proxy_fetch()==null && leave()==true).
//
// Templated on the context/fetch types so the invariant is unit-testable on
// every platform with fakes (the real instantiation is Windows-only).
template <typename InnerContextT, typename ProxyFetchT>
void FinishProxyFetchAndLeave(InnerContextT* inner) {
  ProxyFetchT* proxy_fetch = inner->proxy_fetch();
  // Clear state BEFORE the (possibly self-freeing, possibly re-entrant) Done().
  inner->set_proxy_fetch(nullptr);
  inner->set_leave(true);
  if (proxy_fetch != nullptr) {
    // MUST be the last statement: this may synchronously `delete inner`.
    proxy_fetch->Done(true);
  }
}

}  // namespace net_instaweb

#endif  // IIS_PROXY_FETCH_COMPLETION_H_
