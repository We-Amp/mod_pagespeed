// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The module fetches resources of its own site from this same server, from
// the address the original request arrived on (or over loopback).  Such a
// fetch reaches nginx from one of the server's own addresses, which an
// address-based access rule such as `allow 127.0.0.1` admits whoever the
// original client was.  A recognized self-fetch is therefore relabeled, in
// the POST_READ phase and so before any access rule runs, to a reserved
// address no client connects from; the original address is put back when
// the request ends.  The relabel and the restore work on any connection type
// with nginx's field names, so they are unit-tested without nginx.
//
// Known gap: a fetch that leaves from another of the host's addresses than
// the one it connects to (neither loopback nor the arrival address, as on a
// server reached on a secondary address with a different source address
// chosen by routing) is not recognized.

#ifndef PAGESPEED_NGINX_NGX_SELF_FETCH_LABEL_H_
#define PAGESPEED_NGINX_NGX_SELF_FETCH_LABEL_H_

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>

namespace net_instaweb {

// The address a self-fetch is given: 224.0.0.0, a reserved multicast address
// that is not loopback and is no server's own address.
extern const char kNgxSelfFetchLabelText[];

// Whether `peer`, the address a connection came from, is this server: a
// loopback address, or the very address the connection arrived on
// (`local`, compared by address, not port).  A null `local` leaves only the
// loopback test; a null `peer` is never this server.
bool NgxSelfFetchPeerIsThisServer(const struct sockaddr* peer,
                                  const struct sockaddr* local);

// Storage for the label, owned by whoever owns the request (the request
// pool, in the module), so it outlives every use of the relabeled address.
struct NgxSelfFetchLabel {
  struct sockaddr_in addr;
  unsigned char text[sizeof("224.0.0.0")];
};

// The connection fields the relabel replaces, saved for the restore.
template <class Connection>
struct NgxSelfFetchSaved {
  Connection* connection;
  struct sockaddr* sockaddr;
  socklen_t socklen;
  size_t addr_text_len;
  unsigned char* addr_text_data;
};

// Fills `label` with 224.0.0.0, port 0.
void NgxInitSelfFetchLabel(NgxSelfFetchLabel* label);

// Saves `c`'s client address (sockaddr, socklen, addr_text) into `saved` and
// points the connection at `label` instead, so `allow`/`deny`, $remote_addr
// and every other reading of the client address see 224.0.0.0.
template <class Connection>
void NgxRelabelSelfFetch(Connection* c, NgxSelfFetchLabel* label,
                         NgxSelfFetchSaved<Connection>* saved) {
  saved->connection = c;
  saved->sockaddr = c->sockaddr;
  saved->socklen = c->socklen;
  saved->addr_text_len = c->addr_text.len;
  saved->addr_text_data = c->addr_text.data;
  NgxInitSelfFetchLabel(label);
  c->sockaddr = reinterpret_cast<struct sockaddr*>(&label->addr);
  c->socklen = sizeof(label->addr);
  c->addr_text.data = label->text;
  c->addr_text.len = sizeof(label->text) - 1;
}

// Puts back what NgxRelabelSelfFetch saved, so a keep-alive connection's next
// request starts from its real client address.
template <class Connection>
void NgxRestoreSelfFetch(const NgxSelfFetchSaved<Connection>& saved) {
  Connection* c = saved.connection;
  c->sockaddr = saved.sockaddr;
  c->socklen = saved.socklen;
  c->addr_text.len = saved.addr_text_len;
  c->addr_text.data = saved.addr_text_data;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_SELF_FETCH_LABEL_H_
