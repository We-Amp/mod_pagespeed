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

#include "pagespeed/apache/apache_mmap_bucket.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"

namespace net_instaweb {

namespace {

// Private bucket data, shared (refcounted) across split()/copy() siblings.
struct MmapBucketData {
  apr_bucket_refcount refcount;  // Must be first (apr_bucket_shared_*).
  MappedSharedString* pin;       // Owns one ref on the Cyclone read handle.
  const char* base;              // Start of the aliased span.
  Variable* copied_out_stat;     // May be null.
  Variable* renew_fail_stat;     // May be null.
};

void MmapBucketDestroy(void* data) {
  MmapBucketData* d = static_cast<MmapBucketData*>(data);
  if (apr_bucket_shared_destroy(d)) {
    delete d->pin;  // Last reference: release the read-handle pin.
    apr_bucket_free(d);
  }
}

// A bucket whose serve is already known-lost: read() always fails.  Used
// when a copy-out fails inside setaside(), whose errors httpd's
// deferred-write path DISCARDS (setaside_remaining_output is void and drops
// ap_save_brigade's status), so returning an error there would let the
// serve "succeed" with the tail silently gone.  Poisoning instead defers
// the failure to the next send attempt, where the core output filter's
// apr_bucket_read failure genuinely aborts the connection.
apr_status_t TornBucketRead(apr_bucket* b, const char** str, apr_size_t* len,
                            apr_read_type_e /*block*/) {
  *str = nullptr;
  *len = 0;
  return APR_EGENERAL;
}

apr_status_t TornBucketSetaside(apr_bucket* /*b*/, apr_pool_t* /*pool*/) {
  return APR_SUCCESS;  // Nothing to park; the read() failure is the payload.
}

void TornBucketDestroy(void* /*data*/) {}

const apr_bucket_type_t kTornAliasBucketType = {
    "PAGESPEED_TORN",
    5,
    apr_bucket_type_t::APR_BUCKET_DATA,
    TornBucketDestroy,
    TornBucketRead,
    TornBucketSetaside,
    apr_bucket_split_notimpl,
    apr_bucket_copy_notimpl,
};

// Morphs 'b' (of any current type) into a poison bucket, releasing whatever
// private data it still holds (the pinned mapped data if the heap morph
// never happened, or the heap copy holding torn bytes).  b->length is kept
// so downstream byte accounting stays consistent.
void PoisonBucket(apr_bucket* b) {
  b->type->destroy(b->data);
  b->type = &kTornAliasBucketType;
  b->data = nullptr;
}

// De-aliases 'b' in place: morphs it into a heap bucket over an owned copy
// of its window, with a copy-then-verify -- a ceiling-forced
// wrap ignores the lease and can race the memcpy, so the borrow is
// re-checked AFTER the bytes were copied.  On kTorn the copied bytes may
// be garbage: fail so the serve is aborted before the Content-Length
// promise is completed with a corrupt body.  Split/copy siblings sharing
// the refcounted data are unaffected; each runs its own barrier when read.
apr_status_t MmapBucketCopyOut(apr_bucket* b) {
  MmapBucketData* d = static_cast<MmapBucketData*>(b->data);
  // The memcpy below dereferences the mapped span with no further validity
  // probe: the pin (a MappedSharedString reference on the Cyclone read
  // handle) keeps the MAPPING itself alive -- unmap happens only when the
  // last reference drops -- so the pointer cannot dangle while 'd' holds
  // the pin.  What the pin does NOT guarantee is byte stability, which is
  // exactly what the verify AFTER the copy checks.  (Serve and release both
  // happen on the request thread, which precedes any cache teardown in the
  // child's lifecycle.)
  const char* src = d->base + b->start;
  const apr_size_t len = b->length;
  // With a null free function apr_bucket_heap_make copies 'src' into a
  // fresh heap allocation (pool-independent) and rewrites b's type/data;
  // on allocation failure it returns null and leaves b untouched.
  if (apr_bucket_heap_make(b, src, len, nullptr) == nullptr) {
    return APR_ENOMEM;
  }
  // Copy-then-verify, through the pin this bucket still holds via 'd'.
  const LeaseRenewal verdict = d->pin->RenewLeaseStrict();
  const bool torn = (verdict == LeaseRenewal::kTorn);
  if (torn) {
    if (d->renew_fail_stat != nullptr) {
      d->renew_fail_stat->Add(1);
    }
  } else if (d->copied_out_stat != nullptr) {
    d->copied_out_stat->Add(1);
  }
  // b is a heap bucket now; drop the reference it held on the mapped data.
  MmapBucketDestroy(d);
  return torn ? APR_EGENERAL : APR_SUCCESS;
}

apr_status_t MmapBucketRead(apr_bucket* b, const char** str, apr_size_t* len,
                            apr_read_type_e block) {
  MmapBucketData* d = static_cast<MmapBucketData*>(b->data);
  // Per-send lease barrier.  The core output filter re-reads every
  // pending bucket (a fresh apr_bucket_read per bucket) when it rebuilds
  // its writev iovecs after every poll wait, so this intent-checked
  // revalidation runs in the same stack, microseconds before the kernel
  // copies the bytes: no wait-then-send window is left open.
  const LeaseRenewal verdict = d->pin->RenewLeaseStrict();
  if (verdict == LeaseRenewal::kTorn) {
    // A wrap committed over the borrow: the bytes may be garbage and the
    // response cannot be produced correctly any more.  Fail closed.
    if (d->renew_fail_stat != nullptr) {
      d->renew_fail_stat->Add(1);
    }
    return APR_EGENERAL;
  }
  if (verdict != LeaseRenewal::kOk ||
      d->pin->NsUntilForcedWrap() <= kMmapAliasReadBarrierMarginNs) {
    // Wrap in flight (region intact), leases disabled, or a ceiling-forced
    // wrap -- which ignores the lease -- is imminent: de-alias by copying
    // now, then serve the owned bytes.
    const apr_status_t rv = MmapBucketCopyOut(b);
    if (rv != APR_SUCCESS) {
      return rv;
    }
    return apr_bucket_read(b, str, len, block);
  }
  *str = d->base + b->start;
  *len = b->length;
  return APR_SUCCESS;
}

// Retention past the current filter pass (deferred core-filter writes via
// ap_save_brigade, mod_http2 beams, async write completion) outlives any
// lease guarantee: RETAIN BY COPY.  The heap morph's storage is
// pool-independent, which satisfies setaside's lifetime contract the same
// way a transient bucket's heap morph does.
//
// A failed copy-out (torn borrow, or the heap allocation failed) must NOT
// be reported as a setaside error: httpd's deferred-write path discards
// setaside errors (core_filters.c setaside_remaining_output is void;
// util_filter.c ap_save_brigade's status is dropped), which would leave a
// "successful" serve with a silently missing or torn tail.  Instead the
// bucket is morphed into a poison bucket and APR_SUCCESS is returned; the
// failure then fires at the next send attempt's read, which the core
// output filter honors by aborting the connection.
apr_status_t MmapBucketSetaside(apr_bucket* b, apr_pool_t* /*pool*/) {
  const apr_status_t rv = MmapBucketCopyOut(b);
  if (rv != APR_SUCCESS) {
    PoisonBucket(b);
  }
  return APR_SUCCESS;
}

const apr_bucket_type_t kMmapAliasBucketType = {
    "PAGESPEED_MMAP",
    5,
    apr_bucket_type_t::APR_BUCKET_DATA,
    MmapBucketDestroy,
    MmapBucketRead,
    MmapBucketSetaside,
    apr_bucket_shared_split,
    apr_bucket_shared_copy,
};

}  // namespace

apr_bucket* CreateMmapAliasBucket(const StringPiece& span,
                                  const MappedSharedString& pin,
                                  Variable* copied_out_stat,
                                  Variable* renew_fail_stat,
                                  apr_bucket_alloc_t* list) {
  apr_bucket* b = static_cast<apr_bucket*>(apr_bucket_alloc(sizeof(*b), list));
  if (b == nullptr) {
    return nullptr;
  }
  APR_BUCKET_INIT(b);
  b->free = apr_bucket_free;
  b->list = list;
  MmapBucketData* d =
      static_cast<MmapBucketData*>(apr_bucket_alloc(sizeof(*d), list));
  if (d == nullptr) {
    apr_bucket_free(b);
    return nullptr;
  }
  d->pin = new MappedSharedString(pin);  // Refcount bump on the read handle.
  d->base = span.data();
  d->copied_out_stat = copied_out_stat;
  d->renew_fail_stat = renew_fail_stat;
  b = apr_bucket_shared_make(b, d, 0, span.size());
  b->type = &kMmapAliasBucketType;
  return b;
}

bool IsMmapAliasBucket(const apr_bucket* b) {
  return b->type == &kMmapAliasBucketType;
}

bool IsTornAliasBucket(const apr_bucket* b) {
  return b->type == &kTornAliasBucketType;
}

}  // namespace net_instaweb
