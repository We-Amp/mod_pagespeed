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

#ifndef PAGESPEED_SYSTEM_MEMCACHED_CACHE_H_
#define PAGESPEED_SYSTEM_MEMCACHED_CACHE_H_

#include <cstddef>
#include <mutex>
#include <vector>

#include "pagespeed/kernel/base/atomic_bool.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/system/external_server_spec.h"

struct memcached_st;

namespace net_instaweb {

class Hasher;
class MessageHandler;
class Statistics;
class UpDownCounter;
class Variable;

// Memcached client using libmemcached. This is a blocking implementation
// suitable for wrapping with AsyncCache.
class MemcachedCache : public CacheInterface {
 public:
  static const size_t kValueSizeThreshold = 1 * 1000 * 1000;
  static const int64 kHealthCheckpointIntervalMs = 30 * Timer::kSecondMs;
  static const int64 kMaxErrorBurst = 4;

  MemcachedCache(const ExternalClusterSpec& cluster, int thread_limit,
                 Hasher* hasher, Statistics* statistics, Timer* timer,
                 MessageHandler* handler);
  ~MemcachedCache() override;

  static void InitStats(Statistics* statistics);

  const ExternalClusterSpec& cluster_spec() const { return cluster_spec_; }

  void Get(const GoogleString& key, Callback* callback) override;
  void Put(const GoogleString& key, const SharedString& value) override;
  void Delete(const GoogleString& key) override;
  void MultiGet(MultiGetRequest* request) override;

  bool Connect();

  bool valid_server_spec() const { return valid_server_spec_; }

  bool GetStatus(GoogleString* status_string);

  static GoogleString FormatName() { return "MemcachedCache"; }
  GoogleString Name() const override { return FormatName(); }

  bool IsBlocking() const override { return true; }

  void RecordError();
  bool IsHealthy() const override;
  void ShutDown() override;

  bool MustEncodeKeyInValueOnPut() const override { return true; }
  void PutWithKeyInValue(const GoogleString& key,
                         const SharedString& key_and_value) override;

  void set_timeout_us(int timeout_us);

 private:
  void DecodeValueMatchingKeyAndCallCallback(const GoogleString& key,
                                             const char* data, size_t data_len,
                                             const char* calling_method,
                                             Callback* callback);
  void PutHelper(const GoogleString& key, const SharedString& key_and_value);

  mutable std::mutex mutex_;
  ExternalClusterSpec cluster_spec_;
  bool valid_server_spec_;
  int timeout_us_;
  memcached_st* memc_;
  Hasher* hasher_;
  Timer* timer_;
  AtomicBool shutdown_;

  Variable* timeouts_;
  UpDownCounter* last_error_checkpoint_ms_;
  UpDownCounter* error_burst_size_;
  MessageHandler* message_handler_;
  Variable* last_error_;

  DISALLOW_COPY_AND_ASSIGN(MemcachedCache);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_MEMCACHED_CACHE_H_
