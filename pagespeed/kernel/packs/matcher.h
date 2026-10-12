// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_MATCHER_H_
#define PAGESPEED_KERNEL_PACKS_MATCHER_H_

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {

// Compiles a path glob. '*' matches any run of characters except '/', '**'
// matches any run including '/'; every other character is literal. A glob
// must start with '/', may not contain '?', '#', whitespace or control
// characters, and may not contain a run of three or more '*'.
absl::StatusOr<Glob> CompileGlob(std::string_view text);

// Compiles `globs` into one anchored RE2::Set (match any). Fails when the set
// exceeds the regex memory budget.
absl::StatusOr<std::shared_ptr<const RE2::Set>> CompileGlobSet(
    const std::vector<Glob>& globs);

// True when the whole of `path` matches the glob.
bool MatchGlob(const Glob& glob, std::string_view path);

// Lowercases a request host, strips a trailing ":port" (IPv6 literals in
// brackets are handled) and one trailing dot.
std::string NormalizeHost(std::string_view host);

// `normalized_host` must come from NormalizeHost(). An exact site matches
// only that host; a "*.suffix" site matches any subdomain of the suffix but
// not the apex.
bool SiteMatchesHost(const Site& site, std::string_view normalized_host);

// The site that applies to `host` (any form; it is normalized here), or null
// when no site lists it. When several match: an exact site beats a wildcard
// one, a longer wildcard suffix beats a shorter one, then file order.
const Site* FindSite(const Pack& pack, std::string_view host);

// Path scope of one rule. Returns the values of the first 9 capture groups of
// path_regex (empty string for a group that did not participate) when the
// rule matches `path`, or nullopt when it does not. A rule matches when ANY
// of its path globs match, NONE of its exclude globs match, and path_regex
// (when present) matches the entire path. `path` excludes the query.
std::optional<std::vector<std::string>> MatchRulePath(const Rule& rule,
                                                      std::string_view path);

struct RuleHit {
  const Rule* rule = nullptr;
  std::vector<std::string> captures;  // captures[i] = group i+1
};

struct Selection {
  // Null when the host is not listed; then nothing else is filled in.
  const Site* site = nullptr;
  // Indexed by KindIndex(): the first enabled rule of that kind
  // (file order) whose path scope matches. Kinds are independent.
  std::array<std::optional<RuleHit>, kKindCount> by_kind;
  // Enabled rules of the same kind that also matched but lost to an earlier
  // rule, in file order.
  std::vector<const Rule*> shadowed;
};

Selection SelectRules(const Pack& pack, std::string_view host,
                      std::string_view path);

// Table lookup by URL. When `query` (without '?') is non-empty, the key
// "path?query" is tried first; then the key "path". Returns null on a miss.
const std::string* LookupTableValue(const ValueTable& table,
                                    std::string_view path,
                                    std::string_view query);

// The first hreflang cluster whose members contain `page_url` (compared with
// NormUrl()), or null.
const HreflangCluster* FindCluster(const Pack& pack, std::string_view page_url);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_MATCHER_H_
