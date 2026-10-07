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

#include "net/instaweb/rewriter/public/critical_selector_finder.h"

#include <map>
#include <memory>

#include "base/logging.h"
#include "net/instaweb/rewriter/critical_keys.pb.h"
#include "net/instaweb/rewriter/public/critical_finder_support_util.h"
#include "net/instaweb/rewriter/public/property_cache_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// "_v2": reports stored under the earlier name were collected with a
// different rule for what counts as critical, and are not read.
const char CriticalSelectorFinder::kCriticalSelectorsPropertyName[] =
    "critical_selectors_v2";

const char CriticalSelectorFinder::kCriticalSelectorsValidCount[] =
    "critical_selectors_valid_count";

const char CriticalSelectorFinder::kCriticalSelectorsExpiredCount[] =
    "critical_selectors_expired_count";

const char CriticalSelectorFinder::kCriticalSelectorsNotFoundCount[] =
    "critical_selectors_not_found_count";

CriticalSelectorFinder::CriticalSelectorFinder(
    const PropertyCache::Cohort* cohort, NonceGenerator* nonce_generator,
    Statistics* statistics)
    : cohort_(cohort), nonce_generator_(nonce_generator) {
  critical_selectors_valid_count_ =
      statistics->GetTimedVariable(kCriticalSelectorsValidCount);
  critical_selectors_expired_count_ =
      statistics->GetTimedVariable(kCriticalSelectorsExpiredCount);
  critical_selectors_not_found_count_ =
      statistics->GetTimedVariable(kCriticalSelectorsNotFoundCount);
}

CriticalSelectorFinder::~CriticalSelectorFinder() {}

void CriticalSelectorFinder::InitStats(Statistics* statistics) {
  statistics->AddTimedVariable(kCriticalSelectorsValidCount,
                               Statistics::kDefaultGroup);
  statistics->AddTimedVariable(kCriticalSelectorsExpiredCount,
                               Statistics::kDefaultGroup);
  statistics->AddTimedVariable(kCriticalSelectorsNotFoundCount,
                               Statistics::kDefaultGroup);
}

bool CriticalSelectorFinder::IsCriticalSelector(RewriteDriver* driver,
                                                const GoogleString& selector) {
  const StringSet& critical_selectors = GetCriticalSelectors(driver);
  return (critical_selectors.find(selector) != critical_selectors.end());
}

const StringSet& CriticalSelectorFinder::GetCriticalSelectors(
    RewriteDriver* driver) {
  UpdateCriticalSelectorInfoInDriver(driver);
  return driver->critical_selector_info()->critical_selectors;
}

const StringSet& CriticalSelectorFinder::GetKnownSelectors(
    RewriteDriver* driver) {
  UpdateCriticalSelectorInfoInDriver(driver);
  return driver->critical_selector_info()->known_selectors;
}

bool CriticalSelectorFinder::HasCurrentBeaconData(RewriteDriver* driver) {
  UpdateCriticalSelectorInfoInDriver(driver);
  return driver->critical_selector_info()->proto.valid_beacons_received() > 0;
}

void CriticalSelectorFinder::RecordTruncatedReport(
    StringPiece nonce, const PropertyCache* cache,
    const PropertyCache::Cohort* cohort, AbstractPropertyPage* page,
    Timer* timer) {
  if (page == nullptr || cohort == nullptr) {
    return;
  }
  PropertyCacheDecodeResult decode_result;
  std::unique_ptr<CriticalKeys> critical_keys(
      DecodeFromPropertyCache<CriticalKeys>(cache, page, cohort,
                                            kCriticalSelectorsPropertyName, -1,
                                            &decode_result));
  if (decode_result != kPropertyCacheDecodeOk || critical_keys == nullptr) {
    return;
  }
  if (!ValidateAndExpireNonce(timer->NowMs(), nonce, critical_keys.get())) {
    return;
  }
  // No complete report describes the page any more. Counting this one among
  // the reports that did not arrive makes a page that never fits its
  // selectors into one report fall back to the long interval instead of
  // being instrumented at the short one for ever.
  critical_keys->set_valid_beacons_received(0);
  critical_keys->set_nonces_recently_expired(
      critical_keys->nonces_recently_expired() + 1);
  UpdateInPropertyCache(*critical_keys, cohort, kCriticalSelectorsPropertyName,
                        false /* write_cohort */, page);
}

void CriticalSelectorFinder::WriteCriticalSelectorsToPropertyCache(
    const StringSet& selector_set, StringPiece nonce, RewriteDriver* driver) {
  DCHECK(cohort_ != nullptr);
  WriteCriticalSelectorsToPropertyCacheStatic(
      selector_set, nonce, SupportInterval(), ShouldReplacePriorResult(),
      driver->server_context()->page_property_cache(), cohort_,
      driver->property_page(), driver->message_handler(), driver->timer());
}

void CriticalSelectorFinder::WriteCriticalSelectorsToPropertyCacheStatic(
    const StringSet& selector_set, StringPiece nonce, int support_interval,
    bool should_replace_prior_result, const PropertyCache* cache,
    const PropertyCache::Cohort* cohort, AbstractPropertyPage* page,
    MessageHandler* message_handler, Timer* timer) {
  CriticalKeysWriteFlags flags;
  if (should_replace_prior_result) {
    flags = kReplacePriorResult;
  } else {
    flags = static_cast<CriticalKeysWriteFlags>(kRequirePriorSupport |
                                                kKeepUnmatchedCandidates);
  }

  WriteCriticalKeysToPropertyCache(selector_set, nonce, support_interval, flags,
                                   kCriticalSelectorsPropertyName, cache,
                                   cohort, page, message_handler, timer);
}

void CriticalSelectorFinder::UpdateCriticalSelectorInfoInDriver(
    RewriteDriver* driver) {
  if (driver->critical_selector_info() != nullptr) {
    return;
  }

  PropertyCacheDecodeResult result;
  // NOTE: if any of these checks fail you probably didn't set up your test
  // environment carefully enough.  Figuring that out based on test failures
  // alone will drive you nuts and take hours out of your life, thus DCHECKs.
  DCHECK(driver != nullptr);
  DCHECK(cohort_ != nullptr);
  std::unique_ptr<CriticalKeys> critical_keys(
      DecodeFromPropertyCache<CriticalKeys>(
          driver, cohort_, kCriticalSelectorsPropertyName,
          driver->options()->finder_properties_cache_expiration_time_ms(),
          &result));
  switch (result) {
    case kPropertyCacheDecodeNotFound:
      critical_selectors_not_found_count_->IncBy(1);
      break;
    case kPropertyCacheDecodeExpired:
      critical_selectors_expired_count_->IncBy(1);
      break;
    case kPropertyCacheDecodeParseError:
      driver->message_handler()->Message(
          kWarning,
          "Unable to parse Critical Selectors PropertyValue; "
          "url: %s",
          driver->url());
      break;
    case kPropertyCacheDecodeOk:
      critical_selectors_valid_count_->IncBy(1);
  }

  // Create a placeholder CriticalKeys to use in case the call to
  // DecodeFromPropertyCache above returned NULL.
  CriticalKeys static_keys;
  CriticalKeys* keys_to_use =
      (critical_keys == nullptr) ? &static_keys : critical_keys.get();

  CriticalSelectorInfo* critical_selector_info = new CriticalSelectorInfo;
  critical_selector_info->proto = *keys_to_use;
  GetCriticalKeysFromProto(0 /* support_percentage */, *keys_to_use,
                           &critical_selector_info->critical_selectors);
  for (int i = 0; i < keys_to_use->key_evidence_size(); ++i) {
    const GoogleString& key = keys_to_use->key_evidence(i).key();
    if (!key.empty()) {
      critical_selector_info->known_selectors.insert(key);
    }
  }
  driver->set_critical_selector_info(critical_selector_info);
}

BeaconMetadata CriticalSelectorFinder::PrepareForBeaconInsertion(
    const StringSet& selectors, RewriteDriver* driver) {
  UpdateCriticalSelectorInfoInDriver(driver);
  BeaconMetadata result;
  result.status = kDoNotBeacon;
  if (selectors.empty()) {
    return result;
  }
  if (ShouldReplacePriorResult()) {
    // The computed critical keys will not require a nonce as we trust all
    // beacon results.
    result.status = kBeaconNoNonce;
    return result;
  }
  // Avoid memory copy by capturing computed_nonce using RVA and swapping the
  // two strings.
  CriticalKeys& proto = driver->critical_selector_info()->proto;
  // If the candidate keys changed, force a rebeacon by clearing the next beacon
  // timestamp. A report that is still on its way was requested for the old
  // set of selectors and says nothing about the new ones, so the nonces
  // handed out before the change are dropped: only a browser that is asked
  // from here on can make the page's data current again.
  if (::net_instaweb::UpdateCandidateKeys(selectors, &proto, true)) {
    proto.clear_pending_nonce();
  }
  net_instaweb::PrepareForBeaconInsertionHelper(
      &proto, nonce_generator_, driver,
      true /* using_candidate_key_detection */, &result);
  if (result.status != kDoNotBeacon) {
    // Once reports agree the helper waits kLowFreqBeaconMult intervals. For
    // selectors that wait is how long a page whose markup changed is served
    // with rules missing from its inline CSS, so it is kept shorter. (When
    // reports are not arriving at all, the helper's long wait stands.)
    if (proto.nonces_recently_expired() <= kNonceExpirationLimit &&
        proto.valid_beacons_received() >= kHighFreqBeaconCount) {
      proto.set_next_beacon_timestamp_ms(
          driver->timer()->NowMs() +
          driver->options()->beacon_reinstrument_time_sec() * Timer::kSecondMs *
              kCriticalSelectorLowFreqBeaconMult);
    }
    DCHECK(cohort_ != nullptr);
    UpdateInPropertyCache(proto, cohort_, kCriticalSelectorsPropertyName,
                          true /* write_cohort */, driver->property_page());
  }
  return result;
}

void BeaconCriticalSelectorFinder::
    WriteCriticalSelectorsToPropertyCacheFromBeacon(
        const StringSet& selector_set, StringPiece nonce, bool truncated,
        const PropertyCache* cache, const PropertyCache::Cohort* cohort,
        AbstractPropertyPage* page, MessageHandler* message_handler,
        Timer* timer) {
  if (truncated) {
    RecordTruncatedReport(nonce, cache, cohort, page, timer);
    return;
  }
  return CriticalSelectorFinder::WriteCriticalSelectorsToPropertyCacheStatic(
      selector_set, nonce, kDefaultSupportInterval, false, cache, cohort, page,
      message_handler, timer);
}

}  // namespace net_instaweb
