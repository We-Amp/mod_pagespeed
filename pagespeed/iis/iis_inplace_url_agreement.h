// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The IIS port's reading of the in-place URL agreement
// (pagespeed/system/inplace_url_agreement.h).  IIS routes a request, matches
// its <location> sections and applies request filtering on the decoded path
// HTTP.sys hands it (HTTP_REQUEST::CookedUrl.pAbsPath); the module keys the
// in-place cache on its own parse of the full URL.  In-place optimization
// acts only when the two name the same path.  Free of the IIS headers so the
// decision can be unit-tested on any platform.

#ifndef PAGESPEED_IIS_IIS_INPLACE_URL_AGREEMENT_H_
#define PAGESPEED_IIS_IIS_INPLACE_URL_AGREEMENT_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// True when IIS and the module read the request's path alike.  `iis_path` is
// the decoded path IIS routes on, as UTF-8, without query.  `module_path` is
// the path of the URL the module parsed, in its canonical percent-encoded
// form and without query.
//
// IIS compares paths without regard to case (its <location> matching, request
// filtering and the file system all do), so ASCII letters are compared
// without regard to case; any other byte must match exactly.  IIS does not
// merge runs of slashes, so neither does the comparison.
bool IisInPlaceUrlAgrees(StringPiece iis_path, StringPiece module_path);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_INPLACE_URL_AGREEMENT_H_
