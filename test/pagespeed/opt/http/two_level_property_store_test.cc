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

#include "pagespeed/opt/http/two_level_property_store.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "base/logging.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/delay_cache.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/opt/http/abstract_property_store_get_callback.h"
#include "pagespeed/opt/http/cache_property_store.h"
#include "pagespeed/opt/http/property_cache.pb.h"
#include "pagespeed/opt/http/property_store.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/opt/http/mock_property_page.h"

namespace net_instaweb {

namespace {

const size_t kMaxCacheSize = 100;
const char kCache1[] = "cache1";
const char kCache2[] = "cache2";
const char kCohortName1[] = "cohort1";
const char kCohortName2[] = "cohort2";
const char kPropName1[] = "prop1";
const char kValueName1[] = "value1";
const char kUrl[] = "www.test.com/sample.html";
const char kOptionsSignatureHash[] = "hash";
const char kCacheKeySuffix[] = "CacheKeySuffix";

}  // namespace

class TwoLevelPropertyStoreTest : public testing::Test {
 public:
  TwoLevelPropertyStoreTest()
      : lru_cache_1_(kMaxCacheSize),
        lru_cache_2_(kMaxCacheSize),
        thread_system_(Platform::CreateThreadSystem()),
        delay_cache_1_(&lru_cache_1_, thread_system_.get()),
        delay_cache_2_(&lru_cache_2_, thread_system_.get()),
        timer_(thread_system_->NewMutex(), MockTimer::kApr_5_2010_ms),
        stats_(thread_system_.get()),
        cache_property_store_1_(kCache1, &delay_cache_1_, &timer_, &stats_,
                                thread_system_.get()),
        cache_property_store_2_(kCache2, &delay_cache_2_, &timer_, &stats_,
                                thread_system_.get()),
        two_level_property_store_(&cache_property_store_1_,
                                  &cache_property_store_2_,
                                  thread_system_.get()),
        num_callback_with_false_called_(0),
        num_callback_with_true_called_(0),
        property_cache_(&two_level_property_store_, &timer_, &stats_,
                        thread_system_.get()) {
    PropertyCache::InitCohortStats(kCohortName1, &stats_);
    PropertyStoreGetCallback::InitStats(&stats_);
    cohort_ = property_cache_.AddCohort(kCohortName1);
    cache_property_store_1_.AddCohort(kCohortName1);
    cache_property_store_2_.AddCohort(kCohortName1);
    cohort_list_.push_back(cohort_);
  }

  void SetUp() override {
    page_ = std::make_unique<MockPropertyPage>(

        thread_system_.get(), &property_cache_, kUrl, kOptionsSignatureHash,
        kCacheKeySuffix);
    property_cache_.Read(page_.get());
    lru_cache_1_.ClearStats();
    lru_cache_2_.ClearStats();
  }

  void PutHelper(PropertyStore* property_store,
                 const PropertyCache::Cohort* cohort) {
    PropertyCacheValues values;
    PropertyValueProtobuf* value_buf = values.add_value();
    value_buf->set_name(kPropName1);
    value_buf->set_body(kValueName1);
    property_store->Put(kUrl, kOptionsSignatureHash, kCacheKeySuffix, cohort,
                        &values, nullptr);
  }

  void ResultCallback(bool result) {
    if (result) {
      ++num_callback_with_true_called_;
    } else {
      ++num_callback_with_false_called_;
    }
  }

  AbstractPropertyStoreGetCallback* ExecuteGet(PropertyPage* page) {
    AbstractPropertyStoreGetCallback* callback = nullptr;
    two_level_property_store_.Get(
        kUrl, kOptionsSignatureHash, kCacheKeySuffix, cohort_list_, page,
        NewCallback(this, &TwoLevelPropertyStoreTest::ResultCallback),
        &callback);
    callback->DeleteWhenDone();
    return callback;
  }

  void ExpectCacheStats(LRUCache* lru_cache, int expected_num_cache_hits,
                        int expected_num_cache_misses,
                        int expected_num_cache_inserts,
                        const GoogleString& cache_string) {
    EXPECT_EQ(expected_num_cache_hits, lru_cache->num_hits()) << cache_string;
    EXPECT_EQ(expected_num_cache_misses, lru_cache->num_misses())
        << cache_string;
    EXPECT_EQ(expected_num_cache_inserts, lru_cache->num_inserts())
        << cache_string;
  }

  void DelayCacheLookup(DelayCache* cache, CachePropertyStore* property_store) {
    GoogleString cache_key = property_store->CacheKey(
        kUrl, kOptionsSignatureHash, kCacheKeySuffix, cohort_);
    LOG(INFO) << "Delay Cache Key:: " << cache_key;
    cache->DelayKey(cache_key);
  }

  void ReleaseCacheLookup(DelayCache* cache,
                          CachePropertyStore* property_store) {
    GoogleString cache_key = property_store->CacheKey(
        kUrl, kOptionsSignatureHash, kCacheKeySuffix, cohort_);
    cache->ReleaseKey(cache_key);
  }

 protected:
  LRUCache lru_cache_1_;
  LRUCache lru_cache_2_;
  std::unique_ptr<ThreadSystem> thread_system_;
  DelayCache delay_cache_1_;
  DelayCache delay_cache_2_;
  MockTimer timer_;
  SimpleStats stats_;
  CachePropertyStore cache_property_store_1_;
  CachePropertyStore cache_property_store_2_;
  TwoLevelPropertyStore two_level_property_store_;
  int num_callback_with_false_called_;
  int num_callback_with_true_called_;

  PropertyCache property_cache_;
  const PropertyCache::Cohort* cohort_;
  PropertyCache::CohortVector cohort_list_;
  std::unique_ptr<MockPropertyPage> page_;

 private:
  TwoLevelPropertyStoreTest(const TwoLevelPropertyStoreTest&) = delete;
  TwoLevelPropertyStoreTest& operator=(const TwoLevelPropertyStoreTest&) = delete;
};

TEST_F(TwoLevelPropertyStoreTest, TestBothCacheMiss) {
  ExecuteGet(page_.get());
  EXPECT_EQ(CacheInterface::kNotFound, page_->GetCacheState(cohort_));
  EXPECT_EQ(1, num_callback_with_false_called_);
  EXPECT_EQ(0, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   0 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   0 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestPrimaryLevelCacheHit) {
  PutHelper(&cache_property_store_1_, cohort_);
  ExecuteGet(page_.get());
  EXPECT_EQ(CacheInterface::kAvailable, page_->GetCacheState(cohort_));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(1, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 0, /* Cache hit */
                   0,                /* Cache miss */
                   0 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestSecondaryLevelCacheHit) {
  PutHelper(&cache_property_store_2_, cohort_);
  ExecuteGet(page_.get());
  EXPECT_EQ(CacheInterface::kAvailable, page_->GetCacheState(cohort_));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(1, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   1 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestOnlyPrimaryHitWhenPresentInBoth) {
  PutHelper(&two_level_property_store_, cohort_);
  ExecuteGet(page_.get());
  EXPECT_EQ(CacheInterface::kAvailable, page_->GetCacheState(cohort_));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(1, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 0, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestCancelBeforePrimaryLookupDone) {
  PutHelper(&cache_property_store_2_, cohort_);
  DelayCacheLookup(&delay_cache_1_, &cache_property_store_1_);
  AbstractPropertyStoreGetCallback* callback = ExecuteGet(page_.get());
  callback->FastFinishLookup();
  ReleaseCacheLookup(&delay_cache_1_, &cache_property_store_1_);
  EXPECT_EQ(CacheInterface::kNotFound, page_->GetCacheState(cohort_));
  EXPECT_EQ(1, num_callback_with_false_called_);
  EXPECT_EQ(0, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   0 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 0, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestCancelBeforeSecondaryLookupDone) {
  PutHelper(&cache_property_store_2_, cohort_);
  DelayCacheLookup(&delay_cache_2_, &cache_property_store_2_);
  AbstractPropertyStoreGetCallback* callback = ExecuteGet(page_.get());
  callback->FastFinishLookup();
  ReleaseCacheLookup(&delay_cache_2_, &cache_property_store_2_);
  EXPECT_EQ(CacheInterface::kNotFound, page_->GetCacheState(cohort_));
  EXPECT_EQ(1, num_callback_with_false_called_);
  EXPECT_EQ(0, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   0 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestCancelAfterSecondaryLookupDone) {
  PutHelper(&cache_property_store_2_, cohort_);
  AbstractPropertyStoreGetCallback* callback = nullptr;
  two_level_property_store_.Get(
      kUrl, kOptionsSignatureHash, kCacheKeySuffix, cohort_list_, page_.get(),
      NewCallback(this, &TwoLevelPropertyStoreTest::ResultCallback), &callback);
  callback->FastFinishLookup();
  callback->DeleteWhenDone();
  EXPECT_EQ(CacheInterface::kAvailable, page_->GetCacheState(cohort_));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(1, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   1 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestDeleteWhenDoneBeforeSecondaryLookupDone) {
  PutHelper(&cache_property_store_2_, cohort_);
  DelayCacheLookup(&delay_cache_2_, &cache_property_store_2_);
  AbstractPropertyStoreGetCallback* callback = nullptr;
  two_level_property_store_.Get(
      kUrl, kOptionsSignatureHash, kCacheKeySuffix, cohort_list_, page_.get(),
      NewCallback(this, &TwoLevelPropertyStoreTest::ResultCallback), &callback);
  callback->FastFinishLookup();
  callback->DeleteWhenDone();
  ReleaseCacheLookup(&delay_cache_2_, &cache_property_store_2_);
  EXPECT_EQ(CacheInterface::kNotFound, page_->GetCacheState(cohort_));
  EXPECT_EQ(1, num_callback_with_false_called_);
  EXPECT_EQ(0, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   0 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestPartialSecondaryLookup) {
  PropertyCache::InitCohortStats(kCohortName2, &stats_);
  const PropertyCache::Cohort* cohort2 =
      property_cache_.AddCohort(kCohortName2);
  cache_property_store_1_.AddCohort(kCohortName2);
  cache_property_store_2_.AddCohort(kCohortName2);
  MockPropertyPage page(thread_system_.get(), &property_cache_, kUrl,
                        kOptionsSignatureHash, kCacheKeySuffix);
  property_cache_.Read(&page);
  cohort_list_.push_back(cohort2);
  lru_cache_1_.ClearStats();
  lru_cache_2_.ClearStats();
  PutHelper(&two_level_property_store_, cohort_);
  PutHelper(&cache_property_store_2_, cohort2);
  ExecuteGet(&page);
  EXPECT_EQ(CacheInterface::kAvailable, page.GetCacheState(cohort_));
  EXPECT_EQ(CacheInterface::kAvailable, page.GetCacheState(cohort2));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(1, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 1, /* Cache hit */
                   1,                /* Cache miss */
                   2 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   2 /* Cache inserts */, kCache2);
}

TEST_F(TwoLevelPropertyStoreTest, TestInsertValueIntoPrimaryFromSecondary) {
  PutHelper(&cache_property_store_2_, cohort_);
  ExecuteGet(page_.get());
  EXPECT_EQ(CacheInterface::kAvailable, page_->GetCacheState(cohort_));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(1, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 0, /* Cache hit */
                   1,                /* Cache miss */
                   1 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   1 /* Cache inserts */, kCache2);

  PropertyValue* pv = page_->GetProperty(cohort_, kPropName1);
  EXPECT_TRUE(pv->has_value());
  EXPECT_EQ(kValueName1, pv->value());

  lru_cache_1_.ClearStats();
  lru_cache_2_.ClearStats();
  ExecuteGet(page_.get());
  EXPECT_EQ(CacheInterface::kAvailable, page_->GetCacheState(cohort_));
  EXPECT_EQ(0, num_callback_with_false_called_);
  EXPECT_EQ(2, num_callback_with_true_called_);
  ExpectCacheStats(&lru_cache_1_, 1, /* Cache hit */
                   0,                /* Cache miss */
                   0 /* Cache inserts */, kCache1);
  ExpectCacheStats(&lru_cache_2_, 0, /* Cache hit */
                   0,                /* Cache miss */
                   0 /* Cache inserts */, kCache2);
  pv = page_->GetProperty(cohort_, kPropName1);
  EXPECT_TRUE(pv->has_value());
  EXPECT_EQ(kValueName1, pv->value());
}

namespace {

// A store whose Put completion is controlled by the test: either inline with a
// fixed result, or held until Release().
class FakePutStore : public PropertyStore {
 public:
  FakePutStore(bool inline_result, bool hold)
      : inline_result_(inline_result), hold_(hold), num_puts_(0) {}

  void Get(const GoogleString&, const GoogleString&, const GoogleString&,
           const PropertyCache::CohortVector&, PropertyPage*,
           BoolCallback* done,
           AbstractPropertyStoreGetCallback** callback) override {
    *callback = nullptr;
    done->Run(false);
  }

  void Put(const GoogleString&, const GoogleString&, const GoogleString&,
           const PropertyCache::Cohort*, const PropertyCacheValues*,
           BoolCallback* done) override {
    ++num_puts_;
    if (done == nullptr) {
      return;
    }
    if (hold_) {
      held_.push_back(done);
    } else {
      done->Run(inline_result_);
    }
  }

  void Release(bool result) {
    for (BoolCallback* done : held_) {
      done->Run(result);
    }
    held_.clear();
  }

  GoogleString Name() const override { return "fake"; }
  int num_puts() const { return num_puts_; }

 private:
  bool inline_result_;
  bool hold_;
  int num_puts_;
  std::vector<BoolCallback*> held_;
};

}  // namespace

class TwoLevelPropertyStorePutTest : public testing::Test {
 protected:
  TwoLevelPropertyStorePutTest()
      : thread_system_(Platform::CreateThreadSystem()),
        true_calls_(0),
        false_calls_(0) {}

  void Done(bool result) { ++(result ? true_calls_ : false_calls_); }

  void DoPut(PropertyStore* store) {
    PropertyCacheValues values;
    store->Put(kUrl, kOptionsSignatureHash, kCacheKeySuffix, nullptr, &values,
               NewCallback(this, &TwoLevelPropertyStorePutTest::Done));
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  int true_calls_;
  int false_calls_;
};

TEST_F(TwoLevelPropertyStorePutTest, BothLevelsSucceed) {
  FakePutStore a(true, false), b(true, false);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(1, a.num_puts());
  EXPECT_EQ(1, b.num_puts());
  EXPECT_EQ(1, true_calls_);
  EXPECT_EQ(0, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, PrimaryFails) {
  FakePutStore a(false, false), b(true, false);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(0, true_calls_);
  EXPECT_EQ(1, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, SecondaryFails) {
  FakePutStore a(true, false), b(false, false);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(0, true_calls_);
  EXPECT_EQ(1, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, WaitsForSlowLevelAndRunsOnce) {
  FakePutStore a(true, false), b(true, true);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(0, true_calls_);
  EXPECT_EQ(0, false_calls_);
  b.Release(true);
  EXPECT_EQ(1, true_calls_);
  EXPECT_EQ(0, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, SlowLevelFailureReported) {
  FakePutStore a(true, true), b(true, true);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(0, true_calls_ + false_calls_);
  a.Release(true);
  EXPECT_EQ(0, true_calls_ + false_calls_);
  b.Release(false);
  EXPECT_EQ(0, true_calls_);
  EXPECT_EQ(1, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, PrimaryHeldWhileSecondaryCompletesInline) {
  FakePutStore a(true, true), b(true, false);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(0, true_calls_ + false_calls_);
  a.Release(true);
  EXPECT_EQ(1, true_calls_);
  EXPECT_EQ(0, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, SlowPrimaryFailsAfterSecondarySucceeds) {
  FakePutStore a(true, true), b(true, false);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  DoPut(&store);
  EXPECT_EQ(0, true_calls_ + false_calls_);
  a.Release(false);
  EXPECT_EQ(0, true_calls_);
  EXPECT_EQ(1, false_calls_);
}

TEST_F(TwoLevelPropertyStorePutTest, NullDoneStillWritesBothLevels) {
  FakePutStore a(true, false), b(true, false);
  TwoLevelPropertyStore store(&a, &b, thread_system_.get());
  PropertyCacheValues values;
  store.Put(kUrl, kOptionsSignatureHash, kCacheKeySuffix, nullptr, &values,
            nullptr);
  EXPECT_EQ(1, a.num_puts());
  EXPECT_EQ(1, b.num_puts());
}

}  // namespace net_instaweb
