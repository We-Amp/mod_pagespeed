// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_purge_observer.h"

namespace net_instaweb {

GoogleString DaemonHtmlNormalizePurgeUrl(StringPiece url) {
  StringPiece scheme;
  StringPiece default_port;
  if (StringCaseStartsWith(url, "https://")) {
    scheme = "https://";
    default_port = ":443";
  } else if (StringCaseStartsWith(url, "http://")) {
    scheme = "http://";
    default_port = ":80";
  } else {
    return GoogleString(url.data(), url.size());
  }
  StringPiece rest = url.substr(scheme.size());
  size_t host_end = rest.find_first_of("/?#");
  if (host_end == StringPiece::npos) {
    host_end = rest.size();
  }
  StringPiece authority = rest.substr(0, host_end);
  const StringPiece tail = rest.substr(host_end);
  if (authority.size() > default_port.size() &&
      authority.substr(authority.size() - default_port.size()) ==
          default_port) {
    authority = authority.substr(0, authority.size() - default_port.size());
  }
  GoogleString out(scheme.data(), scheme.size());
  for (const char c : authority) {
    out.push_back(LowerChar(c));
  }
  StrAppend(&out, tail);
  return out;
}

std::vector<GoogleString> DaemonHtmlPurgeKeys(StringPiece url) {
  std::vector<GoogleString> keys;
  keys.emplace_back(url.data(), url.size());
  GoogleString normalized = DaemonHtmlNormalizePurgeUrl(url);
  if (normalized != keys[0]) {
    keys.push_back(std::move(normalized));
  }
  return keys;
}

}  // namespace net_instaweb
