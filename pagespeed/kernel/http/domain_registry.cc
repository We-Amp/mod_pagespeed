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

#include "pagespeed/kernel/http/domain_registry.h"

#include <cstddef>  // for size_t

#include "external/libpsl/include/libpsl.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

// This wrapper was historically backed by the (dead, Apache-incubator)
// domain_registry_provider ("drp") library and its C entry point
// GetRegistryLength(). drp has no maintained upstream and shipped a
// hand-generated public-suffix table. We now back it on libpsl, which is
// maintained and built from the same Mozilla Public Suffix List dataset.
//
// To keep behavior byte-identical we reproduce drp's GetRegistryLength()
// contract exactly (see RegistryLength below) rather than calling libpsl's
// higher-level psl_registrable_domain(), which differs from drp on
// unlisted/unknown TLDs (libpsl applies the implicit "*" prevailing rule;
// drp does not). The downstream arithmetic that turns a public-suffix byte
// length into the minimal private suffix is unchanged from the drp era.

namespace net_instaweb {
namespace domain_registry {

namespace {

// Process-wide handle to the libpsl builtin (compiled-in) PSL data. The
// builtin list is immutable and read-only, so a single shared const pointer
// is safe to share across threads. Initialized by Init().
const psl_ctx_t* g_psl = nullptr;

// A candidate suffix is well-formed iff it is non-empty and has no empty
// labels, allowing EXACTLY ONE optional trailing dot (FQDN form). This rejects
// "", ".", "..", a leading dot, interior "..", and a double trailing dot ---
// reproducing drp's requirement that every component be non-empty (so inputs
// like ".." and "www.google.com.." fail secure to the whole string).
bool ValidSuffixCandidate(StringPiece c) {
  if (c.empty()) {
    return false;
  }
  if (c[c.size() - 1] == '.') {  // strip exactly one trailing dot
    c = c.substr(0, c.size() - 1);
  }
  if (c.empty()) {  // was just "."
    return false;
  }
  if (c[0] == '.') {  // leading empty label
    return false;
  }
  if (c[c.size() - 1] == '.') {  // a SECOND trailing dot => empty label
    return false;
  }
  for (size_t i = 1; i < c.size(); ++i) {
    if (c[i] == '.' && c[i - 1] == '.') {  // interior empty label
      return false;
    }
  }
  return true;
}

// Reproduces drp's GetRegistryLength(hostname): the byte length, measured
// from the END of `hostname`, of the longest registry / public-suffix
// portion. Contract preserved from drp:
//   * matching is case-insensitive,
//   * a single trailing dot (FQDN form) is counted as part of the suffix,
//   * an UNKNOWN / unlisted TLD yields 0 (no implicit "*" prevailing rule),
//     so the caller "fails secure" by treating the whole hostname as private.
// Returns 0 when no public suffix matches (drp's "unrecognized" signal).
size_t RegistryLength(StringPiece hostname) {
  if (g_psl == nullptr || hostname.empty()) {
    return 0;
  }

  // libpsl matches case-insensitively but expects the caller to supply the
  // candidate; lower-casing a local copy mirrors drp's internal behavior and
  // keeps mixed-case Host headers / hrefs consistent.
  GoogleString lowered;
  lowered.reserve(hostname.size());
  for (char c : hostname) {
    lowered.push_back(LowerChar(c));
  }
  StringPiece lower(lowered);

  // The public suffix is one of the dot-delimited tails of the hostname
  // (including the whole hostname). Test each candidate against the builtin
  // PSL with the "*" prevailing rule DISABLED (PSL_TYPE_NO_STAR_RULE) so that
  // an unlisted TLD is reported as not-a-public-suffix, matching drp. Take the
  // LONGEST matching candidate; its byte length from the end is the value drp
  // returned (a single trailing dot, if present, is part of that candidate and
  // is counted, matching drp's FQDN handling).
  const int kFlags = PSL_TYPE_ICANN | PSL_TYPE_PRIVATE | PSL_TYPE_NO_STAR_RULE;

  // Iterate candidate start offsets from leftmost (longest candidate) to
  // rightmost so the first match is the longest registry suffix.
  for (size_t start = 0; start < lower.size(); ++start) {
    // A candidate begins at offset 0 (the whole string) or immediately after
    // a dot. Skip offsets that are not a label boundary.
    if (start != 0 && lower[start - 1] != '.') {
      continue;
    }
    StringPiece candidate = lower.substr(start);
    if (!ValidSuffixCandidate(candidate)) {
      continue;
    }
    // libpsl treats a trailing-dot single label ("foo.") as a public suffix
    // even under NO_STAR_RULE, whereas drp did not. Strip one trailing dot
    // before the lookup so unknown TLDs are correctly rejected; the dot is
    // still counted in the returned length (drp counted it).
    StringPiece query = candidate;
    if (!query.empty() && query[query.size() - 1] == '.') {
      query = query.substr(0, query.size() - 1);
    }
    // Build a NUL-terminated copy for the C API.
    GoogleString query_cstr(query.data(), query.size());
    if (psl_is_public_suffix2(g_psl, query_cstr.c_str(), kFlags) == 1) {
      return hostname.size() - start;
    }
  }
  return 0;
}

}  // namespace

void Init() {
  // psl_builtin() returns a const, process-lifetime pointer to the compiled-in
  // PSL data (or nullptr if libpsl was built without builtin data). It is
  // idempotent and need not be freed.
  if (g_psl == nullptr) {
    g_psl = psl_builtin();
  }
}

StringPiece MinimalPrivateSuffix(StringPiece hostname) {
  if (hostname.empty()) {
    return "";
  }

  size_t length_of_public_suffix = RegistryLength(hostname);
  if (length_of_public_suffix == 0) {
    // Unrecognized top level domain.  We don't know what kind of multi-level
    // public suffixes they might have created, so be on the safe side and
    // treat the entire hostname as a private suffix.
    return hostname;
  }

  stringpiece_ssize_type last_dot_before_private_suffix =
      hostname.rfind('.', hostname.size() - length_of_public_suffix -
                              1 /* don't include the dot */
                              - 1 /* pos is inclusive */);
  if (last_dot_before_private_suffix == StringPiece::npos) {
    // Hostname is already a minimal private suffix.
    last_dot_before_private_suffix = 0;
  } else {
    last_dot_before_private_suffix++;  // Don't include the dot.
  }
  return hostname.substr(last_dot_before_private_suffix);
}

}  // namespace domain_registry
}  // namespace net_instaweb
