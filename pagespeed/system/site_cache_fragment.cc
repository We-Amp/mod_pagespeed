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

#include "pagespeed/system/site_cache_fragment.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cstdint>

namespace net_instaweb {

namespace {

const char kSiteFragmentPrefix[] = "site:";
// 128 bits of the digest are plenty for a key prefix.
const size_t kSiteFragmentDigestHexDigits = 32;

GoogleString Sha256Hex(StringPiece data) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
  static const char kHex[] = "0123456789abcdef";
  GoogleString hex;
  hex.reserve(size_t{2} * SHA256_DIGEST_LENGTH);
  for (uint8_t byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0xf]);
  }
  return hex;
}

void SortAndDedupe(std::vector<GoogleString>* values) {
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

// Each value carries its length, so no two identities read the same.
void AppendSection(const char* tag, const std::vector<GoogleString>& values,
                   GoogleString* out) {
  for (const GoogleString& value : values) {
    StrAppend(out, tag, IntegerToString(static_cast<int>(value.size())), ":",
              value, "\n");
  }
}

}  // namespace

GoogleString DeriveSiteCacheFragment(const SiteIdentity& identity) {
  std::vector<GoogleString> names = identity.names;
  for (GoogleString& name : names) {
    if (name.empty() || name[0] != '~') {
      LowerString(&name);
    }
  }
  SortAndDedupe(&names);
  std::vector<GoogleString> addresses = identity.listen_addresses;
  SortAndDedupe(&addresses);

  GoogleString input = "site-v1\n";
  AppendSection("n", names, &input);
  AppendSection("a", addresses, &input);
  return StrCat(kSiteFragmentPrefix,
                Sha256Hex(input).substr(0, kSiteFragmentDigestHexDigits));
}

GoogleString SiteIdFromServerFragment(const GoogleString& server_fragment) {
  return server_fragment.empty() ? GoogleString() : Sha256Hex(server_fragment);
}

bool IsSiteId(StringPiece site) {
  if (site.size() != size_t{2} * SHA256_DIGEST_LENGTH) {
    return false;
  }
  for (char c : site) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

GoogleString DeriveIisSiteCacheFragment(uint32_t site_id,
                                        StringPiece site_name_utf8) {
  GoogleString input = "iis-site-v1\n";
  StrAppend(&input, Integer64ToString(static_cast<int64>(site_id)), "\n",
            site_name_utf8);
  return StrCat(kSiteFragmentPrefix,
                Sha256Hex(input).substr(0, kSiteFragmentDigestHexDigits));
}

GoogleString IisRequestSiteId(uint32_t request_site_id,
                              uint32_t context_site_id,
                              const GoogleString& server_fragment) {
  if (request_site_id != context_site_id) {
    return GoogleString();
  }
  return SiteIdFromServerFragment(server_fragment);
}

}  // namespace net_instaweb
