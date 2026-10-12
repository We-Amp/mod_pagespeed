// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/pack_loader.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/packs/json_util.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/matcher.h"
#include "pagespeed/kernel/packs/rule_kind.h"
#include "pagespeed/kernel/packs/url_norm.h"
#include "re2/re2.h"

namespace pagespeed::packs {
namespace {

using JsonValue = ::Json::Value;

#define PACKS_RETURN_IF_ERROR(expr)                \
  do {                                             \
    absl::Status packs_status_ = (expr);           \
    if (!packs_status_.ok()) return packs_status_; \
  } while (0)

const RE2& SemverRe() {
  static const RE2* re = new RE2(
      R"(^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8}))"
      R"((-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$)");
  return *re;
}

const RE2& LeadingVersionRe() {
  static const RE2* re =
      new RE2(R"(^([0-9]{1,9})\.([0-9]{1,9})\.([0-9]{1,9}))");
  return *re;
}

const RE2& RuleIdRe() {
  static const RE2* re = new RE2("^[a-z0-9-]{1,64}$");
  return *re;
}

const RE2& TableNameRe() {
  static const RE2* re = new RE2("^[a-z0-9_-]{1,64}$");
  return *re;
}

const RE2& HostRe() {
  static const RE2* re = new RE2(
      "^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?"
      "(\\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*$");
  return *re;
}

// Collapses a multi-line reader diagnostic into one line.
std::string OneLine(const std::string& text) {
  std::string out;
  bool space = true;  // swallows leading whitespace
  for (char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      if (!space) out.push_back(' ');
      space = true;
    } else {
      out.push_back(c);
      space = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

using Version = std::tuple<int, int, int>;

bool ParseLeadingVersion(std::string_view s, Version* out) {
  std::string a, b, c;
  if (!RE2::PartialMatch(s, LeadingVersionRe(), &a, &b, &c)) return false;
  int x = 0, y = 0, z = 0;
  if (!absl::SimpleAtoi(a, &x) || !absl::SimpleAtoi(b, &y) ||
      !absl::SimpleAtoi(c, &z)) {
    return false;
  }
  *out = {x, y, z};
  return true;
}

class Loader {
 public:
  Loader(std::string_view engine_version, std::string_view source_name)
      : engine_version_(engine_version), file_(source_name) {}

  absl::StatusOr<Pack> Load(std::string_view text) {
    absl::StatusOr<Pack> pack = LoadUnnamed(text);
    if (pack.ok() || file_.empty()) return pack;
    // Every error names the source; the checks below name the JSON path.
    return absl::Status(pack.status().code(),
                        absl::StrCat(file_, ": ", pack.status().message()));
  }

 private:
  absl::StatusOr<Pack> LoadUnnamed(std::string_view text);

  absl::Status GetGlobs(const JsonValue& obj, const std::string& path,
                        std::string_view key, std::vector<Glob>* out);

  absl::Status LoadInfo(const JsonValue& j);
  absl::Status LoadLicense(const JsonValue& j);
  absl::Status LoadSites(const JsonValue& j);
  absl::Status LoadDefaults(const JsonValue& j);
  absl::Status LoadTables(const JsonValue& j);
  absl::Status LoadHreflangTable(const JsonValue& j, const std::string& path);
  absl::Status LoadValueTable(const JsonValue& j, const std::string& path,
                              const std::string& name);
  absl::Status LoadRules(const JsonValue& j);
  absl::Status LoadRule(const JsonValue& j, const std::string& path,
                        Rule* rule);
  absl::Status LoadMatch(const JsonValue& j, const std::string& path,
                         RuleMatch* match);
  absl::Status BuildSets(const std::string& path, RuleMatch* match);
  void LintShadowing();

  std::string engine_version_;
  std::string file_;
  Pack pack_;
  size_t table_entries_ = 0;
  std::set<std::pair<std::string, const RuleKind*>> checked_tables_;
  std::set<std::string> distinct_globs_;
  std::map<std::string, std::shared_ptr<const RE2::Set>> glob_sets_;
};

absl::Status Loader::GetGlobs(const JsonValue& obj, const std::string& path,
                              std::string_view key, std::vector<Glob>* out) {
  if (!HasMember(obj, key)) return absl::OkStatus();
  const JsonValue& list = Member(obj, key);
  const std::string kpath = JsonChild(path, key);
  if (!list.isArray()) {
    return JsonFail(kpath, "must be an array of path globs");
  }
  if (list.empty()) return JsonFail(kpath, "must not be empty");
  if (list.size() > kMaxGlobsPerRule) {
    return JsonFail(kpath,
                    absl::StrCat("has ", list.size(), " globs; the limit is ",
                                 kMaxGlobsPerRule, " per list"));
  }
  out->clear();
  for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
    const JsonValue& e = list[i];
    if (!e.isString()) return JsonFail(JsonIndex(kpath, i), "must be a string");
    auto glob = CompileGlob(AsString(e));
    if (!glob.ok()) {
      return JsonFail(JsonIndex(kpath, i), glob.status().message());
    }
    if (distinct_globs_.insert(glob->text).second &&
        distinct_globs_.size() > kMaxGlobs) {
      return JsonFail(JsonIndex(kpath, i),
                      absl::StrCat("the pack uses more than ", kMaxGlobs,
                                   " distinct globs"));
    }
    out->push_back(*std::move(glob));
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadInfo(const JsonValue& j) {
  const std::string path = "pack";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(
      j, path,
      {"id", "version", "publisher", "engine_min", "description", "license"},
      {"id", "version"}));
  PackInfo& info = pack_.info;
  info.publisher = "local";
  PACKS_RETURN_IF_ERROR(GetString(j, path, "id", &info.id));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "version", &info.version));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "publisher", &info.publisher));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "engine_min", &info.engine_min));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "description", &info.description));
  if (HasMember(j, "license")) {
    PACKS_RETURN_IF_ERROR(LoadLicense(Member(j, "license")));
  }

  if (info.id != kPackId) {
    return JsonFail("pack.id", absl::StrCat("must be \"", kPackId, "\""));
  }
  if (!RE2::FullMatch(info.version, SemverRe())) {
    return JsonFail("pack.version",
                    "must be a semantic version (MAJOR.MINOR.PATCH)");
  }
  if (HasMember(j, "engine_min")) {
    if (!RE2::FullMatch(info.engine_min, SemverRe())) {
      return JsonFail("pack.engine_min",
                      "must be a semantic version (MAJOR.MINOR.PATCH)");
    }
    if (!engine_version_.empty()) {
      Version have{0, 0, 0};
      Version need{0, 0, 0};
      if (!ParseLeadingVersion(engine_version_, &have)) {
        return JsonFail("pack.engine_min",
                        absl::StrCat("cannot compare against engine version \"",
                                     engine_version_, "\""));
      }
      ParseLeadingVersion(info.engine_min, &need);
      if (need > have) {
        return JsonFail(
            "pack.engine_min",
            absl::StrCat("requires engine ", info.engine_min,
                         " or newer; this engine is ", engine_version_));
      }
    }
  }
  return absl::OkStatus();
}

// Gregorian calendar date check for pack.license.expires.
bool IsCalendarDate(int y, int m, int d) {
  if (y < 2000 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = kDays[m - 1];
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days = 29;
  return d <= days;
}

// The regex and the error text in LoadLicense hardcode these limits.
static_assert(kMaxEntitlementLength == 64);
static_assert(kMaxTermsUrlBytes == 512);

absl::Status Loader::LoadLicense(const JsonValue& j) {
  const std::string path = "pack.license";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(
      CheckKeys(j, path, {"entitlement", "expires", "terms"}, {}));
  PackLicense license;
  PACKS_RETURN_IF_ERROR(
      GetString(j, path, "entitlement", &license.entitlement));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "expires", &license.expires));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "terms", &license.terms));

  if (HasMember(j, "entitlement") &&
      !RE2::FullMatch(license.entitlement, "[a-z0-9-]{1,64}")) {
    return JsonFail("pack.license.entitlement", "must match [a-z0-9-]{1,64}");
  }
  if (HasMember(j, "expires")) {
    int y = 0, m = 0, d = 0;
    if (!RE2::FullMatch(license.expires, "(\\d{4})-(\\d{2})-(\\d{2})", &y, &m,
                        &d) ||
        !IsCalendarDate(y, m, d)) {
      return JsonFail("pack.license.expires", "must be a date YYYY-MM-DD");
    }
  }
  if (HasMember(j, "terms") && !(absl::StartsWith(license.terms, "https://") &&
                                 license.terms.size() <= kMaxTermsUrlBytes &&
                                 CheckUrlValue(license.terms).ok())) {
    return JsonFail("pack.license.terms",
                    "must be an absolute https URL of at most 512 bytes");
  }
  pack_.info.license = std::move(license);
  return absl::OkStatus();
}

absl::Status Loader::LoadSites(const JsonValue& j) {
  const std::string path = "sites";
  if (!j.isArray()) return JsonFail(path, "must be an array");
  if (j.empty()) return JsonFail(path, "must list at least one site");
  std::set<std::string> seen;
  for (Json::ArrayIndex i = 0; i < j.size(); ++i) {
    const std::string p = JsonIndex(path, i);
    const JsonValue& e = j[i];
    PACKS_RETURN_IF_ERROR(ExpectObject(e, p));
    PACKS_RETURN_IF_ERROR(CheckKeys(e, p, {"host", "mode"}, {"host"}));
    std::string host, mode = "report";
    PACKS_RETURN_IF_ERROR(GetString(e, p, "host", &host));
    PACKS_RETURN_IF_ERROR(GetString(e, p, "mode", &mode));
    Site site;
    std::string_view h = host;
    if (h.substr(0, 2) == "*.") {
      site.wildcard = true;
      h.remove_prefix(2);
    }
    if (h.empty() || h.size() > kMaxHostLength ||
        !RE2::FullMatch(h, HostRe())) {
      return JsonFail(JsonChild(p, "host"),
                      "must be a lowercase host name, or \"*.\" followed by "
                      "one (no port, no other wildcard)");
    }
    if (site.wildcard && h.find('.') == std::string_view::npos) {
      return JsonFail(JsonChild(p, "host"),
                      "a wildcard needs at least two labels after \"*.\"");
    }
    site.host = std::string(h);
    if (mode == "report") {
      site.mode = Mode::kReport;
    } else if (mode == "enforce") {
      site.mode = Mode::kEnforce;
    } else {
      return JsonFail(JsonChild(p, "mode"),
                      "must be \"report\" or \"enforce\"");
    }
    if (!seen.insert(host).second) {
      return JsonFail(JsonChild(p, "host"),
                      absl::StrCat("duplicate site \"", host, "\""));
    }
    pack_.sites.push_back(std::move(site));
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadDefaults(const JsonValue& j) {
  const std::string path = "defaults";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(j, path, {"paths", "exclude_paths"}, {}));
  PACKS_RETURN_IF_ERROR(GetGlobs(j, path, "paths", &pack_.default_paths));
  PACKS_RETURN_IF_ERROR(
      GetGlobs(j, path, "exclude_paths", &pack_.default_exclude_paths));
  return absl::OkStatus();
}

absl::Status Loader::LoadHreflangTable(const JsonValue& j,
                                       const std::string& path) {
  if (!j.isArray()) {
    return JsonFail(path, "the hreflang table must be an array of clusters");
  }
  for (Json::ArrayIndex i = 0; i < j.size(); ++i) {
    const std::string cp = JsonIndex(path, i);
    const JsonValue& cluster = j[i];
    PACKS_RETURN_IF_ERROR(ExpectObject(cluster, cp));
    if (cluster.empty()) return JsonFail(cp, "cluster must not be empty");
    HreflangCluster out;
    std::set<std::string> codes;
    for (const std::string& key : cluster.getMemberNames()) {
      const std::string ep = JsonChild(cp, key);
      if (++table_entries_ > kMaxTableEntries) {
        return JsonFail("tables",
                        absl::StrCat("holds more than ", kMaxTableEntries,
                                     " entries in total"));
      }
      auto code = NormalizeHreflangCode(key);
      if (!code.has_value()) {
        return JsonFail(ep, "not a valid hreflang code");
      }
      if (!codes.insert(*code).second) {
        return JsonFail(ep, absl::StrCat("duplicate hreflang code \"", *code,
                                         "\" in cluster"));
      }
      const JsonValue& entry = Member(cluster, key);
      if (!entry.isString()) return JsonFail(ep, "must be a string");
      const std::string href = AsString(entry);
      absl::Status st = CheckUrlValue(href);
      if (!st.ok()) return JsonFail(ep, st.message());
      out.entries.emplace_back(*code, href);
    }
    // JSON objects are unordered; sort so the emitted order is deterministic.
    std::sort(out.entries.begin(), out.entries.end());
    out.normalized_members.clear();
    for (const auto& entry : out.entries) {
      out.normalized_members.push_back(*NormUrl(entry.second));
    }
    const size_t index = pack_.hreflang_clusters.size();
    for (const std::string& member : out.normalized_members) {
      auto [pos, inserted] = pack_.cluster_index.emplace(member, index);
      if (!inserted && pos->second != index) {
        return JsonFail(
            cp, absl::StrCat("URL ", member, " is also in cluster ",
                             pos->second, "; a URL can belong to one cluster"));
      }
    }
    pack_.hreflang_clusters.push_back(std::move(out));
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadValueTable(const JsonValue& j, const std::string& path,
                                    const std::string& name) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  ValueTable table;
  for (const std::string& key : j.getMemberNames()) {
    const std::string ep = absl::StrCat(path, "[\"", key, "\"]");
    if (++table_entries_ > kMaxTableEntries) {
      return JsonFail("tables",
                      absl::StrCat("holds more than ", kMaxTableEntries,
                                   " entries in total"));
    }
    if (key.empty() || key.front() != '/' || key.size() > kMaxUrlBytes ||
        key.find('#') != std::string::npos ||
        std::any_of(key.begin(), key.end(), [](char c) {
          return static_cast<unsigned char>(c) <= 0x20 ||
                 static_cast<unsigned char>(c) == 0x7f;
        })) {
      return JsonFail(path, absl::StrCat("key \"", key,
                                         "\" must be a URL path starting with "
                                         "'/' (optionally with ?query), "
                                         "without whitespace or '#'"));
    }
    const JsonValue& entry = Member(j, key);
    if (!entry.isString()) return JsonFail(ep, "must be a string");
    std::string value = AsString(entry);
    if (value.size() > kMaxTemplateBytes) {
      return JsonFail(ep, absl::StrCat("value is longer than ",
                                       kMaxTemplateBytes, " bytes"));
    }
    table.emplace(key, std::move(value));
  }
  pack_.tables.emplace(name, std::move(table));
  return absl::OkStatus();
}

absl::Status Loader::LoadTables(const JsonValue& j) {
  const std::string path = "tables";
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  for (const std::string& name : j.getMemberNames()) {
    const std::string tp = JsonChild(path, name);
    if (!RE2::FullMatch(name, TableNameRe())) {
      return JsonFail(path, absl::StrCat("table name \"", name,
                                         "\" must match [a-z0-9_-]{1,64}"));
    }
    if (name == kHreflangTableName) {
      PACKS_RETURN_IF_ERROR(LoadHreflangTable(Member(j, name), tp));
    } else {
      PACKS_RETURN_IF_ERROR(LoadValueTable(Member(j, name), tp, name));
    }
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadMatch(const JsonValue& j, const std::string& path,
                               RuleMatch* match) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(
      CheckKeys(j, path, {"paths", "exclude_paths", "path_regex"}, {}));
  PACKS_RETURN_IF_ERROR(GetGlobs(j, path, "paths", &match->paths));
  PACKS_RETURN_IF_ERROR(
      GetGlobs(j, path, "exclude_paths", &match->exclude_paths));
  if (HasMember(j, "path_regex")) {
    const std::string rp = JsonChild(path, "path_regex");
    std::string text;
    PACKS_RETURN_IF_ERROR(GetString(j, path, "path_regex", &text));
    if (text.empty()) return JsonFail(rp, "must not be empty");
    if (text.size() > kMaxRegexBytes) {
      return JsonFail(
          rp, absl::StrCat("is longer than ", kMaxRegexBytes, " bytes"));
    }
    auto re = std::make_shared<const RE2>(text, MakeRegexOptions());
    if (!re->ok()) {
      return JsonFail(rp, absl::StrCat("invalid RE2 pattern: ", re->error()));
    }
    match->path_regex_text = std::move(text);
    match->path_regex_groups = re->NumberOfCapturingGroups();
    match->path_regex = std::move(re);
  }
  return absl::OkStatus();
}

absl::Status Loader::LoadRule(const JsonValue& j, const std::string& path,
                              Rule* rule) {
  PACKS_RETURN_IF_ERROR(ExpectObject(j, path));
  PACKS_RETURN_IF_ERROR(CheckKeys(j, path,
                                  {"id", "kind", "enabled", "enforce", "match",
                                   "value", "on_present", "link_header"},
                                  {"id", "kind", "value"}));
  PACKS_RETURN_IF_ERROR(GetString(j, path, "id", &rule->id));
  if (!RE2::FullMatch(rule->id, RuleIdRe())) {
    return JsonFail(JsonChild(path, "id"), "must match [a-z0-9-]{1,64}");
  }

  std::string kind;
  PACKS_RETURN_IF_ERROR(GetString(j, path, "kind", &kind));
  rule->kind = FindKind(kind);
  if (rule->kind == nullptr) {
    return JsonFail(JsonChild(path, "kind"),
                    absl::StrCat("must be one of ", KindNameList()));
  }

  PACKS_RETURN_IF_ERROR(GetBool(j, path, "enabled", &rule->enabled));
  PACKS_RETURN_IF_ERROR(GetBool(j, path, "enforce", &rule->enforce));

  std::string on_present = "keep";
  PACKS_RETURN_IF_ERROR(GetString(j, path, "on_present", &on_present));
  if (on_present == "keep") {
    rule->on_present = OnPresent::kKeep;
  } else if (on_present == "repair") {
    rule->on_present = OnPresent::kRepair;
  } else if (on_present == "replace") {
    rule->on_present = OnPresent::kReplace;
  } else {
    return JsonFail(JsonChild(path, "on_present"),
                    "must be \"keep\", \"repair\" or \"replace\"");
  }

  if (HasMember(j, "link_header")) {
    const std::string lh_path = JsonChild(path, "link_header");
    if (!rule->kind->SupportsLinkHeader()) {
      return JsonFail(lh_path, "is only valid on a canonical rule");
    }
    std::string link_header;
    PACKS_RETURN_IF_ERROR(GetString(j, path, "link_header", &link_header));
    if (link_header == "off") {
      rule->link_header = LinkHeaderMode::kOff;
    } else if (link_header == "match") {
      rule->link_header = LinkHeaderMode::kMatch;
    } else {
      return JsonFail(lh_path, "must be \"off\" or \"match\"");
    }
  }

  if (HasMember(j, "match")) {
    PACKS_RETURN_IF_ERROR(
        LoadMatch(Member(j, "match"), JsonChild(path, "match"), &rule->match));
  }
  if (rule->match.paths.empty()) rule->match.paths = pack_.default_paths;
  if (rule->match.exclude_paths.empty()) {
    rule->match.exclude_paths = pack_.default_exclude_paths;
  }
  PACKS_RETURN_IF_ERROR(BuildSets(JsonChild(path, "match"), &rule->match));

  LoadContext context;
  context.pack = &pack_;
  context.rule = rule;
  context.value_path = JsonChild(path, "value");
  context.checked_tables = &checked_tables_;
  return rule->kind->ParseValue(Member(j, "value"), context, &rule->value);
}

absl::Status Loader::BuildSets(const std::string& path, RuleMatch* match) {
  auto build = [&](const std::vector<Glob>& globs,
                   std::shared_ptr<const RE2::Set>* out) -> absl::Status {
    if (globs.empty()) return absl::OkStatus();
    std::string key;
    for (const Glob& g : globs) absl::StrAppend(&key, g.text, "\n");
    auto it = glob_sets_.find(key);
    if (it == glob_sets_.end()) {
      auto set = CompileGlobSet(globs);
      if (!set.ok()) return JsonFail(path, set.status().message());
      it = glob_sets_.emplace(key, *std::move(set)).first;
    }
    *out = it->second;
    return absl::OkStatus();
  };
  PACKS_RETURN_IF_ERROR(build(match->paths, &match->paths_set));
  return build(match->exclude_paths, &match->exclude_set);
}

absl::Status Loader::LoadRules(const JsonValue& j) {
  const std::string path = "rules";
  if (!j.isArray()) return JsonFail(path, "must be an array");
  if (j.size() > kMaxRules) {
    return JsonFail(path, absl::StrCat("has ", j.size(),
                                       " rules; the limit is ", kMaxRules));
  }
  std::unordered_set<std::string> ids;
  for (Json::ArrayIndex i = 0; i < j.size(); ++i) {
    Rule rule;
    const std::string p = JsonIndex(path, i);
    PACKS_RETURN_IF_ERROR(LoadRule(j[i], p, &rule));
    if (!ids.insert(rule.id).second) {
      return JsonFail(JsonChild(p, "id"),
                      absl::StrCat("duplicate rule id \"", rule.id, "\""));
    }
    if (rule.enabled && rule.link_header == LinkHeaderMode::kMatch) {
      pack_.has_link_header_rule = true;
    }
    pack_.rules.push_back(std::move(rule));
  }
  return absl::OkStatus();
}

// True when every path matched by `inner` is certainly matched by `outer`.
bool GlobCovers(const Glob& outer, const Glob& inner) {
  if (outer.text == inner.text) return true;
  constexpr std::string_view kTail = "**";
  if (outer.text.size() >= kTail.size() &&
      outer.text.compare(outer.text.size() - kTail.size(), kTail.size(),
                         kTail) == 0) {
    const std::string_view prefix(outer.text.data(),
                                  outer.text.size() - kTail.size());
    return inner.text.size() >= prefix.size() &&
           std::string_view(inner.text).substr(0, prefix.size()) == prefix;
  }
  return false;
}

void Loader::LintShadowing() {
  for (size_t b = 0; b < pack_.rules.size(); ++b) {
    const Rule& later = pack_.rules[b];
    if (!later.enabled) continue;
    for (size_t a = 0; a < b; ++a) {
      const Rule& earlier = pack_.rules[a];
      if (!earlier.enabled || earlier.kind != later.kind) continue;
      if (earlier.match.path_regex != nullptr) continue;
      // The earlier rule's excludes must all be excluded by the later rule
      // too, or the later rule could match a path the earlier one skips.
      const bool excludes_subset = std::all_of(
          earlier.match.exclude_paths.begin(),
          earlier.match.exclude_paths.end(), [&](const Glob& e) {
            return std::any_of(later.match.exclude_paths.begin(),
                               later.match.exclude_paths.end(),
                               [&](const Glob& l) { return GlobCovers(l, e); });
          });
      if (!excludes_subset) continue;
      const bool covered = std::all_of(
          later.match.paths.begin(), later.match.paths.end(),
          [&](const Glob& g) {
            return std::any_of(earlier.match.paths.begin(),
                               earlier.match.paths.end(),
                               [&](const Glob& o) { return GlobCovers(o, g); });
          });
      if (covered) {
        pack_.warnings.push_back(
            absl::StrCat("rule \"", later.id, "\" (", later.kind->name(),
                         ") can never apply: rule \"", earlier.id,
                         "\" comes first and matches every path it matches"));
        break;
      }
    }
  }
}

absl::StatusOr<Pack> Loader::LoadUnnamed(std::string_view text) {
  if (text.size() > kMaxPackFileBytes) {
    return JsonFail(
        "", absl::StrCat("file is ", text.size(), " bytes; the limit is ",
                         kMaxPackFileBytes));
  }

  // A strict reader: one object or array at the root, no comments, trailing
  // commas, single quotes, special floats or trailing data, a repeated key at
  // any level is an error, and nesting is bounded.
  ::Json::CharReaderBuilder builder;
  ::Json::CharReaderBuilder::strictMode(&builder.settings_);
  builder.settings_["rejectDupKeys"] = true;
  builder.settings_["stackLimit"] = static_cast<int>(kMaxJsonDepth);
  std::unique_ptr<::Json::CharReader> reader(builder.newCharReader());

  JsonValue root;
  std::string errors;
  try {
    if (!reader->parse(text.data(), text.data() + text.size(), &root,
                       &errors)) {
      // jsoncpp reports "* Line 1, Column 8\n  Syntax error: ..." and, past
      // its stack limit, throws instead.
      return JsonFail("", absl::StrCat("malformed JSON: ", OneLine(errors)));
    }
  } catch (const ::Json::Exception&) {
    return JsonFail(
        "", absl::StrCat("nested deeper than ", kMaxJsonDepth, " levels"));
  }
  // The reader hands over whatever bytes the file held.
  PACKS_RETURN_IF_ERROR(CheckUtf8(root));

  PACKS_RETURN_IF_ERROR(ExpectObject(root, ""));
  // "signature" is reserved: signatures live in the <pack>.sig sidecar, so the
  // key is rejected like any unknown key.
  PACKS_RETURN_IF_ERROR(
      CheckKeys(root, "", {"pack", "sites", "defaults", "tables", "rules"},
                {"pack", "sites", "rules"}));

  PACKS_RETURN_IF_ERROR(LoadInfo(Member(root, "pack")));
  PACKS_RETURN_IF_ERROR(LoadSites(Member(root, "sites")));
  if (HasMember(root, "defaults")) {
    PACKS_RETURN_IF_ERROR(LoadDefaults(Member(root, "defaults")));
  }
  if (pack_.default_paths.empty()) {
    auto all = CompileGlob("/**");
    if (!all.ok()) return all.status();
    pack_.default_paths.push_back(*std::move(all));
  }
  if (HasMember(root, "tables")) {
    PACKS_RETURN_IF_ERROR(LoadTables(Member(root, "tables")));
  }
  PACKS_RETURN_IF_ERROR(LoadRules(Member(root, "rules")));
  LintShadowing();
  return std::move(pack_);
}

}  // namespace

absl::StatusOr<Pack> LoadPack(std::string_view json,
                              std::string_view engine_version,
                              std::string_view source_name) {
  Loader loader(engine_version, source_name);
  return loader.Load(json);
}

absl::StatusOr<Pack> LoadPackFile(const std::string& path,
                                  std::string_view engine_version) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return absl::NotFoundError(
        absl::StrCat(path, ": cannot read pack file: ", ec.message()));
  }
  if (size > kMaxPackFileBytes) {
    return absl::InvalidArgumentError(absl::StrCat(path, ": $: file is ", size,
                                                   " bytes; the limit is ",
                                                   kMaxPackFileBytes));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(absl::StrCat(path, ": cannot open pack file"));
  }
  // Read at most one byte past the limit, so a file that grew since the size
  // check is still bounded (and then rejected by LoadPack).
  std::string text(kMaxPackFileBytes + 1, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(in.gcount()));
  return LoadPack(text, engine_version, path);
}

}  // namespace pagespeed::packs
