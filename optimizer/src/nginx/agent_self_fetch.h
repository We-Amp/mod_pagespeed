// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#ifndef SRC_NGINX_AGENT_SELF_FETCH_H_
#define SRC_NGINX_AGENT_SELF_FETCH_H_

#include <span>
#include <string_view>

// With agent_optimize (or its llms.txt synthesis) on, the worker fetches the
// site's own pages and subresources with curl, pinned to the site's own
// address (src/browser/agent_fetcher.h, lib/net/curl_fetcher.h).  When the
// worker runs on the same host as the front end, such a fetch reaches the
// front end from loopback or from the very address it connects to, so an
// access rule that admits local clients (`allow 127.0.0.1`) would admit it,
// and the restricted content could reach a remote client through the agent
// markdown variant or /llms.txt.
//
// A front end therefore recognizes such a fetch (the worker's User-Agent
// marker AND a peer that is this server) and presents it to its access rules
// as kAgentSelfFetchLabel, a reserved address no client connects from.  A
// remote client that copies the User-Agent and connects directly keeps its
// own address: the peer test fails for it.
//
// Platform-neutral (addresses as raw bytes), so the decision is unit-tested
// without nginx and on every platform.  The nginx module applies it in
// POST_READ; the ASP.NET Core middleware carries the same rule in C#
// (samples/aspnetcore/src/WeAmp.PageSpeed.AspNetCore/Internal/AgentSelfFetch.cs).
//
// Limitations: a fetch that leaves from another of the host's addresses than
// the one it connects to (neither loopback nor the arrival address, as on a
// server reached on a secondary address with a different source address
// chosen by routing), or that reaches the front end through a proxy on
// another host, is not recognized and keeps its address.
// Behind a proxy on the same host every request arrives from this host, so
// the peer test cannot tell the worker's fetch from a proxied client;
// recognition there rests on the User-Agent alone.

namespace pagespeed {

// The token the worker's agent fetches carry in their User-Agent
// (CurlFetchOptions::user_agent, "Mozilla/5.0 (compatible; mod_pagespeed
// agent_optimize)").  Matched as a case-sensitive substring.
inline constexpr std::string_view kAgentFetchUserAgentMarker =
    "mod_pagespeed agent_optimize";

// The address a recognized agent fetch is given: 224.0.0.0, a reserved
// multicast address that is not loopback and is no server's own address.
inline constexpr std::string_view kAgentSelfFetchLabel = "224.0.0.0";

// The label's four address bytes, network order.
inline constexpr unsigned char kAgentSelfFetchLabelBytes[4] = {224, 0, 0, 0};

// Whether `user_agent` carries the worker's agent-fetch marker.
bool IsAgentFetchUserAgent(std::string_view user_agent);

// Whether `peer`, the address a connection came from, is this server: a
// loopback address (127.0.0.0/8, ::1, ::ffff:127.0.0.0/104), or the address
// the connection arrived on (`local`, compared by address only; an
// IPv4-mapped IPv6 address compares equal to its IPv4 form).  Addresses are
// raw bytes in network order: 4 bytes for IPv4, 16 for IPv6; any other
// length is no address.  An empty `local` leaves only the loopback test; an
// empty `peer` is never this server.
bool AgentFetchPeerIsThisServer(std::span<const unsigned char> peer,
                                std::span<const unsigned char> local);

// Both conditions: the request is the worker's agent fetch arriving from
// this server, and is to be presented to access rules as
// kAgentSelfFetchLabel.
bool IsAgentSelfFetch(std::string_view user_agent,
                      std::span<const unsigned char> peer,
                      std::span<const unsigned char> local);

}  // namespace pagespeed

#endif  // SRC_NGINX_AGENT_SELF_FETCH_H_
