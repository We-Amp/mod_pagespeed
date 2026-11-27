// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test-only access to the substrate underneath PageSpeedCache.
//
// PageSpeedCache deliberately does not publish `cyclone::Cache&`.  The reason
// is the alternate chain: a plain whole-key write on a key that carries one
// repoints the directory head at a document with no alternate id, which
// ORPHANS every alternate on that key at once — silently, totally, and
// looking exactly like a cold cache.  Every method on the cache is
// alternate-scoped so that this cannot happen; publishing the raw handle
// alongside them would leave that a matter of everyone's good behaviour
// rather than a property of the type.
//
// Tests still need the raw handle: to set up entries the public API refuses
// to create, to stop the cache mid-flight, and — in
// durable_originals_test.cc — to DEMONSTRATE the orphaning hazard, which is
// what makes the invariant worth asserting.  That is what this peer is for,
// and its being a separate, test-only target is the point: production code
// cannot reach past the cache without adding a dependency that says so.

#ifndef PAGESPEED_TEST_TEST_UTIL_CACHE_TEST_PEER_H_
#define PAGESPEED_TEST_TEST_UTIL_CACHE_TEST_PEER_H_

#include <atomic>
#include <string>

#include "cyclone/cache.hpp"
#include "lib/cache/cache.h"

namespace pagespeed {

class PageSpeedCacheTestPeer {
 public:
  static cyclone::Cache& Cyclone(PageSpeedCache& cache) {
    return cache.RawCycloneCacheForTesting();
  }

  // Makes the cache treat `path` as a live volume file (a purge then tries to
  // replace it), to stage a purge over more than one file.
  static void AddLivePath(PageSpeedCache& cache, const std::string& path) {
    cache.volume_file_paths_.push_back(path);
  }

  // Makes the next `count` substrate content reads return CacheError::Busy
  // without reaching the substrate, to drive the Busy retry deterministically.
  static void ForceBusyReads(PageSpeedCache& cache, int count) {
    cache.forced_busy_reads_for_testing_.store(count,
                                               std::memory_order_relaxed);
  }
};

}  // namespace pagespeed

#endif  // PAGESPEED_TEST_TEST_UTIL_CACHE_TEST_PEER_H_
