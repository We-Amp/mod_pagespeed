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

#include "pagespeed/system/memcached_cache.h"

#include <cstdlib>
#include <map>
#include <vector>

#include <libmemcached-1.0/memcached.h>

#include "base/logging.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/cache/key_value_codec.h"

namespace net_instaweb {

namespace {

const char kMemCacheTimeouts[] = "memcache_timeouts";
const char kLastErrorCheckpointMs[] = "memcache_last_error_checkpoint_ms";
const char kErrorBurstSize[] = "memcache_error_burst_size";
const int kTimeoutUnset = -1;

}  // namespace

MemcachedCache::MemcachedCache(const ExternalClusterSpec& cluster,
                               int thread_limit, Hasher* hasher,
                               Statistics* statistics, Timer* timer,
                               MessageHandler* handler)
    : cluster_spec_(cluster),
      valid_server_spec_(!cluster.empty()),
      timeout_us_(kTimeoutUnset),
      memc_(nullptr),
      hasher_(hasher),
      timer_(timer),
      timeouts_(statistics->GetVariable(kMemCacheTimeouts)),
      last_error_checkpoint_ms_(
          statistics->GetUpDownCounter(kLastErrorCheckpointMs)),
      error_burst_size_(statistics->GetUpDownCounter(kErrorBurstSize)),
      message_handler_(handler),
      last_error_(statistics->GetVariable(kMemCacheTimeouts)) {}

MemcachedCache::~MemcachedCache() {
  if (memc_ != nullptr) {
    memcached_free(memc_);
    memc_ = nullptr;
  }
}

void MemcachedCache::InitStats(Statistics* statistics) {
  statistics->AddVariable(kMemCacheTimeouts);
  statistics->AddUpDownCounter(kLastErrorCheckpointMs);
  statistics->AddUpDownCounter(kErrorBurstSize);
}

bool MemcachedCache::Connect() {
  if (memc_ != nullptr) {
    memcached_free(memc_);
    memc_ = nullptr;
  }

  memc_ = memcached_create(nullptr);
  if (memc_ == nullptr) {
    message_handler_->Message(kError, "MemcachedCache: Failed to create client");
    return false;
  }

  // Configure behaviors.
  memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, 1);
  memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_NO_BLOCK, 0);
  memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_TCP_NODELAY, 1);

  if (timeout_us_ != kTimeoutUnset) {
    // libmemcached uses milliseconds for timeouts.
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_SND_TIMEOUT,
                           timeout_us_ / 1000);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_RCV_TIMEOUT,
                           timeout_us_ / 1000);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_CONNECT_TIMEOUT,
                           timeout_us_ / 1000);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_POLL_TIMEOUT,
                           timeout_us_ / 1000);
  }

  bool success = true;
  for (const ExternalServerSpec& spec : cluster_spec_.servers) {
    memcached_return_t rc = memcached_server_add(
        memc_, spec.host.c_str(), static_cast<in_port_t>(spec.port));
    if (rc != MEMCACHED_SUCCESS) {
      message_handler_->Message(
          kError, "MemcachedCache: Failed to add server %s:%d: %s",
          spec.host.c_str(), spec.port, memcached_strerror(memc_, rc));
      success = false;
    }
  }
  return success;
}

void MemcachedCache::DecodeValueMatchingKeyAndCallCallback(
    const GoogleString& key, const char* data, size_t data_len,
    const char* calling_method, Callback* callback) {
  SharedString key_and_value;
  key_and_value.Assign(data, data_len);
  GoogleString actual_key;
  SharedString tmp_value;
  if (key_value_codec::Decode(&key_and_value, &actual_key, &tmp_value)) {
    callback->set_value(tmp_value);
    if (key == actual_key) {
      ValidateAndReportResult(actual_key, CacheInterface::kAvailable, callback);
    } else {
      message_handler_->Message(
          kError, "MemcachedCache::%s key collision %s != %s", calling_method,
          key.c_str(), actual_key.c_str());
      ValidateAndReportResult(key, CacheInterface::kNotFound, callback);
    }
  } else {
    message_handler_->Message(kError,
                              "MemcachedCache::%s decoding error on key %s",
                              calling_method, key.c_str());
    ValidateAndReportResult(key, CacheInterface::kNotFound, callback);
  }
}

void MemcachedCache::Get(const GoogleString& key, Callback* callback) {
  if (!IsHealthy()) {
    ValidateAndReportResult(key, CacheInterface::kNotFound, callback);
    return;
  }

  GoogleString hashed_key = hasher_->Hash(key);
  size_t value_length = 0;
  uint32_t flags = 0;
  memcached_return_t rc;
  char* value = memcached_get(memc_, hashed_key.data(), hashed_key.size(),
                              &value_length, &flags, &rc);
  if (rc == MEMCACHED_SUCCESS && value != nullptr) {
    DecodeValueMatchingKeyAndCallCallback(key, value, value_length, "Get",
                                          callback);
    free(value);
  } else {
    if (rc != MEMCACHED_NOTFOUND) {
      RecordError();
      message_handler_->Message(
          kError, "MemcachedCache::Get error: %s on key %s",
          memcached_strerror(memc_, rc), key.c_str());
      if (rc == MEMCACHED_TIMEOUT) {
        timeouts_->Add(1);
      }
    }
    ValidateAndReportResult(key, CacheInterface::kNotFound, callback);
    if (value != nullptr) {
      free(value);
    }
  }
}

void MemcachedCache::MultiGet(MultiGetRequest* request) {
  if (!IsHealthy()) {
    ReportMultiGetNotFound(request);
    return;
  }

  int n = request->size();
  std::vector<GoogleString> hashed_keys(n);
  std::vector<const char*> keys(n);
  std::vector<size_t> key_lengths(n);

  for (int i = 0; i < n; ++i) {
    hashed_keys[i] = hasher_->Hash((*request)[i].key);
    keys[i] = hashed_keys[i].data();
    key_lengths[i] = hashed_keys[i].size();
  }

  memcached_return_t rc =
      memcached_mget(memc_, keys.data(), key_lengths.data(), n);

  if (rc != MEMCACHED_SUCCESS) {
    RecordError();
    message_handler_->Message(
        kError, "MemcachedCache::MultiGet mget error: %s on %d keys",
        memcached_strerror(memc_, rc), n);
    ReportMultiGetNotFound(request);
    return;
  }

  // Build a map from hashed_key -> index for result matching.
  std::map<GoogleString, int> key_index_map;
  for (int i = 0; i < n; ++i) {
    key_index_map[hashed_keys[i]] = i;
  }

  // Track which keys got results.
  std::vector<bool> found(n, false);

  // Fetch results one by one.
  memcached_result_st result_obj;
  memcached_result_create(memc_, &result_obj);
  bool error_recorded = false;

  while (true) {
    memcached_result_st* result =
        memcached_fetch_result(memc_, &result_obj, &rc);
    if (result == nullptr) {
      break;
    }
    if (rc == MEMCACHED_SUCCESS) {
      GoogleString result_key(memcached_result_key_value(result),
                              memcached_result_key_length(result));
      auto it = key_index_map.find(result_key);
      if (it != key_index_map.end()) {
        int idx = it->second;
        found[idx] = true;
        const char* data = memcached_result_value(result);
        size_t data_len = memcached_result_length(result);
        DecodeValueMatchingKeyAndCallCallback(
            (*request)[idx].key, data, data_len, "MultiGet",
            (*request)[idx].callback);
      }
    } else if (rc != MEMCACHED_END) {
      if (!error_recorded) {
        error_recorded = true;
        RecordError();
      }
      if (rc == MEMCACHED_TIMEOUT) {
        timeouts_->Add(1);
      }
    }
  }
  memcached_result_free(&result_obj);

  // Report not-found for any keys we didn't get results for.
  for (int i = 0; i < n; ++i) {
    if (!found[i]) {
      ValidateAndReportResult((*request)[i].key, CacheInterface::kNotFound,
                              (*request)[i].callback);
    }
  }
  delete request;
}

void MemcachedCache::PutHelper(const GoogleString& key,
                               const SharedString& key_and_value) {
  GoogleString hashed_key = hasher_->Hash(key);
  memcached_return_t rc = memcached_set(
      memc_, hashed_key.data(), hashed_key.size(),
      key_and_value.data(), key_and_value.size(), 0, 0);
  if (rc != MEMCACHED_SUCCESS) {
    RecordError();
    int value_size =
        key_value_codec::GetValueSizeFromKeyAndKeyValue(key, key_and_value);
    message_handler_->Message(
        kError, "MemcachedCache::Put error: %s on key %s, value-size %d",
        memcached_strerror(memc_, rc), key.c_str(), value_size);
    if (rc == MEMCACHED_TIMEOUT) {
      timeouts_->Add(1);
    }
  }
}

void MemcachedCache::PutWithKeyInValue(const GoogleString& key,
                                       const SharedString& key_and_value) {
  if (!IsHealthy()) {
    return;
  }
  PutHelper(key, key_and_value);
}

void MemcachedCache::Put(const GoogleString& key, const SharedString& value) {
  if (!IsHealthy()) {
    return;
  }
  SharedString key_and_value;
  if (key_value_codec::Encode(key, value, &key_and_value)) {
    PutHelper(key, key_and_value);
  } else {
    message_handler_->Message(
        kError,
        "MemcachedCache::Put error: key size %d too large, first "
        "100 bytes of key is: %s",
        static_cast<int>(key.size()), key.substr(0, 100).c_str());
  }
}

void MemcachedCache::Delete(const GoogleString& key) {
  if (!IsHealthy()) {
    return;
  }
  GoogleString hashed_key = hasher_->Hash(key);
  memcached_return_t rc =
      memcached_delete(memc_, hashed_key.data(), hashed_key.size(), 0);
  if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_NOTFOUND) {
    RecordError();
    message_handler_->Message(
        kError, "MemcachedCache::Delete error: %s on key %s",
        memcached_strerror(memc_, rc), key.c_str());
    if (rc == MEMCACHED_TIMEOUT) {
      timeouts_->Add(1);
    }
  }
}

bool MemcachedCache::GetStatus(GoogleString* buffer) {
  if (memc_ == nullptr) {
    return false;
  }
  memcached_stat_st* stats = memcached_stat(memc_, nullptr, nullptr);
  if (stats == nullptr) {
    return false;
  }
  uint32_t server_count = memcached_server_count(memc_);
  for (uint32_t i = 0; i < server_count; ++i) {
    const memcached_instance_st* instance =
        memcached_server_instance_by_position(memc_, i);
    StrAppend(buffer, "memcached server ",
              memcached_server_name(instance), ":",
              IntegerToString(memcached_server_port(instance)));
    StrAppend(buffer, " version ", stats[i].version);
    StrAppend(buffer, " pid ", IntegerToString(stats[i].pid), " up ",
              IntegerToString(stats[i].uptime), " seconds\n");
    StrAppend(buffer, "bytes:                 ",
              Integer64ToString(stats[i].bytes), "\n");
    StrAppend(buffer, "bytes_read:            ",
              Integer64ToString(stats[i].bytes_read), "\n");
    StrAppend(buffer, "bytes_written:         ",
              Integer64ToString(stats[i].bytes_written), "\n");
    StrAppend(buffer, "cmd_get:               ",
              Integer64ToString(stats[i].cmd_get), "\n");
    StrAppend(buffer, "cmd_set:               ",
              Integer64ToString(stats[i].cmd_set), "\n");
    StrAppend(buffer, "curr_connections:      ",
              IntegerToString(stats[i].curr_connections), "\n");
    StrAppend(buffer, "curr_items:            ",
              IntegerToString(stats[i].curr_items), "\n");
    StrAppend(buffer, "evictions:             ",
              Integer64ToString(stats[i].evictions), "\n");
    StrAppend(buffer, "get_hits:              ",
              Integer64ToString(stats[i].get_hits), "\n");
    StrAppend(buffer, "get_misses:            ",
              Integer64ToString(stats[i].get_misses), "\n");
    StrAppend(buffer, "limit_maxbytes:        ",
              Integer64ToString(stats[i].limit_maxbytes), "\n");
    StrAppend(buffer, "threads:               ",
              IntegerToString(stats[i].threads), "\n");
    StrAppend(buffer, "total_connections:     ",
              IntegerToString(stats[i].total_connections), "\n");
    StrAppend(buffer, "total_items:           ",
              IntegerToString(stats[i].total_items), "\n\n");
  }
  memcached_stat_free(memc_, stats);
  return true;
}

void MemcachedCache::RecordError() {
  int64 time_ms = timer_->NowMs();
  int64 last_error_checkpoint_ms = last_error_checkpoint_ms_->Get();
  int64 delta_ms = time_ms - last_error_checkpoint_ms;

  if (delta_ms > kHealthCheckpointIntervalMs) {
    last_error_checkpoint_ms_->Set(time_ms);
    error_burst_size_->Set(1);
  } else {
    error_burst_size_->Add(1);
  }
}

bool MemcachedCache::IsHealthy() const {
  if (shutdown_.value()) {
    return false;
  }
  int64 time_ms = timer_->NowMs();
  int64 last_error_checkpoint_ms = last_error_checkpoint_ms_->Get();
  int64 delta_ms = time_ms - last_error_checkpoint_ms;
  int64 error_burst_size = error_burst_size_->Get();

  if (delta_ms > kHealthCheckpointIntervalMs) {
    if (error_burst_size >= kMaxErrorBurst) {
      message_handler_->Message(
          kInfo, "MemcachedCache::IsHealthy: Attempting to recover");
    }
    error_burst_size_->Set(0);
    return true;
  }
  return error_burst_size < kMaxErrorBurst;
}

void MemcachedCache::ShutDown() { shutdown_.set_value(true); }

void MemcachedCache::set_timeout_us(int timeout_us) {
  timeout_us_ = timeout_us;
  if (memc_ != nullptr && timeout_us != kTimeoutUnset) {
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_SND_TIMEOUT,
                           timeout_us / 1000);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_RCV_TIMEOUT,
                           timeout_us / 1000);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_CONNECT_TIMEOUT,
                           timeout_us / 1000);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_POLL_TIMEOUT,
                           timeout_us / 1000);
  }
}

}  // namespace net_instaweb
