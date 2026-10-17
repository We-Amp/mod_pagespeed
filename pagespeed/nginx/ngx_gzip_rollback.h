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

//
// The bookkeeping half of NgxGZipSetter: which configuration slots the
// module's automatic gzip setup wrote, so an explicit gzip directive can
// roll them back, and whether the automatic setup is armed.
//
// The slots live in the configuration pool of one configuration cycle.
// The setter itself is process-global and outlives cycles (every reload
// parses a fresh configuration), so the record is reset at the start of
// each cycle: a slot from an earlier cycle is never written again.
//
// Deliberately nginx-free so it can be unit tested standalone
// (test/pagespeed/nginx). The slot types match nginx's ngx_flag_t
// (intptr_t) and ngx_uint_t (uintptr_t).

#ifndef NGX_GZIP_ROLLBACK_H_
#define NGX_GZIP_ROLLBACK_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace net_instaweb {

class NgxGZipRollbackLog {
 public:
  NgxGZipRollbackLog() : enabled_(false) {}

  // Starts a configuration cycle: forgets every slot recorded in earlier
  // cycles and arms the automatic setup when `gzip_available`.
  void BeginCycle(bool gzip_available);

  bool enabled() const { return enabled_; }

  void RecordFlag(intptr_t* slot) { flags_.push_back(slot); }
  void RecordUint(uintptr_t* slot) { uints_.push_back(slot); }
  void RecordTypes(void* slot) { types_.push_back(slot); }

  // Hands every slot recorded in this cycle to the matching callback,
  // forgets them, and disarms the automatic setup for the rest of the
  // cycle.
  void RollBack(const std::function<void(intptr_t*)>& reset_flag,
                const std::function<void(uintptr_t*)>& reset_uint,
                const std::function<void(void*)>& reset_types);

  size_t recorded() const {
    return flags_.size() + uints_.size() + types_.size();
  }

 private:
  std::vector<intptr_t*> flags_;
  std::vector<uintptr_t*> uints_;
  std::vector<void*> types_;
  bool enabled_;
};

}  // namespace net_instaweb

#endif  // NGX_GZIP_ROLLBACK_H_
