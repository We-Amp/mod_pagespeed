// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_inplace_url_agreement.h"

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/system/inplace_url_agreement.h"

namespace net_instaweb {

bool IisInPlaceUrlAgrees(StringPiece iis_path, StringPiece module_path) {
  // Case is folded on the decoded paths, so a letter the module keeps
  // escaped compares like the letter itself.  The module's folded path is
  // handed to the shared comparison with only its percent signs escaped,
  // which the comparison decodes back to exactly that path.  The host takes
  // no part: IIS chose the site, and the module's URL carries the host of
  // the same request.
  GoogleString iis = iis_path.as_string();
  LowerString(&iis);
  GoogleString decoded = GoogleUrl::UnescapeIgnorePlus(module_path);
  LowerString(&decoded);
  GoogleString module;
  module.reserve(decoded.size());
  for (const char c : decoded) {
    if (c == '%') {
      module.append("%25");
    } else {
      module.push_back(c);
    }
  }
  return InPlaceUrlAgrees(iis, "", module, "", false /* merge_slashes */);
}

}  // namespace net_instaweb
