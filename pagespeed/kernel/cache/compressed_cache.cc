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

#include "pagespeed/kernel/cache/compressed_cache.h"

#include "base/logging.h"
////#include "strings/stringpiece_utils.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/util/gzip_inflater.h"

namespace net_instaweb {

const int64 CompressedCache::kDefaultMaxInflatedValueSize;

namespace {

// A few bytes to put at the end of the physical payload we can track
// corruption.  Note that CompressedCacheTest.CrapAtEnd fails without this.
const char kTrailer[] = "[[]]";

// TODO(jmarantz): Evaluate the impact of histogramming the size reduction of
// each entry.  The compressed_cache_speed_test.cc side-steps this because
// SimpleStats doesn't implement histograms.  See split_statistics_test.cc:93
// for mechanism to speed-test histograms.
#define INCLUDE_HISTOGRAMS 0

#if INCLUDE_HISTOGRAMS
const char kCompressedCacheSavings[] = "compressed_cache_savings";
#endif
const char kCompressedCacheOriginalSize[] = "compressed_cache_original_size";
const char kCompressedCacheCompressedSize[] =
    "compressed_cache_compressed_size";
const char kCompressedCacheCorruptPayloads[] =
    "compressed_cache_corrupt_payloads";

class CompressedCallback : public CacheInterface::Callback {
 public:
  CompressedCallback(CacheInterface::Callback* callback,
                     Variable* corrupt_payloads, int64 max_inflated_value_size)
      : callback_(callback),
        corrupt_payloads_(corrupt_payloads),
        max_inflated_value_size_(max_inflated_value_size),
        validate_candidate_called_(false) {}

  ~CompressedCallback() override {}

  bool ValidateCandidate(const GoogleString& key,
                         CacheInterface::KeyState state) override {
    validate_candidate_called_ = true;
    bool ret = false;
    if (state == CacheInterface::kAvailable) {
      GoogleString uncompressed;
      StringWriter writer(&uncompressed);
      StringPiece compressed = value().Value();
      // A payload can inflate a thousandfold, so stop at the largest
      // inflated value we are willing to produce and report anything past
      // it like any other corruption.
      const size_t max_inflated_bytes =
          (max_inflated_value_size_ < 0)
              ? static_cast<size_t>(-1)
              : static_cast<size_t>(max_inflated_value_size_);
      if (strings::EndsWith(compressed,
                            StringPiece(kTrailer, STATIC_STRLEN(kTrailer))) &&
          GzipInflater::InflateWithLimit(
              compressed.substr(0, compressed.size() - STATIC_STRLEN(kTrailer)),
              GzipInflater::kDeflate, max_inflated_bytes, &writer)) {
        SharedString uncompressed_shared;
        uncompressed_shared.SwapWithString(&uncompressed);
        callback_->set_value(uncompressed_shared);
        ret = true;
      } else {
        state = CacheInterface::kNotFound;
        corrupt_payloads_->Add(1);
      }
    }
    ret &= callback_->DelegatedValidateCandidate(key, state);
    return ret;
  }

  void Done(CacheInterface::KeyState state) override {
    DCHECK(validate_candidate_called_);
    callback_->DelegatedDone(state);
    delete this;
  }

  Callback* callback_;
  Variable* corrupt_payloads_;
  int64 max_inflated_value_size_;
  bool validate_candidate_called_;
};

}  // namespace

CompressedCache::CompressedCache(CacheInterface* cache, Statistics* stats)
    : CompressedCache(cache, stats, kDefaultMaxInflatedValueSize) {}

CompressedCache::CompressedCache(CacheInterface* cache, Statistics* stats,
                                 int64 max_inflated_value_size)
    : cache_(cache), max_inflated_value_size_(max_inflated_value_size) {
#if INCLUDE_HISTOGRAMS
  compressed_cache_savings_ = stats->GetHistogram(kCompressedCacheSavings);
#endif
  corrupt_payloads_ = stats->GetVariable(kCompressedCacheCorruptPayloads);
  original_size_ = stats->GetVariable(kCompressedCacheOriginalSize);
  compressed_size_ = stats->GetVariable(kCompressedCacheCompressedSize);
}

CompressedCache::~CompressedCache() {}

GoogleString CompressedCache::FormatName(StringPiece name) {
  return StrCat("Compressed(", name, ")");
}

void CompressedCache::InitStats(Statistics* statistics) {
#if INCLUDE_HISTOGRAMS
  statistics->AddHistogram(kCompressedCacheSavings);
#endif
  statistics->AddVariable(kCompressedCacheCorruptPayloads);
  statistics->AddVariable(kCompressedCacheOriginalSize);
  statistics->AddVariable(kCompressedCacheCompressedSize);
}

void CompressedCache::Get(const GoogleString& key, Callback* callback) {
  CompressedCallback* cb = new CompressedCallback(callback, corrupt_payloads_,
                                                  max_inflated_value_size_);
  cache_->Get(key, cb);
}

void CompressedCache::Put(const GoogleString& key, const SharedString& value) {
  int64 old_size = value.size();
  GoogleString buf;
  buf.reserve(old_size + STATIC_STRLEN(kTrailer));
  StringWriter writer(&buf);
  original_size_->Add(old_size);
  if (GzipInflater::Deflate(value.Value(), GzipInflater::kDeflate, &writer)) {
    buf.append(kTrailer, STATIC_STRLEN(kTrailer));
#if INCLUDE_HISTOGRAMS
    compressed_cache_savings_->Add(old_size - static_cast<int64>(buf.size()));
#endif
    compressed_size_->Add(buf.size());
    cache_->PutSwappingString(key, &buf);
  }
}

void CompressedCache::Delete(const GoogleString& key) { cache_->Delete(key); }

bool CompressedCache::IsHealthy() const { return cache_->IsHealthy(); }

void CompressedCache::ShutDown() { return cache_->ShutDown(); }

int64 CompressedCache::CorruptPayloads() const {
  return corrupt_payloads_->Get();
}

int64 CompressedCache::OriginalSize() const { return original_size_->Get(); }

int64 CompressedCache::CompressedSize() const {
  return compressed_size_->Get();
}

}  // namespace net_instaweb
