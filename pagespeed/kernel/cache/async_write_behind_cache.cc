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

#include "pagespeed/kernel/cache/async_write_behind_cache.h"

#include <cstddef>

#include "base/logging.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/cache/key_value_codec.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"

namespace net_instaweb {

const size_t AsyncWriteBehindCache::kUnlimited = static_cast<size_t>(-1);

AsyncWriteBehindCache::AsyncWriteBehindCache(CacheInterface* cache,
                                             QueuedWorkerPool* pool,
                                             size_t sync_put_size_threshold)
    : cache_(cache), sync_put_size_threshold_(sync_put_size_threshold) {
  CHECK(cache->IsBlocking());
  sequence_ = pool->NewSequence();
  sequence_->set_max_queue_size(kMaxQueueSize);
}

AsyncWriteBehindCache::~AsyncWriteBehindCache() {
  DCHECK_EQ(0, outstanding_operations());
}

GoogleString AsyncWriteBehindCache::FormatName(StringPiece cache) {
  return StrCat("AsyncWriteBehind(", cache, ")");
}

void AsyncWriteBehindCache::Get(const GoogleString& key, Callback* callback) {
  // Reads stay on the caller's thread and hit the blocking backend directly,
  // preserving the lock-free / zero-copy read path.
  cache_->Get(key, callback);
}

void AsyncWriteBehindCache::MultiGet(MultiGetRequest* request) {
  cache_->MultiGet(request);
}

void AsyncWriteBehindCache::PutEncoded(const GoogleString& key,
                                       const SharedString& value) {
  if (cache_->MustEncodeKeyInValueOnPut()) {
    cache_->PutWithKeyInValue(key, value);
  } else {
    cache_->Put(key, value);
  }
}

void AsyncWriteBehindCache::Put(const GoogleString& key,
                                const SharedString& value) {
  if (!IsHealthy()) {
    return;
  }

  // If the backend encodes the key into the value on Put, do that encoding now,
  // on the initiating thread: SharedString::Append can mutate shared storage in
  // a thread-unsafe way, so it must not run on the worker sequence.  (For the
  // metadata L2 tier the backend does not require this, so this is a no-op.)
  SharedString value_to_put = value;
  if (cache_->MustEncodeKeyInValueOnPut()) {
    SharedString encoded_value;
    if (!key_value_codec::Encode(key, value, &encoded_value)) {
      return;
    }
    value_to_put = encoded_value;
  }

  // INV-4: an entry that skips the shm L1 lives only in this L2 tier, so a
  // same-machine reader (which checks L1 first, then this cache) would miss it
  // until the write lands.  Keep those writes synchronous.  The size test uses
  // the ORIGINAL value, mirroring the enclosing WriteThroughCache's L1 gate.
  if (SkipsL1(key, value)) {
    PutEncoded(key, value_to_put);
    return;
  }

  // Defer the write.  Capturing 'value_to_put' by value bumps the underlying
  // refcount (INV-3), and the key is heap-copied, so the task is memory-safe.
  outstanding_operations_.NoBarrierIncrement(1);
  sequence_->Add(MakeFunction(this, &AsyncWriteBehindCache::DoPut,
                              &AsyncWriteBehindCache::CancelPut,
                              new GoogleString(key), value_to_put));
}

void AsyncWriteBehindCache::DoPut(
    GoogleString* key,
    const SharedString value) {  // NOLINT(performance-unnecessary-value-param)
  if (IsHealthy()) {
    PutEncoded(*key, value);
  }
  delete key;
  outstanding_operations_.BarrierIncrement(-1);
}

void AsyncWriteBehindCache::CancelPut(
    GoogleString* key,
    const SharedString value) {  // NOLINT(performance-unnecessary-value-param)
  delete key;
  outstanding_operations_.BarrierIncrement(-1);
}

void AsyncWriteBehindCache::Delete(const GoogleString& key) {
  if (!IsHealthy()) {
    return;
  }
  // Deletes go through the SAME sequence as deferred Puts, so a Put and a later
  // Delete of the same key retire in FIFO order and can never reorder (INV-2).
  outstanding_operations_.NoBarrierIncrement(1);
  sequence_->Add(MakeFunction(this, &AsyncWriteBehindCache::DoDelete,
                              &AsyncWriteBehindCache::CancelDelete,
                              new GoogleString(key)));
}

void AsyncWriteBehindCache::DoDelete(GoogleString* key) {
  if (IsHealthy()) {
    cache_->Delete(*key);
  }
  delete key;
  outstanding_operations_.BarrierIncrement(-1);
}

void AsyncWriteBehindCache::CancelDelete(GoogleString* key) {
  delete key;
  outstanding_operations_.BarrierIncrement(-1);
}

void AsyncWriteBehindCache::ShutDown() {
  stopped_.set_value(true);

  // Drop any queued-but-not-started writes; they are re-derivable metadata.
  sequence_->CancelPendingFunctions();

  // Any write currently executing on the sequence may still be in the backend.
  // Queue the backend ShutDown behind it so the ShutDown runs only after the
  // in-flight write has drained.  Full quiescence (so the backend outlives the
  // last task) is enforced by the owner shutting the worker pool down and
  // waiting for it to complete before destroying the backend.
  sequence_->Add(MakeFunction(cache_, &CacheInterface::ShutDown));
}

}  // namespace net_instaweb
