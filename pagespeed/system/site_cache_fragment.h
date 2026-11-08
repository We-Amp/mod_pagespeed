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

// The cache keyspace of a site: the nginx server block or Apache virtual host
// that received a request.  A site with no CacheFragment of its own gets one
// derived from its configuration here, at configuration time, so what it
// shares its cache entries with never depends on anything a request says.

#ifndef PAGESPEED_SYSTEM_SITE_CACHE_FRAGMENT_H_
#define PAGESPEED_SYSTEM_SITE_CACHE_FRAGMENT_H_

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// What a site is made of, as the port reads it from its configuration.
// Order, repeats and the case of names do not matter.  An entry starting with
// "~" is a pattern and is kept as written; every other name is lowercased.
// Addresses are text in the form "1.2.3.4:80", "[::1]:80" or "unix:/path".
struct SiteIdentity {
  std::vector<GoogleString> names;
  std::vector<GoogleString> listen_addresses;
};

// The fragment for a site without one of its own: "site:" and 32 lowercase
// hex digits of a SHA-256 over the normalized identity.  The ":" makes it a
// value the CacheFragment directive refuses, so a site configured by hand can
// never name it.  Set it with RewriteOptions::set_cache_fragment.
GoogleString DeriveSiteCacheFragment(const SiteIdentity& identity);

// The name of a site's keyspace outside the classic cache: the SHA-256 of its
// server-scope fragment, as 64 lowercase hex digits; "" when it has none.
// The server-scope fragment is the one of the server block or server record
// that received the request (nginx: its server context's global options;
// Apache: the record's derived value where it has one, else its server
// context's global configuration), never one from a request's options: those
// also hold per-directory settings, which do not choose the site.
GoogleString SiteIdFromServerFragment(const GoogleString& server_fragment);

// Whether `site` has the form SiteIdFromServerFragment gives a site with a
// fragment: exactly 64 lowercase hex digits.  The optimizer-worker arms send
// nothing, read nothing and write nothing for a request without one.
bool IsSiteId(StringPiece site);

// The fragment for an IIS site without one of its own: "site:" and 32
// lowercase hex digits of a SHA-256 over the IIS site id and the site name,
// given as UTF-8 and taken byte for byte.  The module does not fold the
// name's case; IIS supplies it upper-cased.  Every application of one site
// shares the value; a site that is renamed, or a site id that is reused under
// another name, gets a different one and starts cold.
GoogleString DeriveIisSiteCacheFragment(uint32_t site_id,
                                        StringPiece site_name_utf8);

// The site an IIS request's optimizer-worker arms use: the site of its server
// context's fragment when the request's IIS site id is the one that context
// was created for, else "", so both arms decline the request.
GoogleString IisRequestSiteId(uint32_t request_site_id,
                              uint32_t context_site_id,
                              const GoogleString& server_fragment);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SITE_CACHE_FRAGMENT_H_
