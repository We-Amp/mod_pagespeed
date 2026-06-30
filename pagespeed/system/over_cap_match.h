// Copyright (c) 2024-2026 We-Amp B.V.

// the design record over-cap host matching — the pure, PSL-free predicates that decide
// whether an observed request host is "over-cap" against a site-scoped
// license's registrable domain. Reimplemented byte-faithful from ModPageSpeed
// 2.0 (src/worker/worker.cc) rather than vendored: these live in the engine,
// not in the crypto sync boundary (tools/sync-crypto.sh), and they deliberately
// ship NO public-suffix dataset — the mint side (license-service tldts) already
// reduced the licensed domain to its registrable (eTLD+1) form. Keeping 1.1 and
// 2.0 on identical suffix-with-dot logic guarantees the over-cap verdict cannot
// drift between the two products as PSL snapshots age (the reason we do NOT
// route this through RequestContext::minimal_private_suffix()).
//
// Display/telemetry only — these never gate optimization.

#ifndef PAGESPEED_SYSTEM_OVER_CAP_MATCH_H_
#define PAGESPEED_SYSTEM_OVER_CAP_MATCH_H_

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Returns true iff |host| looks like a real, externally-routable FQDN that we
// would expect to match a licensed domain: non-empty, multi-label (contains a
// dot), not localhost, not a bare IPv4/IPv6 literal, no embedded port/colon.
// Anything else (localhost, internal single-label hosts, IP literals, blanks
// seen during testing/health checks) is conservatively NOT treated as an
// over-cap signal. Caller passes a host already stripped of any ":port".
inline bool IsExternalFqdn(StringPiece host) {
  if (host.empty() || host.size() > 253) return false;
  if (host == "localhost") return false;
  if (host.find(':') != StringPiece::npos) return false;  // IPv6 / port
  bool has_dot = false;
  bool all_digits_and_dots = true;
  for (char c : host) {
    if (c == '.') {
      has_dot = true;
    } else if (static_cast<unsigned char>(c) < '0' ||
               static_cast<unsigned char>(c) > '9') {
      all_digits_and_dots = false;
    }
  }
  if (!has_dot) return false;             // single-label internal host
  if (all_digits_and_dots) return false;  // dotted-quad IPv4 literal
  if (host[0] == '.' || host[host.size() - 1] == '.') return false;
  if (host.size() >= 10 && host.substr(host.size() - 10) == ".localhost") {
    return false;
  }
  return true;
}

// Returns true iff |observed| is the licensed registrable domain or one of its
// sub-domains. Both
// args are lowercased by the caller; |licensed| is the mint-normalized eTLD+1.
// Suffix-with-dot match avoids the badexample.com-vs-example.com false match.
// No public-suffix dataset is shipped — the mint side already reduced
// |licensed| to its registrable form (license-service tldts).
inline bool HostnameMatchesLicensedDomain(StringPiece observed,
                                          StringPiece licensed) {
  if (licensed.empty()) return false;
  if (observed == licensed) return true;
  if (observed.size() > licensed.size() + 1) {
    const StringPiece::size_type off = observed.size() - licensed.size() - 1;
    return observed[off] == '.' && observed.substr(off + 1) == licensed;
  }
  return false;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_OVER_CAP_MATCH_H_
