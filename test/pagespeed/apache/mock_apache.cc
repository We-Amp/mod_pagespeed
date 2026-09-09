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

#include "test/pagespeed/apache/mock_apache.h"

#include <cstdlib>
#include <vector>

// clang-format off: the httpd headers below require apache_httpd_includes.h
// (httpd.h) first; alphabetical include sorting breaks them.
#include "base/logging.h"
#include "pagespeed/apache/apache_httpd_includes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "apr_buckets.h"  // NOLINT - for the ap_pass_brigade mock
#include "apr_strings.h"  // NOLINT - for apr_pstrdup
#include "http_log.h"     // NOLINT - for ap_log_*_ declarations
#include "http_protocol.h"  // NOLINT - for AP_DECLARE_HOOK types
#include "http_request.h"   // NOLINT - for AP_DECLARE_HOOK types
#include "unixd.h"        // NOLINT - for unixd_config_rec
#include "util_filter.h"  // NOLINT
// clang-format on

namespace {

net_instaweb::StringVector* recorded_actions = nullptr;
bool apr_initialized = false;
bool partial_pass_brigade = false;

}  // namespace

namespace net_instaweb {

void MockApache::Initialize() {
  CHECK(recorded_actions == nullptr);
  recorded_actions = new StringVector();
  partial_pass_brigade = false;
  if (!apr_initialized) {
    apr_initialize();
    atexit(apr_terminate);
    apr_initialized = true;
  }
}

void MockApache::Terminate() {
  CHECK(recorded_actions != nullptr);
  if (!recorded_actions->empty()) {
    LOG(FATAL) << "MockApache: unprocessed actions: " << ActionsSinceLastCall();
  }
  delete recorded_actions;
  recorded_actions = nullptr;
}

void MockApache::PrepareRequest(request_rec* request) {
  apr_pool_create(&request->pool, nullptr);
  request->headers_in = apr_table_make(request->pool, 10);
  request->headers_out = apr_table_make(request->pool, 10);
  request->subprocess_env = apr_table_make(request->pool, 10);

  // Create three fake downstream filters so we can make sure the right ones are
  // removed.
  int n_fake_filters = 3;
  ap_filter_t** filter = &request->output_filters;
  for (int i = 0; i < n_fake_filters; ++i, filter = &(*filter)->next) {
    *filter = static_cast<ap_filter_t*>(
        apr_palloc(request->pool, sizeof(ap_filter_t)));
    (*filter)->frec = static_cast<ap_filter_rec_t*>(
        apr_palloc(request->pool, sizeof(ap_filter_rec_t)));
    const char* filter_name;
    switch (i) {
      case 0:
        filter_name = "MOD_EXPIRES";
        break;
      case 1:
        filter_name = "FIXUP_HEADERS_OUT";
        break;
      case 2:
        filter_name = "OTHER_FILTER";
        break;
      default:
        LOG(FATAL) << "There should only be three fake filters.";
    }
    (*filter)->frec->name = filter_name;
  }
  *filter = nullptr;  // Terminate the linked list.
}

void MockApache::CleanupRequest(request_rec* request) {
  apr_pool_destroy(request->pool);
}

void MockApache::set_partial_pass_brigade(bool enabled) {
  partial_pass_brigade = enabled;
}

GoogleString MockApache::ActionsSinceLastCall() {
  CHECK(recorded_actions != nullptr)
      << "Must call MockApache::Initialize() first";
  GoogleString response = JoinCollection(*recorded_actions, " ");
  recorded_actions->clear();
  return response;
}

}  // namespace net_instaweb

namespace {

void log_action(StringPiece action) {
  CHECK(recorded_actions != nullptr)
      << "Must call MockApache::Initialize() first";
  recorded_actions->push_back(action.as_string());
}

void log_fatal(StringPiece function) {
  LOG(FATAL) << function << " should not be called";
}

}  // namespace

extern "C" {

int ap_rwrite(const void* buf, int nbyte, request_rec* r) {
  log_action(StrCat("ap_rwrite(",
                    StringPiece(static_cast<const char*>(buf), nbyte), ")"));
  return 1;
}

int ap_rflush(request_rec* r) {
  log_action("ap_rflush()");
  return 1;
}

void ap_set_content_length(request_rec* r, apr_off_t length) {
  log_action(StrCat("ap_set_content_length(",
                    net_instaweb::IntegerToString(length), ")"));
}

void ap_set_content_type(request_rec* r, const char* ct) {
  log_action(StrCat("ap_set_content_type(", StringPiece(ct), ")"));
  // Incomplete implementation, but enough to be testing.
  apr_table_set(r->headers_out, net_instaweb::HttpAttributes::kContentType, ct);
}

void ap_remove_output_filter(ap_filter_t* filter) {
  CHECK(filter != nullptr);
  CHECK(filter->frec != nullptr);
  log_action(StrCat("ap_remove_output_filter(", filter->frec->name, ")"));
}

ap_filter_t* ap_add_output_filter(const char* name, void*, request_rec*,
                                  conn_rec*) {
  log_action(StrCat("ap_add_output_filter(", StringPiece(name), ")"));
  return nullptr;
}

apr_status_t ap_get_brigade(ap_filter_t*, apr_bucket_brigade*, ap_input_mode_t,
                            apr_read_type_e, apr_off_t) {
  log_fatal("ap_get_brigade");
  return 0;
}

// Functional mock: consumes the brigade the way a downstream chain would,
// reading every data bucket (which runs a PAGESPEED_MMAP bucket's lease
// read barrier) and logging the concatenated bytes.  Read failures (e.g. a
// torn mapped borrow) propagate to the caller like a downstream error.
apr_status_t ap_pass_brigade(ap_filter_t*, apr_bucket_brigade* bb) {
  if (APR_BRIGADE_EMPTY(bb)) {
    // ApacheWriter::SettleOutputFilters() passes one empty brigade before
    // the eligibility walk.  Log it distinctly, and keep
    // partial mode's data-bucket invariant for real body passes.
    log_action("ap_pass_brigade(EMPTY)");
    return APR_SUCCESS;
  }
  if (partial_pass_brigade) {
    // Model the deferred-write geometry (see mock_apache.h) for the first
    // data bucket.
    apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
    while (bucket != APR_BRIGADE_SENTINEL(bb) &&
           APR_BUCKET_IS_METADATA(bucket)) {
      bucket = APR_BUCKET_NEXT(bucket);
    }
    CHECK(bucket != APR_BRIGADE_SENTINEL(bb))
        << "partial mode needs a data bucket";
    const char* data = nullptr;
    apr_size_t len = 0;
    apr_status_t rv = apr_bucket_read(bucket, &data, &len, APR_BLOCK_READ);
    if (rv != APR_SUCCESS) {
      log_action("ap_pass_brigade_partial(first=READ_ERROR)");
      return rv;
    }
    CHECK_GE(len, 2u) << "partial mode needs a splittable bucket";
    const apr_size_t half = len / 2;
    GoogleString first(data, half);
    CHECK_EQ(APR_SUCCESS, apr_bucket_split(bucket, half));
    apr_bucket* rest = APR_BUCKET_NEXT(bucket);
    apr_bucket_delete(bucket);  // First half accepted by the "socket".
    // Deferred-write park.  Real httpd DISCARDS this status
    // (setaside_remaining_output is void), so the mock logs it but does not
    // return it -- a failure must surface through the re-read below, the
    // way a poisoned parked bucket fails the next real drain.
    apr_status_t setaside_rv = apr_bucket_setaside(rest, bb->p);
    // The next send attempt: re-read (the re-entry barrier), then consume.
    apr_status_t rest_rv = apr_bucket_read(rest, &data, &len, APR_BLOCK_READ);
    if (rest_rv != APR_SUCCESS) {
      log_action(StrCat("ap_pass_brigade_partial(first=", first, ",setaside=",
                        net_instaweb::IntegerToString(setaside_rv),
                        ",rest=READ_ERROR)"));
      return rest_rv;
    }
    GoogleString rest_bytes(data, len);
    apr_brigade_cleanup(bb);
    log_action(StrCat("ap_pass_brigade_partial(first=", first,
                      ",setaside=", net_instaweb::IntegerToString(setaside_rv),
                      ",rest=", rest_bytes, ")"));
    return APR_SUCCESS;
  }
  GoogleString bytes;
  for (apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
       bucket != APR_BRIGADE_SENTINEL(bb); bucket = APR_BUCKET_NEXT(bucket)) {
    if (APR_BUCKET_IS_METADATA(bucket)) {
      continue;
    }
    const char* data = nullptr;
    apr_size_t len = 0;
    apr_status_t rv = apr_bucket_read(bucket, &data, &len, APR_BLOCK_READ);
    if (rv != APR_SUCCESS) {
      log_action("ap_pass_brigade(READ_ERROR)");
      return rv;
    }
    bytes.append(data, len);
  }
  apr_brigade_cleanup(bb);
  log_action(StrCat("ap_pass_brigade(", bytes, ")"));
  return APR_SUCCESS;
}

ap_filter_rec_t* ap_register_output_filter(const char*, ap_out_filter_func,
                                           ap_init_filter_func,
                                           ap_filter_type) {
  log_fatal("ap_register_output_filter");
  return nullptr;
}

ap_filter_rec_t* ap_register_input_filter(const char*, ap_in_filter_func,
                                          ap_init_filter_func, ap_filter_type) {
  log_fatal("ap_register_input_filter");
  return nullptr;
}

// Note: Most Apache functions cannot be mocked with simple void stubs because
// they have specific return types. The tests that need these should link
// against the real Apache libraries or use more sophisticated mocking.

// Apache 2.4 defines macros like "#define ap_log_error ap_log_error_"
// so we need to provide the underscore-suffixed versions.
// These are variadic functions - we just ignore the arguments for testing.
void ap_log_error_(const char* file, int line, int module_index, int level,
                   apr_status_t status, const server_rec* s, const char* fmt,
                   ...) {
  // Do nothing - this is a mock for testing.
}

void ap_log_rerror_(const char* file, int line, int module_index, int level,
                    apr_status_t status, const request_rec* r, const char* fmt,
                    ...) {
  // Do nothing - this is a mock for testing.
}

void ap_log_perror_(const char* file, int line, int module_index, int level,
                    apr_status_t status, apr_pool_t* p, const char* fmt, ...) {
  // Do nothing - this is a mock for testing.
}

// Note: ap_log_error and ap_log_rerror are macros that expand to
// ap_log_error_ and ap_log_rerror_, which we implement above as variadic
// functions that do nothing for testing.

// We need to define the ap_unixd_config symbol to avoid link errors.
// Apache 2.4 renamed unixd_config to ap_unixd_config.
// This is a mock - tests don't actually use this structure.
unixd_config_rec ap_unixd_config = {
    nullptr,  // user_name
    nullptr,  // group_name
    0,        // user_id
    0,        // group_id
    0,        // suexec_enabled
    nullptr,  // chroot_dir
    nullptr   // suexec_disabled_reason
};

// Mock implementations of Apache functions needed by mod_pagespeed code.
// These allow the Apache tests to link without requiring the full Apache
// runtime libraries.

void ap_send_error_response(request_rec* r, int recursive_error) {
  // Do nothing - mock for testing.
}

apr_status_t ap_mpm_query(int query_code, int* result) {
  // Return a sensible default - not threaded, limit=1.
  if (result != nullptr) {
    *result = 1;
  }
  return APR_SUCCESS;
}

int ap_directory_walk(request_rec* r) {
  // Return APACHE_OK (0) to indicate success.
  return APACHE_OK;
}

char* ap_construct_url(apr_pool_t* p, const char* uri, request_rec* r) {
  // Return a simple mock URL.
  return apr_pstrdup(p, uri);
}

const char* ap_check_cmd_context(cmd_parms* cmd, unsigned forbidden) {
  // Return nullptr to indicate no error (context is allowed).
  return nullptr;
}

const char* ap_build_cont_config(apr_pool_t* p, apr_pool_t* temp_pool,
                                 cmd_parms* parms, ap_directive_t** current,
                                 ap_directive_t** curr_parent,
                                 char* orig_directive) {
  // Return nullptr to indicate no error.
  return nullptr;
}

// Hook registration functions - these are no-ops for testing.
// The hooks use callback function pointer types generated by Apache macros.
// We just ignore the registration in tests.

void ap_hook_handler(ap_HOOK_handler_t* pf, const char* const* aszPre,
                     const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_post_read_request(ap_HOOK_post_read_request_t* pf,
                               const char* const* aszPre,
                               const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_post_config(ap_HOOK_post_config_t* pf, const char* const* aszPre,
                         const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_child_init(ap_HOOK_child_init_t* pf, const char* const* aszPre,
                        const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_log_transaction(ap_HOOK_log_transaction_t* pf,
                             const char* const* aszPre,
                             const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_translate_name(ap_HOOK_translate_name_t* pf,
                            const char* const* aszPre,
                            const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_map_to_storage(ap_HOOK_map_to_storage_t* pf,
                            const char* const* aszPre,
                            const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

void ap_hook_optional_fn_retrieve(ap_HOOK_optional_fn_retrieve_t* pf,
                                  const char* const* aszPre,
                                  const char* const* aszSucc, int nOrder) {
  // Do nothing - mock for testing.
}

}  // extern "C"
