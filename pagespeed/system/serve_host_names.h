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

// The host a serve is recorded under in the optimizer's serve savings per
// host -- and so the one row a per-host admin console may see.

#ifndef PAGESPEED_SYSTEM_SERVE_HOST_NAMES_H_
#define PAGESPEED_SYSTEM_SERVE_HOST_NAMES_H_

#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The names a site's configuration lists, as the server port spells them:
// the primary name (Apache's ServerName, nginx's server_name) and every
// further name (Apache's exact ServerAlias names, every nginx server_name
// entry).  A port passes its names as configured -- wildcards, patterns and
// placeholders included -- and VouchedServeHost decides which are exact.
struct ConfiguredHostNames {
  GoogleString primary;
  std::vector<GoogleString> aliases;
};

// The optimizer's host-name rule for serve savings per host, applied the
// same way, so a name the module passes is never refused there: ASCII
// letters are lowercased; a ":port" of 1-5 digits is dropped (after the "]"
// of a bracketed IPv6 literal); one trailing "." is dropped.  What is left
// is a host name when it is 1-127 bytes with at least one letter or digit,
// made only of letters, digits, ".", "-" and "_", or a bracketed IPv6
// literal of hexadecimal digits, ":" and ".".  Anything else -- empty,
// longer, any other byte, an unbracketed IPv6 address, a name still ending
// in "." -- is not: returns false with `out` empty.
bool NormalizeServeHostName(StringPiece host, GoogleString* out);

// The host a serve -- and the per-host admin console of the site that
// served it -- is attributed to.  Never a value the visitor chose alone:
//  1. the request's host, normalised, when it equals one of the site's
//     exact configured names (the primary name or an exact alias);
//  2. otherwise the site's primary name, normalised, when it is usable (a
//     request that reached the site through a wildcard name, a pattern, or
//     because the site is the default one counts under the site that
//     served it);
//  3. otherwise "" -- the serve is recorded without a host.
// A configured name is exact, and a primary name usable, when it is a host
// name under the rule above and does not start with "." (nginx's
// ".example.com" also matches every subdomain).  Wildcards ("*"), patterns
// ("~..."), "_" and "" are never host names under that rule.
GoogleString VouchedServeHost(StringPiece request_host,
                              const ConfiguredHostNames& names);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SERVE_HOST_NAMES_H_
