// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_IIS_IIS_CONFIG_UTIL_H_
#define PAGESPEED_IIS_IIS_CONFIG_UTIL_H_

#include <string>
#include <vector>

namespace iis_config_util {

// Tokenize a string by splitting on characters for which the predicate |fp|
// returns non-zero (e.g. isspace).  Supports double-quote delimited tokens:
// content between quotes is kept verbatim (spaces included).  Doubled quotes
// ("") inside a quoted region produce a single literal quote character.
inline std::vector<std::string> tokenize(const std::string& s,
                                         int (*fp)(int)) {
  std::vector<std::string> r;
  std::string tmp;
  bool instring = false;

  for (size_t i = 0; i < s.size(); i++) {
    char current = s[i];
    if (fp(static_cast<unsigned char>(s[i])) && !instring) {
      if (tmp.size()) {
        r.push_back(tmp);
        tmp = "";
      }
    } else {
      if (current == '\"') {
        if (instring) {
          if (i < (s.size() - 1) && s[i + 1] == '\"') {
            tmp += s[i];
            i++;
            continue;
          } else {
          }
        }
        instring = !instring;
        continue;
      } else {
        tmp += s[i];
      }
    }
  }

  if (tmp.size()) {
    r.push_back(tmp);
  }

  return r;
}

// --------------------------------------------------------------------------
// pagespeed.config resolution.
//
// ONE documented precedence chain, shared by the factory (process/root
// options + FileID watch), the request-time merge, and the engage gate, so
// they can never diverge (the split-brain failure this replaced surfaced). Lowest
// precedence first, effective winner last:
//
//   1. %ProgramData%\We-Amp\PageSpeed\pagespeed.config    machine-global base default
//   2. %ProgramData%\We-Amp\IISWebSpeed\pagespeed.config  legacy IISpeed fallback (upgrade only)
//   3. <site physical path>\pagespeed.config              per-site override, authoritative when present
//
// Within each directory pagespeed.config is preferred over iiswebspeed.config
// (the legacy upgrade-from-IISpeed inner fallback, via ::FindConfigFile).
// --------------------------------------------------------------------------

// The %ProgramData% base config (tiers 1/2, canonical-first). Sets *exists to
// whether a file was actually found. When neither tier exists it still returns
// the canonical PageSpeed path so callers can surface a sensible "not found"
// diagnostic pointing at where the file _should_ live.
std::string ResolveProgramDataConfig(bool* exists);

// True iff the site's per-site override (tier 3) exists on disk. This is the
// engage-gate primitive: a site engages when it has its own config file.
bool SiteConfigExists(const std::string& site_physical_path);

// The ordered precedence chain, existing files only, lowest precedence first
// (index 0) to effective winner last (back()). Contains at most the resolved
// %ProgramData% base (if present) followed by the per-site override (if
// present). Fed directly to ConfigFactory::GetConfig as the merge list.
std::vector<std::string> ResolveConfigPaths(const std::string& site_physical_path);

// The single effective winning config file = last existing entry of
// ResolveConfigPaths, or "" when no config exists anywhere in the chain.
std::string ResolveEffectiveConfigFile(const std::string& site_physical_path);

// Byte-for-byte content comparison of two config files. Missing/unreadable
// files compare unequal to any readable file (and two missing files compare
// equal). Used only for the drift-warning diagnostic.
bool ConfigContentsEqual(const std::string& a, const std::string& b);

}  // namespace iis_config_util

#endif  // PAGESPEED_IIS_IIS_CONFIG_UTIL_H_
