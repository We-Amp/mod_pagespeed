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

#include "net/instaweb/rewriter/public/option_context.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

// SHA-256 from BoringSSL, added here as a NEW and deliberate dependency of
// this library rather than picked up from something that already had it.  Two
// things are worth stating plainly because both are easy to get wrong later.
//
// It is a new third-party edge, including into the Windows/IIS DLL, which this
// repository otherwise avoids: //pagespeed/kernel/base does not depend on
// BoringSSL, and the one existing user of it there is compiled out on Windows
// by default.  That cost is accepted knowingly.
//
// What it buys is that the dependency is UNCONDITIONAL.  The obvious
// alternative, SHA1Signature, is compiled out entirely when
// ENABLE_URL_SIGNATURES is 0 — which the Windows developer configuration sets
// — and returns a run of zero bytes when it is.  A signature that silently
// became a constant on one platform would collapse every option context into
// one, which is the single worst failure this file could have, and it would do
// it without a diagnostic anywhere.  A build-time dependency is a cheap price
// for that not being possible.
#include <openssl/sha.h>

namespace net_instaweb {

const char kOptionContextFormatVersion[] = "psoc1";

namespace {

// Record tags.  One byte each, and their relative order in ASCII is what puts
// filters ahead of options in the sorted payload: 'f' < 'o' < 'r'.
const char kFilterTag = 'f';
const char kOptionTag = 'o';    // value carried verbatim
const char kRedactedTag = 'r';  // value replaced by a hash of itself

const char kHexDigits[] = "0123456789abcdef";

void AppendHexByte(uint8_t byte, GoogleString* out) {
  out->push_back(kHexDigits[byte >> 4]);
  out->push_back(kHexDigits[byte & 0x0F]);
}

GoogleString ToHex(const uint8_t* bytes, size_t len) {
  GoogleString out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    AppendHexByte(bytes[i], &out);
  }
  return out;
}

// Escapes a value so that a record is one line and splits unambiguously.
//
// Two rules, and between them the mapping is total and injective: a backslash
// becomes two, and every byte outside printable ASCII becomes \xHH.  Nothing
// else is touched, so ordinary values stay readable — which matters, because a
// payload is a thing an operator may end up looking at.  '=' needs no escape:
// a record splits at its FIRST '=', and keys are checked to contain none.
void AppendEscaped(StringPiece value, GoogleString* out) {
  for (size_t i = 0; i < value.size(); ++i) {
    const auto byte = static_cast<uint8_t>(value[i]);
    if (byte == '\\') {
      out->append("\\\\");
    } else if (byte < 0x20 || byte > 0x7E) {
      out->append("\\x");
      AppendHexByte(byte, out);
    } else {
      out->push_back(static_cast<char>(byte));
    }
  }
}

// A key must be non-empty, printable, and free of the one byte the record
// format gives meaning to.  Everything registered today satisfies this;
// EveryRegisteredOptionHasASerializableKey is the test that keeps it true.
bool KeyIsSerializable(StringPiece key) {
  if (key.empty()) {
    return false;
  }
  for (size_t i = 0; i < key.size(); ++i) {
    const auto byte = static_cast<uint8_t>(key[i]);
    if (byte <= 0x20 || byte >= 0x7F || byte == '=') {
      return false;
    }
  }
  return true;
}

GoogleString Sha256Hex(StringPiece data) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
  return ToHex(digest, SHA256_DIGEST_LENGTH);
}

}  // namespace

OptionContextStatus OptionContext::Serialize(const RewriteOptions& options,
                                             GoogleString* payload) {
  payload->clear();

  std::vector<GoogleString> records;

  // Enabled filters.
  //
  // Emitted by their two-letter id, never by their enum value: the enum is
  // renumbered whenever a filter is inserted, and the id is not.  Sorting the
  // ids afterwards drops the last bit of enum-order dependence that the
  // existing signature still carries — there, inserting a filter mid-enum
  // reorders the string even though the token set is unchanged.
  //
  // kDebug is excluded, matching the existing signature.  Debug output is a
  // diagnostic overlay rather than a different optimization of the resource,
  // and letting it split the cache would double every context the moment
  // someone turns it on to look at something.
  for (int i = RewriteOptions::kFirstFilter; i != RewriteOptions::kEndOfFilters;
       ++i) {
    const auto filter = static_cast<RewriteOptions::Filter>(i);
    if (filter == RewriteOptions::kDebug || !options.Enabled(filter)) {
      continue;
    }
    StringPiece id(RewriteOptions::FilterId(filter));
    if (!KeyIsSerializable(id)) {
      return OptionContextStatus::kUnserializable;
    }
    GoogleString record(1, kFilterTag);
    StrAppend(&record, ":", id);
    records.push_back(record);
  }

  // Options.
  //
  // Same predicate the existing signature uses — participates in signature
  // computation AND was explicitly set — for the reason given in the header:
  // it is what makes a payload survive a release that adds options.
  const RewriteOptions::OptionBaseVector& all = options.all_options();
  for (size_t i = 0; i < all.size(); ++i) {
    const RewriteOptions::OptionBase* option = all[i];
    if (!option->is_used_for_signature_computation() || !option->was_set()) {
      continue;
    }

    // Prefer the configuration name; fall back to the short id for the
    // request-scoped options registered without one.  The '@' marks which
    // namespace a key came from, so the two can never collide: no
    // configuration name begins with '@'.
    StringPiece name = option->option_name();
    GoogleString key;
    if (name.empty()) {
      key = StrCat("@", option->id());
    } else {
      name.CopyToString(&key);
    }
    if (!KeyIsSerializable(key)) {
      return OptionContextStatus::kUnserializable;
    }

    const bool safe = option->property()->safe_to_print();
    GoogleString record(1, safe ? kOptionTag : kRedactedTag);
    StrAppend(&record, ":", key, "=");
    if (safe) {
      AppendEscaped(option->ToString(), &record);
    } else {
      // The value is withheld and its hash stands in.  Separation is
      // preserved — a different value still produces a different record and
      // therefore a different signature — while the value itself never
      // reaches the bytes.  This is the arm that keeps a proxy password out of
      // anything derived from a configuration.
      record.append(Sha256Hex(option->ToString()));
    }
    records.push_back(record);
  }

  // The sort IS the canonicalization.  Everything above may run in any order,
  // and two RewriteOptions that resolved to the same values through different
  // paths converge here.
  std::sort(records.begin(), records.end());

  size_t total = strlen(kOptionContextFormatVersion) + 1;
  for (size_t i = 0; i < records.size(); ++i) {
    total += records[i].size() + 1;
  }
  // Checked BEFORE the payload is assembled, so an oversized context costs a
  // size computation rather than a 16 KB string nobody will use.
  if (total > kMaxOptionContextBytes) {
    return OptionContextStatus::kTooLarge;
  }

  payload->reserve(total);
  payload->append(kOptionContextFormatVersion);
  payload->push_back('\n');
  for (size_t i = 0; i < records.size(); ++i) {
    payload->append(records[i]);
    payload->push_back('\n');
  }
  return OptionContextStatus::kOk;
}

GoogleString OptionContext::Signature(StringPiece payload) {
  return Sha256Hex(payload);
}

OptionContextStatus OptionContext::Compute(const RewriteOptions& options,
                                           GoogleString* payload,
                                           GoogleString* signature) {
  signature->clear();
  const OptionContextStatus status = Serialize(options, payload);
  if (status != OptionContextStatus::kOk) {
    payload->clear();
    return status;
  }
  *signature = Signature(*payload);
  return status;
}

GoogleString OptionContext::DefaultSignature() {
  GoogleString empty_payload(kOptionContextFormatVersion);
  empty_payload.push_back('\n');
  return Signature(empty_payload);
}

}  // namespace net_instaweb
