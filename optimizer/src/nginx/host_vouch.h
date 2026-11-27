// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_HOST_VOUCH_H_
#define PAGESPEED_SRC_NGINX_HOST_VOUCH_H_

// Which request hostnames a server block stands behind.  Pure C++ (no nginx
// headers) so it is unit-testable without an nginx build; the module adds
// the regex server names, which need nginx's own matcher.
//
// The worker fetches from the hostname the module hands it (the /llms.txt
// build, the agent render).  A hostname is vouched for when the block that
// received the request lists it as one of its server names, or when the
// operator's host alias table names it.

#include <string>
#include <string_view>
#include <unordered_map>

namespace pagespeed {

// The host part of a request authority in the form nginx compares with a
// server name: ASCII-lowercased, without the port and without one trailing
// dot.  A bracketed IPv6 literal keeps its brackets.
std::string AuthorityHost(std::string_view authority);

// True when `server_name`, one non-regex server name as nginx stores it,
// names `host` (an AuthorityHost() result).  Exact names and nginx's three
// wildcard forms: "*.example.com" (any name below example.com),
// ".example.com" (example.com itself and any name below it) and
// "www.example.*" (any name that starts with "www.example.").  An empty name
// names nothing.
bool ServerNameMatchesHost(std::string_view server_name, std::string_view host);

// True when `hostname`, in the module's normalized cache form, appears in the
// operator's host alias table, either as an alias or as the name an alias
// maps to.
bool HostAliasesName(
    const std::unordered_map<std::string, std::string>& host_aliases,
    std::string_view hostname);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_HOST_VOUCH_H_
