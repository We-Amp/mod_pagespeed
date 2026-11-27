// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/robots_ai_directives.h"

#include <string>
#include <string_view>
#include <vector>

#include "lib/base/string_util.h"

namespace net_instaweb {

namespace {

// Split a directive value on commas / whitespace / semicolons into lowercased
// tokens. Used for X-Robots-Tag and <meta robots> content.
bool HasToken(std::string_view value, std::string_view token) {
  std::size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() &&
           (value[i] == ',' || value[i] == ';' || IsHtmlSpace(value[i]))) {
      ++i;
    }
    std::size_t start = i;
    while (i < value.size() && value[i] != ',' && value[i] != ';' &&
           !IsHtmlSpace(value[i])) {
      ++i;
    }
    if (i > start && StringCaseEqual(value.substr(start, i - start), token)) {
      return true;
    }
  }
  return false;
}

// A single robots.txt rule.
struct Rule {
  bool allow;
  std::string path;
};

// A robots.txt group: the user-agents it applies to + its rules.
struct Group {
  std::vector<std::string> user_agents;  // lowercased
  std::vector<Rule> rules;
};

// Strip a trailing comment and surrounding whitespace from a robots.txt line.
std::string_view CleanLine(std::string_view line) {
  std::size_t hash = line.find('#');
  if (hash != std::string_view::npos) line = line.substr(0, hash);
  TrimHtmlWhitespace(&line);
  return line;
}

// Parse "field: value" (case-insensitive field). Returns false if not a
// recognized directive line.
bool ParseDirective(std::string_view line, std::string_view field,
                    std::string_view* value_out) {
  std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) return false;
  std::string_view key = line.substr(0, colon);
  TrimHtmlWhitespace(&key);
  if (!StringCaseEqual(key, field)) return false;
  std::string_view value = line.substr(colon + 1);
  TrimHtmlWhitespace(&value);
  *value_out = value;
  return true;
}

std::vector<Group> ParseGroups(std::string_view robots) {
  std::vector<Group> groups;
  Group current;
  bool seen_rule = false;  // a UA line after a rule starts a new group
  std::size_t pos = 0;
  while (pos <= robots.size()) {
    std::size_t nl = robots.find('\n', pos);
    std::string_view raw = robots.substr(
        pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = (nl == std::string_view::npos) ? robots.size() + 1 : nl + 1;

    std::string_view line = CleanLine(raw);
    if (line.empty()) continue;

    std::string_view value;
    if (ParseDirective(line, "user-agent", &value)) {
      if (seen_rule) {  // start a new group
        if (!current.user_agents.empty()) groups.push_back(std::move(current));
        current = Group{};
        seen_rule = false;
      }
      if (!value.empty()) current.user_agents.push_back(AsciiToLower(value));
    } else if (ParseDirective(line, "disallow", &value)) {
      current.rules.push_back(Rule{false, std::string(value)});
      seen_rule = true;
    } else if (ParseDirective(line, "allow", &value)) {
      current.rules.push_back(Rule{true, std::string(value)});
      seen_rule = true;
    }
    // Other directives (Sitemap, Crawl-delay, Host) are ignored.
  }
  if (!current.user_agents.empty()) groups.push_back(std::move(current));
  return groups;
}

bool PathBlocksRoot(const std::string& path) {
  return path == "/" || path == "/*";
}

// Is the root path "/" disallowed for `ua` (lowercased)? Prefers a group with
// an exact UA match; falls back to the "*" group. Allow "/" beats Disallow "/".
bool DisallowsRoot(const std::vector<Group>& groups, std::string_view ua) {
  const Group* specific = nullptr;
  const Group* wildcard = nullptr;
  for (const Group& g : groups) {
    for (const std::string& name : g.user_agents) {
      if (name == "*") {
        wildcard = &g;
      } else if (StringCaseEqual(name, ua)) {
        specific = &g;
      }
    }
  }
  const Group* applicable = specific != nullptr ? specific : wildcard;
  if (applicable == nullptr) return false;
  bool disallow_root = false;
  bool allow_root = false;
  for (const Rule& r : applicable->rules) {
    if (PathBlocksRoot(r.path)) {
      if (r.allow) {
        allow_root = true;
      } else {
        disallow_root = true;
      }
    }
  }
  return disallow_root && !allow_root;
}

}  // namespace

const std::vector<std::string>& KnownAiUserAgents() {
  static const std::vector<std::string>* kAgents =
      new std::vector<std::string>{"gptbot",
                                   "google-extended",
                                   "ccbot",
                                   "claudebot",
                                   "anthropic-ai",
                                   "perplexitybot",
                                   "bytespider",
                                   "amazonbot",
                                   "applebot-extended",
                                   "meta-externalagent",
                                   "cohere-ai",
                                   "diffbot",
                                   "omgilibot",
                                   "facebookbot",
                                   "imagesiftbot",
                                   "claude-web",
                                   "google-cloudvertexbot"};
  return *kAgents;
}

RobotsAiPolicy AnalyzeRobotsForAi(std::string_view robots_txt) {
  RobotsAiPolicy policy;
  std::vector<Group> groups = ParseGroups(robots_txt);
  if (DisallowsRoot(groups, "*")) {
    policy.ai_blocked_site_wide = true;
    return policy;
  }
  for (const std::string& ua : KnownAiUserAgents()) {
    if (DisallowsRoot(groups, ua)) {
      policy.ai_blocked_site_wide = true;
      return policy;
    }
  }
  return policy;
}

bool PageExcludedByAiDirectives(std::string_view x_robots_tag,
                                std::string_view meta_robots,
                                std::string_view google_extended) {
  if (HasToken(x_robots_tag, "noai") || HasToken(x_robots_tag, "noimageai") ||
      HasToken(x_robots_tag, "none")) {
    return true;
  }
  if (HasToken(meta_robots, "noai") || HasToken(meta_robots, "noimageai") ||
      HasToken(meta_robots, "none")) {
    return true;
  }
  std::string_view ge = google_extended;
  TrimHtmlWhitespace(&ge);
  if (StringCaseEqual(ge, "none")) return true;
  return false;
}

}  // namespace net_instaweb
