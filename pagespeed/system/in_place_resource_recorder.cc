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

#include "pagespeed/system/in_place_resource_recorder.h"

#include <algorithm>

#include "base/logging.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_cache_failure.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/inflating_fetch.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/gzip_inflater.h"

namespace net_instaweb {

namespace {

const char kNumResources[] = "ipro_recorder_resources";
const char kNumInsertedIntoCache[] = "ipro_recorder_inserted_into_cache";
const char kNumNotCacheable[] = "ipro_recorder_not_cacheable";
const char kNumFailed[] = "ipro_recorder_failed";
const char kNumDroppedDueToLoad[] = "ipro_recorder_dropped_due_to_load";
const char kNumDroppedDueToSize[] = "ipro_recorder_dropped_due_to_size";
const char kNumDroppedContentType[] = "ipro_recorder_dropped_content_type";
const char kNumErrorStatus[] = "ipro_recorder_error_status";
const char kNumSkippedTransient[] = "ipro_recorder_skipped_transient";
const char kNumEmpty[] = "ipro_recorder_empty";

}  // namespace

AtomicInt32 InPlaceResourceRecorder::active_recordings_(0);

InPlaceResourceRecorder::InPlaceResourceRecorder(
    const RequestContextPtr& request_context, StringPiece url,
    StringPiece fragment, const RequestHeaders::Properties& request_properties,
    int max_response_bytes, int max_concurrent_recordings, HTTPCache* cache,
    Statistics* stats, MessageHandler* handler)
    : url_(url.data(), url.size()),
      fragment_(fragment.data(), fragment.size()),
      request_properties_(request_properties),
      http_options_(request_context->options()),
      max_response_bytes_(max_response_bytes),
      max_concurrent_recordings_(max_concurrent_recordings),
      write_to_resource_value_(request_context, &resource_value_),
      inflating_fetch_(&write_to_resource_value_),
      cache_(cache),
      handler_(handler),
      num_resources_(stats->GetVariable(kNumResources)),
      num_inserted_into_cache_(stats->GetVariable(kNumInsertedIntoCache)),
      num_not_cacheable_(stats->GetVariable(kNumNotCacheable)),
      num_failed_(stats->GetVariable(kNumFailed)),
      num_dropped_due_to_load_(stats->GetVariable(kNumDroppedDueToLoad)),
      num_dropped_due_to_size_(stats->GetVariable(kNumDroppedDueToSize)),
      num_dropped_content_type_(stats->GetVariable(kNumDroppedContentType)),
      num_error_status_(stats->GetVariable(kNumErrorStatus)),
      num_skipped_transient_(stats->GetVariable(kNumSkippedTransient)),
      num_empty_(stats->GetVariable(kNumEmpty)),
      status_code_(-1),
      failure_(false),
      specific_outcome_recorded_(false),
      full_response_headers_considered_(false),
      consider_response_headers_called_(false),
      cache_control_set_(false) {
  num_resources_->Add(1);
  if (limit_active_recordings() &&
      active_recordings_.BarrierIncrement(1) > max_concurrent_recordings_) {
    VLOG(1) << "IPRO: too many recordings in progress, not recording";
    RecordSpecificOutcome(num_dropped_due_to_load_);
    handler_->Message(kInfo,
                      "IPRO: not recording %s: too many concurrent recordings",
                      url_.c_str());
    failure_ = true;
  }

  // The http cache also has a maximum response body length that it will accept,
  // so we need to look at max_response_bytes_ and takes the most constraining
  // of the two.
  int64 cache_max_cl = cache_->max_cacheable_response_content_length();
  if (cache_max_cl != -1) {
    if (max_response_bytes_ <= 0) {
      max_response_bytes_ = cache_max_cl;
    } else {
      max_response_bytes_ = std::min(max_response_bytes_, cache_max_cl);
    }
  }
}

InPlaceResourceRecorder::~InPlaceResourceRecorder() {
  if (limit_active_recordings()) {
    active_recordings_.BarrierIncrement(-1);
  }
}

void InPlaceResourceRecorder::InitStats(Statistics* statistics) {
  statistics->AddVariable(kNumResources);
  statistics->AddVariable(kNumInsertedIntoCache);
  statistics->AddVariable(kNumNotCacheable);
  statistics->AddVariable(kNumFailed);
  statistics->AddVariable(kNumDroppedDueToLoad);
  statistics->AddVariable(kNumDroppedDueToSize);
  statistics->AddVariable(kNumDroppedContentType);
  statistics->AddVariable(kNumErrorStatus);
  statistics->AddVariable(kNumSkippedTransient);
  statistics->AddVariable(kNumEmpty);
}

bool InPlaceResourceRecorder::RecordSpecificOutcome(Variable* counter) {
  if (specific_outcome_recorded_) {
    return false;
  }
  specific_outcome_recorded_ = true;
  counter->Add(1);
  return true;
}

bool InPlaceResourceRecorder::Write(const StringPiece& contents,
                                    MessageHandler* handler) {
  DCHECK(consider_response_headers_called_);
  if (failure_) {
    return false;
  }

  // Write into resource_value_ decompressing if needed. A write/inflate
  // error here is a genuine recording malfunction: leave it uncounted so
  // DoneAndSetHeaders() attributes it to num_failed_. Write() short-circuits
  // once failure_ is set, so this fires at most once per recording.
  failure_ = !inflating_fetch_.Write(contents, handler_);
  if (failure_) {
    handler_->Message(kWarning,
                      "IPRO: failed to record %s: write/inflate error",
                      url_.c_str());
  }
  if (max_response_bytes_ <= 0 ||
      resource_value_.contents_size() < max_response_bytes_) {
    return !failure_;
  } else {
    DroppedDueToSize();
    VLOG(1) << "IPRO: MaxResponseBytes exceeded while recording " << url_;
    return false;
  }
}

void InPlaceResourceRecorder::ConsiderResponseHeaders(
    HeadersKind headers_kind, ResponseHeaders* response_headers) {
  CHECK(response_headers != nullptr);
  DCHECK(!full_response_headers_considered_);

  if (!consider_response_headers_called_) {
    consider_response_headers_called_ = true;
    // In first call, set up headers for potential deflating. We basically only
    // care about Content-Encoding, plus AsyncFetch gets unhappy with 0
    // status code.
    inflating_fetch_.response_headers()->CopyFrom(*response_headers);
    write_to_resource_value_.response_headers()->set_status_code(
        HttpStatus::kOK);
  }

  status_code_ = response_headers->status_code();

  // Shortcut for bailing out early when the response will be too large.
  int64 content_length;
  if (max_response_bytes_ <= 0 &&
      response_headers->FindContentLength(&content_length) &&
      content_length > max_response_bytes_) {
    VLOG(1) << "IPRO: Content-Length header indicates that [" << url_
            << "] is too large to record (" << content_length << " bytes)";
    DroppedDueToSize();
    return;
  }

  // Check if IPRO applies considering the content type, if we have one at
  // this point.  Depending on the server, we may only know the content type
  // after the we are called with kFullHeaders.
  //
  // Note: in a proxy setup it might be desirable to cache HTML and
  // non-rewritable Content-Types to avoid re-fetching from the origin server.
  //
  // If we have the full-headers, then we demand to have a good content type
  // now.
  if (response_headers->Has(HttpAttributes::kContentType) ||
      (headers_kind == kFullHeaders)) {
    const ContentType* content_type = response_headers->DetermineContentType();

    // Bail if not an image, css, or JS.
    if ((content_type == nullptr) ||
        !(content_type->IsImage() || content_type->IsCss() ||
          content_type->IsJsLike())) {
      // Not an image/CSS/JS-like content type: this is an expected, benign
      // bail (HTML, PDF, plain text, ...), not a recording malfunction.
      // Count it in its own bucket so it never inflates num_failed_.
      if (RecordSpecificOutcome(num_dropped_content_type_)) {
        handler_->Message(kInfo,
                          "IPRO: not recording %s: content type is not "
                          "rewritable (image/CSS/JS)",
                          url_.c_str());
      }
      // DroppedAsUncacheable().  If at some point we decide to go this
      // way, we must also change the expected cache_inserts count in
      // "Blocking rewrite enabled." in apache/system_test.sh from 3 to
      // 2.
      if (headers_kind == kFullHeaders) {
        // If we have to wait till we have recorded all the bytes to learn
        // that this content-type is uninteresting, then we should cache
        // that so we don't have to re-record.
        DroppedAsUncacheable();
      } else {
        // If we were able to learn the content-type early then the added
        // caching pressure is not worth short-circuiting the filter, and we can
        // simply bail here on every request.
        // scripts/siege_html_high_entropy.sh saw a 16% benefit with this
        // strategy.
        failure_ = true;
      }
      return;
    }
  }

  if (headers_kind != kFullHeaders) {
    return;
  }
  full_response_headers_considered_ = true;

  // For 4xx and 5xx we can't IPRO, but we can also cache the failure so we
  // don't retry recording for a bit.
  if (response_headers->IsErrorStatus()) {
    FetchResponseStatus failure_kind = kFetchStatusOtherError;
    if (status_code_ >= 400 && status_code_ < 500) {
      failure_kind = kFetchStatus4xxError;
    }
    cache_->RememberFailure(url_, fragment_, failure_kind, handler_);
    // A 4xx/5xx origin response is a broken-resource signal for the operator,
    // but not a recorder malfunction; it gets its own counter.
    if (RecordSpecificOutcome(num_error_status_)) {
      handler_->Message(kInfo, "IPRO: not recording %s: origin returned %d",
                        url_.c_str(), status_code_);
    }
    failure_ = true;
    return;
  }

  // We can't optimize anything that's not a 200, so say recording failed
  // for such statuses. However, we don't cache the failure here: for statuses
  // like 304 and 206 an another response is likely to be a 200 soon. We group
  // the other stuff with them here since it's the conservative default.
  if (status_code_ != HttpStatus::kOK) {
    // Transient non-200 (304 Not Modified, 206 Partial Content, ...):
    // deliberately not remembered, and not a malfunction. Own counter.
    if (RecordSpecificOutcome(num_skipped_transient_)) {
      handler_->Message(kInfo,
                        "IPRO: not recording %s: transient status %d "
                        "(not a 200)",
                        url_.c_str(), status_code_);
    }
    failure_ = true;
    return;
  }

  bool is_cacheable = response_headers->IsProxyCacheable(
      request_properties_,
      ResponseHeaders::GetVaryOption(http_options_.respect_vary),
      ResponseHeaders::kNoValidator);
  if (!is_cacheable) {
    DroppedAsUncacheable();
    // Cache-Control forbids caching: an expected policy outcome, not a
    // malfunction. Keeps its existing dedicated counter.
    if (RecordSpecificOutcome(num_not_cacheable_)) {
      handler_->Message(kInfo,
                        "IPRO: not recording %s: response is not cacheable",
                        url_.c_str());
    }
    return;
  }
}

void InPlaceResourceRecorder::DroppedDueToSize() {
  // Oversize responses are a deliberate size-limit policy outcome, not a
  // malfunction; keep their dedicated counter and out of num_failed_.
  if (RecordSpecificOutcome(num_dropped_due_to_size_)) {
    handler_->Message(kInfo,
                      "IPRO: not recording %s: response exceeds the size limit",
                      url_.c_str());
  }
  // Too big == too big to cache.
  DroppedAsUncacheable();
}

void InPlaceResourceRecorder::DroppedAsUncacheable() {
  if (!failure_) {
    cache_->RememberFailure(url_, fragment_,
                            status_code_ == 200 ? kFetchStatusUncacheable200
                                                : kFetchStatusUncacheableError,
                            handler_);
    failure_ = true;
  }
}

void InPlaceResourceRecorder::SaveCacheControl(const char* cache_control) {
  cache_control_set_ = true;
  if (cache_control != nullptr) {
    cache_control_ = cache_control;
  }
}

void InPlaceResourceRecorder::DoneAndSetHeaders(
    ResponseHeaders* response_headers, bool entire_response_received) {
  if (!entire_response_received) {
    // To record successfully, we must have a complete response.  Otherwise you
    // get https://github.com/apache/incubator-pagespeed-mod/issues/1081.
    // A truncated response is a genuine recording failure: leave it uncounted
    // by the specific buckets so it lands in num_failed_ below.
    handler_->Message(
        kWarning, "IPRO: failed to record %s: incomplete/truncated response",
        url_.c_str());
    Fail();
  }

  if (!failure_ && !full_response_headers_considered_) {
    ConsiderResponseHeaders(kFullHeaders, response_headers);
  }

  // Ignore Empty 200 responses.
  // https://github.com/apache/incubator-pagespeed-mod/issues/1050
  // A legal empty resource is a deliberate cache-the-skip, not a malfunction,
  // so it gets its own counter. Guarded on !failure_ so a truncated or
  // otherwise-failed response that happens to be empty stays a genuine
  // failure rather than being reclassified as "empty".
  if (!failure_ && status_code_ == HttpStatus::kOK &&
      resource_value_.contents_size() == 0) {
    cache_->RememberFailure(url_, fragment_, kFetchStatusEmpty, handler_);
    if (RecordSpecificOutcome(num_empty_)) {
      handler_->Message(kInfo, "IPRO: not recording %s: empty 200 response",
                        url_.c_str());
    }
    failure_ = true;
  }

  if (failure_) {
    // num_failed_ counts genuine recording malfunctions only. Every expected
    // or policy outcome above recorded a specific counter and set
    // specific_outcome_recorded_, so only write/inflate errors and truncated
    // responses reach this bump.
    if (!specific_outcome_recorded_) {
      num_failed_->Add(1);
    }
  } else {
    // We are skeptical of the correctness of the  content-encoding here,
    // since it can  be captured post-mod_deflate with pre-deflate content.
    // Also note that content-length doesn't have to be accurate either, since
    // it can be due to compression; we do still use it for quickly reject since
    // if gzip'd is too large uncompressed is likely too large, too. We sniff
    // the content to make sure that the headers match the Content-Encoding.
    StringPiece contents;
    resource_value_.ExtractContents(&contents);
    // TODO(jcrowell): remove this sniffing fix, and do a proper fix by merging
    // the IPRO filters in mod_instaweb.cc and in ngx_pagespeed.
    if (!GzipInflater::HasGzipMagicBytes(contents)) {
      // Only remove these headers if the content is not gzipped.
      response_headers->RemoveAll(HttpAttributes::kContentEncoding);
    }
    response_headers->RemoveAll(HttpAttributes::kContentLength);

    if (cache_control_set_) {
      // Use the cache control value from SaveCacheControl instead of the one in
      // the response.
      response_headers->RemoveAll(HttpAttributes::kCacheControl);
      if (!cache_control_.empty()) {
        response_headers->Add(HttpAttributes::kCacheControl, cache_control_);
      }
    }

    resource_value_.SetHeaders(response_headers);
    cache_->Put(url_, fragment_, request_properties_, http_options_,
                &resource_value_, handler_);
    // TODO(sligocki): Start IPRO rewrite.
    num_inserted_into_cache_->Add(1);
  }
  delete this;
}

}  // namespace net_instaweb
