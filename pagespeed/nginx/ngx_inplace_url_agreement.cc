// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_inplace_url_agreement.h"

#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"

namespace net_instaweb {

bool NgxInPlaceUrlAgrees(StringPiece server_uri, StringPiece server_host,
                         StringPiece module_path, StringPiece module_host,
                         bool merge_slashes) {
  if (!StringCaseEqual(server_host, module_host)) {
    return false;
  }
  // The server's path is percent-decoded, the module's canonical path keeps
  // its escapes.  Decoding the escapes of the module's path puts the two in
  // the same form; the one further step the server takes that the module
  // does not is merging runs of slashes, applied here only when the server
  // applied it.  Nothing else -- no separator or dot-segment handling -- so
  // every other normalization on which the two parsers differ shows up as a
  // difference.
  GoogleString path = GoogleUrl::UnescapeIgnorePlus(module_path);
  if (merge_slashes) {
    GoogleString merged;
    merged.reserve(path.size());
    for (const char c : path) {
      if (c != '/' || merged.empty() || merged.back() != '/') {
        merged.push_back(c);
      }
    }
    path.swap(merged);
  }
  return path == server_uri;
}

}  // namespace net_instaweb
