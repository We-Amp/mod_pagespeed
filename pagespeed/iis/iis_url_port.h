// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Drops the port from the authority of the absolute URL the IIS port builds
// for a request (HTTP.sys's full URL always names one), split out of
// iis_http_module.cpp so it can be unit-tested without IIS.
//
// An IPv6 literal host is bracketed and holds colons of its own
// ("http://[2001:db8::1]:80/"): only a colon after the closing bracket starts
// the port.  Colons in the path, query or fragment are never touched.

#ifndef PAGESPEED_IIS_IIS_URL_PORT_H_
#define PAGESPEED_IIS_IIS_URL_PORT_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// `url` with the ":port" of its authority removed; a URL without a port, or
// without a "scheme://" prefix, is returned unchanged.
GoogleString IisUrlWithoutPort(StringPiece url);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_URL_PORT_H_
