// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/rule_value_parser.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/packs/json_util.h"
#include "pagespeed/kernel/packs/template.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {
namespace {

absl::Status ParseTpl(const std::string& path, const std::string& text,
                      const LoadContext& context, const ValueSpec& spec,
                      Template* out) {
  if (text.empty()) return JsonFail(path, "template must not be empty");
  if (text.size() > kMaxTemplateBytes) {
    return JsonFail(path, absl::StrCat("template is longer than ",
                                       kMaxTemplateBytes, " bytes"));
  }
  if (spec.forbid_control_chars && HasControlChars(text)) {
    return JsonFail(path,
                    "template cannot contain NUL or other control "
                    "characters");
  }
  if (spec.json_template && JsonNestingExceeds(text, kMaxJsonLdDepth)) {
    return JsonFail(path, absl::StrCat("template is nested deeper than ",
                                       kMaxJsonLdDepth, " levels"));
  }
  TemplateParseOptions options;
  options.allow_page_values = spec.json_template;
  options.capture_groups = context.rule->match.path_regex_groups;
  auto tpl = ParseTemplate(text, options);
  if (!tpl.ok()) return JsonFail(path, tpl.status().message());
  *out = *std::move(tpl);
  return absl::OkStatus();
}

// Checks each entry of `table_name` once per kind.
absl::Status CheckTableValues(const LoadContext& context, const ValueSpec& spec,
                              const std::string& table_name) {
  if (spec.check_table_value == nullptr) return absl::OkStatus();
  if (context.checked_tables != nullptr &&
      !context.checked_tables->emplace(table_name, context.rule->kind).second) {
    return absl::OkStatus();
  }
  const ValueTable& table = context.pack->tables.at(table_name);
  for (const auto& [key, value] : table) {
    absl::Status st = spec.check_table_value(value);
    if (!st.ok()) {
      return JsonFail(
          absl::StrCat("tables.", table_name, "[\"", key, "\"]"),
          absl::StrCat(st.message(), " (used by ", context.value_path, ")"));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status ParseRuleValue(const Json::Value& j, const LoadContext& context,
                            const ValueSpec& spec, RuleValue* out) {
  const std::string& path = context.value_path;
  absl::Status st = ExpectObject(j, path);
  if (!st.ok()) return st;
  st = CheckKeys(j, path, {"template", "table", "fallback_template", "pattern"},
                 {});
  if (!st.ok()) return st;
  const int sources = static_cast<int>(HasMember(j, "template")) +
                      static_cast<int>(HasMember(j, "table")) +
                      static_cast<int>(HasMember(j, "pattern"));
  if (sources != 1) {
    return JsonFail(path,
                    "must have exactly one of \"template\", \"table\" or "
                    "\"pattern\"");
  }
  if (HasMember(j, "fallback_template") && !HasMember(j, "table")) {
    return JsonFail(JsonChild(path, "fallback_template"),
                    "is only allowed together with \"table\"");
  }

  if (HasMember(j, "template")) {
    if (!spec.allow_template) {
      return JsonFail(JsonChild(path, "template"), spec.template_denied);
    }
    std::string text;
    st = GetString(j, path, "template", &text);
    if (!st.ok()) return st;
    out->source = ValueSource::kTemplate;
    return ParseTpl(JsonChild(path, "template"), text, context, spec,
                    &out->tpl);
  }

  if (HasMember(j, "pattern")) {
    const std::string pp = JsonChild(path, "pattern");
    if (!spec.allow_pattern) {
      return JsonFail(pp, "\"pattern\" is only allowed for hreflang rules");
    }
    const Json::Value& pat = Member(j, "pattern");
    st = ExpectObject(pat, pp);
    if (!st.ok()) return st;
    if (pat.empty()) return JsonFail(pp, "must not be empty");
    out->source = ValueSource::kPattern;
    std::set<std::string> codes;
    for (const std::string& key : pat.getMemberNames()) {
      const std::string ep = JsonChild(pp, key);
      auto code = NormalizeHreflangCode(key);
      if (!code.has_value()) return JsonFail(ep, "not a valid hreflang code");
      if (!codes.insert(*code).second) {
        return JsonFail(
            ep, absl::StrCat("duplicate hreflang code \"", *code, "\""));
      }
      const Json::Value& entry = Member(pat, key);
      if (!entry.isString()) return JsonFail(ep, "must be a string");
      Template tpl;
      st = ParseTpl(ep, AsString(entry), context, spec, &tpl);
      if (!st.ok()) return st;
      out->pattern.emplace_back(*code, std::move(tpl));
    }
    // JSON objects are unordered; sort so the emitted order is deterministic.
    std::sort(out->pattern.begin(), out->pattern.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return absl::OkStatus();
  }

  // "table"
  const std::string tp = JsonChild(path, "table");
  st = GetString(j, path, "table", &out->table);
  if (!st.ok()) return st;
  out->source = ValueSource::kTable;
  if (!spec.allow_table) return JsonFail(tp, spec.table_denied);
  if (spec.hreflang_table) {
    if (out->table != kHreflangTableName) {
      return JsonFail(tp, "hreflang rules can only use the table \"hreflang\"");
    }
    if (HasMember(j, "fallback_template")) {
      return JsonFail(JsonChild(path, "fallback_template"),
                      "is not allowed for the hreflang table");
    }
    return absl::OkStatus();
  }
  if (out->table == kHreflangTableName) {
    return JsonFail(tp, "the table \"hreflang\" is only for hreflang rules");
  }
  if (context.pack->tables.find(out->table) == context.pack->tables.end()) {
    return JsonFail(tp, absl::StrCat("unknown table \"", out->table, "\""));
  }
  st = CheckTableValues(context, spec, out->table);
  if (!st.ok()) return st;
  if (HasMember(j, "fallback_template")) {
    std::string text;
    st = GetString(j, path, "fallback_template", &text);
    if (!st.ok()) return st;
    Template tpl;
    st = ParseTpl(JsonChild(path, "fallback_template"), text, context, spec,
                  &tpl);
    if (!st.ok()) return st;
    out->fallback_tpl = std::move(tpl);
  }
  return absl::OkStatus();
}

}  // namespace pagespeed::packs
