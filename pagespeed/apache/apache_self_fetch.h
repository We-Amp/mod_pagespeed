// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The module fetches resources of its own site from this same server, from
// the address the original request arrived on (or over loopback).  Such a
// fetch reaches the server from one of the server's own addresses, which an
// address-based access rule such as "Require local" admits whoever the
// original client was.  This file decides whether a request is such a fetch
// and, if so, makes it present as a remote client before any access or
// authorization hook runs.  Pulled out of mod_instaweb.cc so the decision can
// be unit-tested without a live httpd.
//
// Known gap: a fetch that leaves from another of the host's addresses than
// the one it connects to (neither loopback nor the arrival address, as on a
// server reached on a secondary address with a different source address
// chosen by routing) is not recognized.

#ifndef PAGESPEED_APACHE_APACHE_SELF_FETCH_H_
#define PAGESPEED_APACHE_APACHE_SELF_FETCH_H_

#include "apr_network_io.h"                          // NOLINT
#include "pagespeed/apache/apache_httpd_includes.h"  // NOLINT

namespace net_instaweb {

// The address a self-fetch is given: a reserved multicast address no client
// connects from, so it matches no "local", loopback or own-address rule.
extern const char kApacheSelfFetchClientIp[];

// Whether `peer`, the address a connection came from, is this server: a
// loopback address, or the very address the connection arrived on
// (`local`, compared by address, not port).  Those are the addresses
// "Require local" admits.  A null `local` leaves only the loopback test.
bool ApacheSelfFetchPeerIsThisServer(const apr_sockaddr_t* peer,
                                     const apr_sockaddr_t* local);

// When `r` carries the module's fetch marker in its User-Agent and comes
// from this server (see above), relabels the request's client address to
// kApacheSelfFetchClientIp and clears the remote host name, so the access
// and authorization hooks treat it as a remote client.  Returns whether it
// relabeled.  A request from any other address is left alone, whatever its
// User-Agent, so the marker cannot be used to step around a rule that names
// a client's address.
bool ApacheTreatSelfFetchAsRemote(request_rec* r);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_SELF_FETCH_H_
