/*
 * Copyright (c) 2026 We-Amp B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Test-only CacheInterface wrapper that turns every metadata hit into the
// record an older version would have stored: CachedResult field 32 (set by
// versions that build optimizations only from inputs the module fetched
// itself) is cleared from every partition.  Looked up by number, so the
// wrapper changes nothing where the field does not exist.

#ifndef TEST_NET_INSTAWEB_REWRITER_OLDER_RESULT_CACHE_H_
#define TEST_NET_INSTAWEB_REWRITER_OLDER_RESULT_CACHE_H_

#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cache_interface.h"

namespace net_instaweb {

class OlderResultCache : public CacheInterface {
 public:
  explicit OlderResultCache(CacheInterface* delegate) : delegate_(delegate) {}

  void Get(const GoogleString& key, Callback* callback) override {
    delegate_->Get(key, new AgingCallback(callback, &num_aged_));
  }
  void Put(const GoogleString& key, const SharedString& value) override {
    delegate_->Put(key, value);
  }
  void Delete(const GoogleString& key) override { delegate_->Delete(key); }
  GoogleString Name() const override {
    return StrCat("OlderResult(", delegate_->Name(), ")");
  }
  bool IsBlocking() const override { return delegate_->IsBlocking(); }
  bool IsHealthy() const override { return delegate_->IsHealthy(); }
  void ShutDown() override { delegate_->ShutDown(); }

  // Hits that carried the field and had it cleared.
  int num_aged() const { return num_aged_; }

 private:
  static constexpr int kFieldNumber = 32;

  class AgingCallback : public CacheInterface::Callback {
   public:
    AgingCallback(CacheInterface::Callback* callback, int* num_aged)
        : callback_(callback), num_aged_(num_aged) {}

    bool ValidateCandidate(const GoogleString& key,
                           CacheInterface::KeyState state) override {
      OutputPartitions partitions;
      if (state == CacheInterface::kAvailable &&
          partitions.ParseFromString(value().Value().as_string())) {
        bool aged = false;
        for (int i = 0; i < partitions.partition_size(); ++i) {
          CachedResult* result = partitions.mutable_partition(i);
          const google::protobuf::FieldDescriptor* field =
              result->GetDescriptor()->FindFieldByNumber(kFieldNumber);
          const google::protobuf::Reflection* reflection =
              result->GetReflection();
          if (field != nullptr && reflection->HasField(*result, field)) {
            reflection->ClearField(result, field);
            aged = true;
          }
        }
        if (aged) {
          GoogleString older;
          (void)partitions.SerializeToString(&older);
          callback_->set_value(MappedSharedString(SharedString(older)));
          ++*num_aged_;
        }
      }
      return callback_->DelegatedValidateCandidate(key, state);
    }

    void Done(CacheInterface::KeyState state) override {
      callback_->DelegatedDone(state);
      delete this;
    }

   private:
    CacheInterface::Callback* callback_;
    int* num_aged_;

    AgingCallback(const AgingCallback&) = delete;
    AgingCallback& operator=(const AgingCallback&) = delete;
  };

  CacheInterface* delegate_;  // Not owned.
  int num_aged_ = 0;

  OlderResultCache(const OlderResultCache&) = delete;
  OlderResultCache& operator=(const OlderResultCache&) = delete;
};

}  // namespace net_instaweb

#endif  // TEST_NET_INSTAWEB_REWRITER_OLDER_RESULT_CACHE_H_
