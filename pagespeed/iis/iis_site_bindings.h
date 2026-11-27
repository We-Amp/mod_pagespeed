// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The host names an IIS site's bindings vouch for, as the HTML cache engine
// reads them (ConfiguredHostNames, HostExactlyConfigured).
//
// A binding's information is IIS's "address:port:host".  Only an http or
// https binding with a host names one: a binding with an empty host ("*:80:"
// or an address only, "127.0.0.1:8205:") is reached by any Host a client
// sends, so it vouches for no name; a wildcard host ("*.example.com") names
// no one host either.  The port is not part of the name: the engine compares
// host names without their port.
//
// Free of the IIS headers so it is unit-tested on every platform; the port
// reads the bindings through the admin manager (iis_http_module.cpp).

#ifndef PAGESPEED_IIS_IIS_SITE_BINDINGS_H_
#define PAGESPEED_IIS_IIS_SITE_BINDINGS_H_

#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/serve_host_names.h"

namespace net_instaweb {

// The host one binding names, or false when it names none.
bool IisBindingHostName(StringPiece protocol, StringPiece binding_information,
                        GoogleString* host);

// (protocol, bindingInformation) pairs, in the site's order: the names they
// vouch for, the first as the primary, each once.
ConfiguredHostNames IisSiteHostNames(
    const std::vector<std::pair<GoogleString, GoogleString>>& bindings);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_SITE_BINDINGS_H_
