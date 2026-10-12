// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PACK_H_
#define PAGESPEED_KERNEL_PACKS_PACK_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "pagespeed/kernel/packs/template.h"
#include "re2/re2.h"
#include "re2/set.h"

namespace pagespeed::packs {

// ---------------------------------------------------------------------------
// Limits. Every bound the loader and the later page pass enforce is named
// here.
// ---------------------------------------------------------------------------
inline constexpr size_t kMaxPackFileBytes = 1024 * 1024;  // 1 MiB
inline constexpr size_t kMaxRules = 200;
inline constexpr size_t kMaxTableEntries = 50000;          // all tables
inline constexpr size_t kMaxTemplateBytes = 16 * 1024;     // 16 KiB
inline constexpr int64_t kRegexMaxMemBytes = 1024 * 1024;  // per regex
inline constexpr size_t kMaxUrlBytes = 2048;               // per value
inline constexpr size_t kMaxTitleChars = 300;              // per value
inline constexpr size_t kMaxDescriptionChars = 1000;       // per value
inline constexpr size_t kMaxJsonLdBytes = 16 * 1024;       // expanded
// An existing JSON-LD block larger than this is not parsed; the jsonld rule
// then leaves the page's JSON-LD alone.
inline constexpr size_t kMaxJsonLdScanBytes = 1024 * 1024;  // per block
// Deepest [ / { nesting a JSON-LD block or template may have. Deeper blocks
// are never parsed (parsing and printing recurse per level).
inline constexpr size_t kMaxJsonLdDepth = 64;
inline constexpr size_t kMaxAddedBytesPerPage = 64 * 1024;  // per page
inline constexpr size_t kMaxJsonDepth = 32;                 // pack file
inline constexpr size_t kMaxRuleIdLength = 64;
inline constexpr size_t kMaxEntitlementLength = 64;  // pack.license.entitlement
inline constexpr size_t kMaxTermsUrlBytes = 512;     // pack.license.terms
inline constexpr size_t kMaxTableNameLength = 64;
inline constexpr size_t kMaxHostLength = 253;
inline constexpr size_t kMaxGlobBytes = 512;
inline constexpr size_t kMaxGlobsPerRule = 32;  // per list (paths, excludes)
inline constexpr size_t kMaxGlobs = 2000;       // distinct globs in a pack
inline constexpr size_t kMaxRegexBytes = 1024;
inline constexpr int kMaxCaptureRefs = 9;  // {1}..{9}

inline constexpr std::string_view kPackId = "edge-seo";
inline constexpr std::string_view kHreflangTableName = "hreflang";

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

// Report < enforce. The effective mode is the lowest of the global mode, the
// matching site's mode and the rule's own opt-in.
enum class Mode { kReport = 0, kEnforce = 1 };

// A rule's kind is a handler from the kind registry (kind_registry.h); the
// kinds and their order are listed in kind_table.inc.
class RuleKind;

enum class OnPresent { kKeep, kRepair, kReplace };

// The canonical rule's optional `link_header` field: `match` also keeps the
// `Link: <url>; rel="canonical"` response header in line with the canonical
// the rule decides on.
enum class LinkHeaderMode { kOff, kMatch };

std::string_view ModeName(Mode mode);
std::string_view OnPresentName(OnPresent on_present);
std::string_view LinkHeaderModeName(LinkHeaderMode mode);

// min(global, site, rule). `rule_enforce` false means the rule is report-only.
Mode EffectiveMode(Mode global, Mode site, bool rule_enforce);

// The optional pack.license object. An empty string = the key was absent.
struct PackLicense {
  std::string entitlement;
  std::string expires;  // YYYY-MM-DD
  std::string terms;    // absolute https URL
};

struct PackInfo {
  std::string id;
  std::string version;
  // Key id a signature must be made with; "local" when absent.
  std::string publisher;
  std::string engine_min;  // empty when absent
  std::string description;
  std::optional<PackLicense> license;  // nullopt = a free pack
};

struct Site {
  // Exact lowercase host, or the suffix (without "*.") when `wildcard`.
  std::string host;
  bool wildcard = false;
  Mode mode = Mode::kReport;
};

// A compiled path glob: '*' matches within a path segment, '**' crosses
// '/'. Every other character is literal.
struct Glob {
  std::string text;
  std::string pattern;  // the RE2 source the glob compiles to
  std::shared_ptr<const RE2> regex;
};

// Effective path scope of a rule (rule values with defaults applied).
struct RuleMatch {
  std::vector<Glob> paths;          // never empty after loading
  std::vector<Glob> exclude_paths;  // may be empty
  // The same globs compiled into one anchored set each, so a rule costs one
  // automaton run however many globs it has. Null for a hand-built rule, in
  // which case matching falls back to the individual globs.
  std::shared_ptr<const RE2::Set> paths_set;
  std::shared_ptr<const RE2::Set> exclude_set;
  std::string path_regex_text;            // empty when absent
  std::shared_ptr<const RE2> path_regex;  // null when absent
  int path_regex_groups = 0;
};

enum class ValueSource { kTemplate, kTable, kPattern };

struct RuleValue {
  ValueSource source = ValueSource::kTemplate;
  Template tpl;                          // kTemplate
  std::string table;                     // kTable: name of the table
  std::optional<Template> fallback_tpl;  // kTable, optional
  // kPattern (hreflang): normalized code -> href template, sorted by code.
  std::vector<std::pair<std::string, Template>> pattern;
};

struct Rule {
  std::string id;
  const RuleKind* kind = nullptr;  // never null in a loaded pack
  bool enabled = true;
  bool enforce = false;
  RuleMatch match;
  RuleValue value;
  OnPresent on_present = OnPresent::kKeep;
  // Only canonical rules accept anything but kOff.
  LinkHeaderMode link_header = LinkHeaderMode::kOff;
};

// A per-URL value table: key is a URL path (with "?query" when the key
// contains one) -> value.
using ValueTable = std::map<std::string, std::string, std::less<>>;

// One hreflang cluster: normalized code -> absolute href, sorted by code.
// `normalized_members` holds NormUrl() of every href, for membership tests.
struct HreflangCluster {
  std::vector<std::pair<std::string, std::string>> entries;
  std::vector<std::string> normalized_members;
};

struct Pack {
  PackInfo info;
  std::vector<Site> sites;
  std::vector<Glob> default_paths;          // "/**" when absent
  std::vector<Glob> default_exclude_paths;  // may be empty
  std::map<std::string, ValueTable, std::less<>> tables;
  std::vector<HreflangCluster> hreflang_clusters;
  // NormUrl(member href) -> index into hreflang_clusters. A URL can belong to
  // one cluster only (a load error otherwise).
  std::unordered_map<std::string, size_t> cluster_index;
  std::vector<Rule> rules;
  // True when an enabled rule has `link_header: match`; set by the loader, so
  // callers need not walk the rules.
  bool has_link_header_rule = false;
  // Non-fatal findings from the load (currently: statically shadowed rules).
  std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// Value checks shared by the loader (table values) and the page pass
// (expanded values).
// ---------------------------------------------------------------------------

// OK when `value` is an absolute http(s) URL with a host, at most
// kMaxUrlBytes long, and free of whitespace, control characters, backslashes
// and the characters " < >.
absl::Status CheckUrlValue(std::string_view value);

// RE2 options for every pattern in a pack: Latin-1, so a pattern and a URL
// path are matched byte for byte (`.` matches any byte, including 0xFF), with
// the per-regex memory budget and logging off.
RE2::Options MakeRegexOptions();

// True when the [ / { nesting of `s` (outside JSON strings) exceeds
// `max_depth`. A lexical scan; it does not need `s` to be valid JSON.
bool JsonNestingExceeds(std::string_view s, size_t max_depth);

// True when `s` contains NUL or any other C0 control character, or DEL.
bool HasControlChars(std::string_view s);

// Number of UTF-8 code points (counts lead bytes; assumes valid UTF-8).
size_t Utf8Length(std::string_view s);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PACK_H_
