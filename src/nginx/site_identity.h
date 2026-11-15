// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_SITE_IDENTITY_H_
#define PAGESPEED_SRC_NGINX_SITE_IDENTITY_H_

// The site of an nginx server block: the cache keyspace the block reads and
// records in.  Pure C++ (no nginx headers) so it is unit-testable without an
// nginx build; the module collects the inputs at configuration time.
//
// Each server block keeps its own cache entries: every entry the module
// records, and every notification it sends, carries the site of the server
// block that received the request, and the request's host name selects an
// entry among that block's entries.  Blocks do not share entries, also when
// they list the same names or use the same cache path.

#include <string>
#include <vector>

namespace pagespeed {

// The site of a server block: 64 lowercase hex characters (the form the
// worker protocol and PageSpeedCache take as a site), derived from the
// block's server names and the addresses it listens on.
//
// `names` are the block's server names as nginx stores them, with a leading
// "~" on a regular-expression name.  Other names are compared without case.
// `listen_addresses` are the block's listening addresses in nginx's text
// form with the port ("0.0.0.0:80", "[::]:443", "unix:/run/x.sock").  Order
// and repeats do not matter, so the value is the same in every worker
// process and across reloads for as long as the block's names and listeners
// stay the same; adding or removing either gives the block a new, empty
// keyspace.
std::string DeriveServerBlockSite(std::vector<std::string> names,
                                  std::vector<std::string> listen_addresses);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_SITE_IDENTITY_H_
