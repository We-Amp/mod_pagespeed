// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_url_port.h"

namespace net_instaweb {

GoogleString IisUrlWithoutPort(StringPiece url) {
  const StringPiece::size_type scheme_end = url.find("://");
  if (scheme_end == StringPiece::npos) {
    return url.as_string();
  }
  const StringPiece::size_type authority = scheme_end + 3;
  StringPiece::size_type authority_end = url.find_first_of("/?#", authority);
  if (authority_end == StringPiece::npos) {
    authority_end = url.size();
  }
  // Userinfo, if any, ends at the last '@' of the authority.
  StringPiece::size_type host = authority;
  const StringPiece::size_type at = url.rfind('@', authority_end - 1);
  if (at != StringPiece::npos && at >= authority && at < authority_end) {
    host = at + 1;
  }
  StringPiece::size_type colon = StringPiece::npos;
  if (host < authority_end && url[host] == '[') {
    const StringPiece::size_type close = url.find(']', host);
    if (close == StringPiece::npos || close >= authority_end) {
      return url.as_string();
    }
    if (close + 1 < authority_end && url[close + 1] == ':') {
      colon = close + 1;
    }
  } else {
    colon = url.find(':', host);
    if (colon != StringPiece::npos && colon >= authority_end) {
      colon = StringPiece::npos;
    }
  }
  if (colon == StringPiece::npos) {
    return url.as_string();
  }
  return StrCat(url.substr(0, colon), url.substr(authority_end));
}

}  // namespace net_instaweb
