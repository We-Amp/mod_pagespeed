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

#include "pagespeed/system/daemon_adapter_pool.h"

namespace net_instaweb {

DaemonAdapterPool::Checked DaemonAdapterPool::GetOrCheck(
    StringPiece socket_path, StringPiece volume_path, MessageHandler* handler) {
  std::lock_guard<std::mutex> lock(mu_);
  std::pair<GoogleString, GoogleString> key(socket_path.as_string(),
                                            volume_path.as_string());
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    return it->second;
  }
  Checked checked;
  checked.adapter =
      std::make_shared<DaemonAdapter>(socket_path, volume_path, handler);
  checked.status = checked.adapter->StartupCheck();
  entries_.emplace(std::move(key), checked);
  return checked;
}

size_t DaemonAdapterPool::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.size();
}

}  // namespace net_instaweb
