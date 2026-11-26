// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/apache/apache_conditional_stash.h"

#include "apr_pools.h"   // NOLINT
#include "apr_tables.h"  // NOLINT
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"

namespace net_instaweb {

namespace {

// The pool user-data key of the stash.  A pool's user data is not reachable
// from a client.
const char kStashKey[] = "mod_pagespeed_client_conditional_stash";

int CollectLine(void* rec, const char* /*key*/, const char* value) {
  static_cast<StringPieceVector*>(rec)->push_back(
      StringPiece(value == nullptr ? "" : value));
  return 1;  // keep going
}

StringPieceVector Lines(apr_table_t* table, const char* name) {
  StringPieceVector lines;
  apr_table_do(CollectLine, &lines, table, name, nullptr);
  return lines;
}

apr_status_t DeleteStash(void* data) {
  delete static_cast<ClientConditionalStash*>(data);
  return APR_SUCCESS;
}

// The request the client sent: subrequests and internal redirects lead back
// to it.  An internal redirect shares its request's pool; a subrequest has a
// pool of its own.
const request_rec* ClientRequest(const request_rec* r) {
  while (r->main != nullptr || r->prev != nullptr) {
    r = r->main != nullptr ? r->main : r->prev;
  }
  return r;
}

ClientConditionalStash* FindStash(const request_rec* r) {
  void* data = nullptr;
  if (apr_pool_userdata_get(&data, kStashKey, ClientRequest(r)->pool) !=
      APR_SUCCESS) {
    return nullptr;
  }
  return static_cast<ClientConditionalStash*>(data);
}

}  // namespace

void ApacheStashAndStripClientConditionals(request_rec* r) {
  if (r == nullptr || r->headers_in == nullptr) {
    return;
  }
  ClientConditionalStash stash;
  const bool strip = StashClientConditionals(
      Lines(r->headers_in, HttpAttributes::kIfNoneMatch),
      Lines(r->headers_in, HttpAttributes::kIfModifiedSince), &stash);
  if (strip) {
    apr_table_unset(r->headers_in, HttpAttributes::kIfNoneMatch);
    apr_table_unset(r->headers_in, HttpAttributes::kIfModifiedSince);
  }
  ClientConditionalStash* kept = FindStash(r);
  if (kept != nullptr) {
    // A later pass (an internal redirect) sees the fields as left by the
    // first: the first stash already holds them as they arrived.
    kept->stripped = kept->stripped || strip;
    return;
  }
  apr_pool_t* pool = ClientRequest(r)->pool;
  ClientConditionalStash* owned = new ClientConditionalStash(stash);
  if (apr_pool_userdata_setn(owned, kStashKey, DeleteStash, pool) !=
      APR_SUCCESS) {
    delete owned;
  }
}

const ClientConditionalStash* ApacheClientConditionalStash(
    const request_rec* r) {
  return r == nullptr ? nullptr : FindStash(r);
}

}  // namespace net_instaweb
