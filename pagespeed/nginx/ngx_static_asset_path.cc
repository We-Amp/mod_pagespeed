// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_static_asset_path.h"

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

StringPiece NgxStaticAssetName(StringPiece server_uri, StringPiece module_path,
                               StringPiece prefix) {
  if (prefix.empty() || !prefix.ends_with("/") || server_uri != module_path ||
      server_uri.size() <= prefix.size() || !server_uri.starts_with(prefix)) {
    return StringPiece();
  }
  StringPiece name = server_uri;
  name.remove_prefix(prefix.size());
  if (name.find('/') != StringPiece::npos) {
    return StringPiece();
  }
  return name;
}

}  // namespace net_instaweb
