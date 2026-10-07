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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_CRITICAL_SELECTOR_FINDER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_CRITICAL_SELECTOR_FINDER_H_

#include "net/instaweb/rewriter/critical_keys.pb.h"
#include "net/instaweb/rewriter/public/critical_finder_support_util.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class MessageHandler;
class NonceGenerator;
class RewriteDriver;
class Statistics;
class TimedVariable;
class Timer;

// This struct contains all the state that the finder needs, and will be held by
// the RewriteDriver. The critical_selectors field will be lazily initialized
// with the aggregate set of critical selectors strings from the protobuf entry,
// and should only need to be computed once per request (by
// UpdateCriticalSelectorInfoInDriver).
struct CriticalSelectorInfo {
  StringSet critical_selectors;
  // Every selector browsers have been asked about for this page, critical or
  // not.
  StringSet known_selectors;
  CriticalKeys proto;
};

// Interface to store/retrieve critical selector information in the property
// cache. See critical_finder_support_util.h for a description on how critical
// selectors are stored and updated.
class CriticalSelectorFinder {
 public:
  static const char kCriticalSelectorsValidCount[];
  static const char kCriticalSelectorsExpiredCount[];
  static const char kCriticalSelectorsNotFoundCount[];
  static const char kCriticalSelectorsPropertyName[];

  // All of the passed-in constructor arguments are owned by the caller.  If
  // critical selector data is being received from a trusted source
  // (ShouldReplacePriorResult() must return true in this case), nonce_generator
  // may be NULL.
  CriticalSelectorFinder(const PropertyCache::Cohort* cohort,
                         NonceGenerator* nonce_generator, Statistics* stats);
  virtual ~CriticalSelectorFinder();

  static void InitStats(Statistics* statistics);

  bool IsCriticalSelector(RewriteDriver* driver, const GoogleString& selector);

  const StringSet& GetCriticalSelectors(RewriteDriver* driver);

  // Every selector browsers have been asked about for this page, whether
  // they reported it as critical or not. A selector that is not in this set
  // has not been judged by any browser yet.
  const StringSet& GetKnownSelectors(RewriteDriver* driver);

  // True when at least one complete report has arrived since the page's set
  // of selectors last grew and since the last report that was cut short.
  // Until then the stored reports do not describe the page.
  bool HasCurrentBeaconData(RewriteDriver* driver);

  // True when this finder registers every selector it asks browsers about
  // (the default). A finder fed from a trusted source records only the
  // selectors that were reported.
  bool TracksCandidateSelectors() const { return !ShouldReplacePriorResult(); }

  // Records that a report arrived cut short at the beacon's size limit: its
  // nonce is used up, its selectors are not counted (the ones that did not
  // fit would look unmatched), and the page has no current data until a
  // complete report arrives.
  static void RecordTruncatedReport(StringPiece nonce,
                                    const PropertyCache* cache,
                                    const PropertyCache::Cohort* cohort,
                                    AbstractPropertyPage* page, Timer* timer);

  // Updates the critical selectors in the property cache. Support for the new
  // selector_set is added to the existing record of beacon support.  This
  // updates the value in the in-memory property page but does not write the
  // cohort.  If results are obtained from a trusted source
  // (ShouldReplacePriorResult() must return true) then nonce may be NULL.
  virtual void WriteCriticalSelectorsToPropertyCache(
      const StringSet& selector_set, StringPiece nonce, RewriteDriver* driver);

  // As above, but suitable for use in a beacon context where no RewriteDriver
  // is available.
  static void WriteCriticalSelectorsToPropertyCacheStatic(
      const StringSet& selector_set, StringPiece nonce, int support_interval,
      bool should_replace_prior_result, const PropertyCache* cache,
      const PropertyCache::Cohort* cohort, AbstractPropertyPage* page,
      MessageHandler* message_handler, Timer* timer);

  // Given a set of candidate critical selectors, decide whether beaconing
  // should take place.  We should *always* beacon if there's new critical
  // selector data.  Otherwise re-beaconing is based on a time and request
  // interval.  Returns the BeaconMetadata; result.status indicates whether
  // beaconing should occur.
  BeaconMetadata PrepareForBeaconInsertion(const StringSet& selector_set,
                                           RewriteDriver* driver);

  // Gets the SupportInterval for a new beacon result.
  virtual int SupportInterval() const = 0;

 protected:
  // Returns true if a beacon result should replace all previous results.
  virtual bool ShouldReplacePriorResult() const { return false; }

  // TODO(morlovich): Add an API for enabling the appropriate instrumentation
  // filter; once it's clear when the configuration resolving takes place.

 private:
  // RewriteDriver holds all of the state from the CriticalSelectorFinder. This
  // function should be called to update that state from the property cache
  // before it is used.
  void UpdateCriticalSelectorInfoInDriver(RewriteDriver* driver);

  const PropertyCache::Cohort* cohort_;
  NonceGenerator* nonce_generator_;

  TimedVariable* critical_selectors_valid_count_;
  TimedVariable* critical_selectors_expired_count_;
  TimedVariable* critical_selectors_not_found_count_;

  CriticalSelectorFinder(const CriticalSelectorFinder&) = delete;
  CriticalSelectorFinder& operator=(const CriticalSelectorFinder&) = delete;
};

class BeaconCriticalSelectorFinder : public CriticalSelectorFinder {
 public:
  BeaconCriticalSelectorFinder(const PropertyCache::Cohort* cohort,
                               NonceGenerator* nonce_generator,
                               Statistics* stats)
      : CriticalSelectorFinder(cohort, nonce_generator, stats) {}

  // truncated: the browser could not fit all matching selectors in the
  // report (see RecordTruncatedReport).
  static void WriteCriticalSelectorsToPropertyCacheFromBeacon(
      const StringSet& selector_set, StringPiece nonce, bool truncated,
      const PropertyCache* cache, const PropertyCache::Cohort* cohort,
      AbstractPropertyPage* page, MessageHandler* message_handler,
      Timer* timer);

 private:
  // Default support interval.
  static const int kDefaultSupportInterval = 10;

  // Gets the SupportInterval for a new beacon result (see comment at top).
  int SupportInterval() const override { return kDefaultSupportInterval; }
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_CRITICAL_SELECTOR_FINDER_H_
