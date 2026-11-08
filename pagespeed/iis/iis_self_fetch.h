// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The module fetches resources of its own site from this same server, from
// the address the original request arrived on (or over loopback), with
// WinHTTP.  Such a fetch enters IIS from one of the server's own addresses,
// so an IP and Domain Restrictions rule that admits loopback or the server's
// address admits it whoever the original client was.  This file decides
// whether a request is such a fetch and, if so, rewrites the request's client
// address to a reserved address no client connects from, before IIS's own
// modules read it.  The decision is unit-tested without IIS.
//
// Known gap: a fetch that leaves from another of the host's addresses than
// the one it connects to (neither loopback nor the arrival address, as on a
// server reached on a secondary address with a different source address
// chosen by routing) is not recognized.

#ifndef PAGESPEED_IIS_IIS_SELF_FETCH_H_
#define PAGESPEED_IIS_IIS_SELF_FETCH_H_

// Keep <windows.h> from pulling the legacy <winsock.h> in ahead of
// <winsock2.h>; <winsock2.h> must precede <ws2tcpip.h>.
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The address a self-fetch is given: 224.0.0.0, a reserved multicast address
// that is not loopback and is no server's own address.
extern const char kIisSelfFetchClientIp[];

// Whether `peer`, the address a connection came from, is this server: a
// loopback address (127.0.0.0/8, ::1, or ::ffff:127.x.y.z), or the very
// address the connection arrived on (`local`, compared by address, not port;
// an IPv4 address and its IPv4-mapped IPv6 form are the same address).  A
// null `local` leaves only the loopback test; a null `peer` is never this
// server.
bool IisSelfFetchPeerIsThisServer(const SOCKADDR* peer, const SOCKADDR* local);

// Whether a request with this User-Agent, peer and local address is the
// module's own fetch: the User-Agent carries the module's fetch marker AND
// the peer is this server.  A request from any other address is not, whatever
// its User-Agent, so the marker cannot be used to step around a rule that
// names a client's address.
bool IisIsSelfFetch(StringPiece user_agent, const SOCKADDR* peer,
                    const SOCKADDR* local);

// Overwrites the client address `remote` in place with 224.0.0.0, port 0,
// as an IPv4 address.  `remote` must be an AF_INET or AF_INET6 address (the
// storage of either holds a SOCKADDR_IN); returns false and leaves it alone
// otherwise.
bool IisRelabelSelfFetch(SOCKADDR* remote);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_SELF_FETCH_H_
