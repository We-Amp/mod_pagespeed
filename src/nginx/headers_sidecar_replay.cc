// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/headers_sidecar_replay.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_metadata.h"
#include "src/nginx/headers_sidecar_record.h"

namespace pagespeed {

namespace {

bool NameIn(std::span<const std::string_view> names, std::string_view name) {
  for (std::string_view candidate : names) {
    if (HeaderNameEquals(candidate, name)) return true;
  }
  return false;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

constexpr uint8_t kClassifiedAndRequired =
    AlternateMetadata::kFlagHeadersClassified |
    AlternateMetadata::kFlagHeadersSidecarRequired;

}  // namespace

bool SidecarReadRequired(uint8_t entry_flags, bool agent_markdown) {
  return !agent_markdown &&
         (entry_flags & kClassifiedAndRequired) == kClassifiedAndRequired;
}

bool SidecarFieldCollidesWithServePath(std::string_view name) {
  return NameIn(HeadersReproducedByEntryMetadata(), name) ||
         NameIn(HeadersStampedPerResponse(), name);
}

SidecarServePlan PlanSidecarServe(uint8_t entry_flags, bool agent_markdown,
                                  std::optional<std::string_view> payload) {
  SidecarServePlan plan;
  if (!SidecarReadRequired(entry_flags, agent_markdown)) {
    plan.outcome = SidecarServeOutcome::kNotRequired;
    return plan;
  }
  std::optional<std::vector<HeaderField>> parsed;
  if (payload.has_value()) {
    parsed = ParseHeadersSidecar(*payload);
  }
  if (!parsed.has_value() || parsed->empty()) {
    // The gate never stores an empty block, so an empty parse is not "nothing
    // to carry": it is a block this entry cannot be served with.
    plan.outcome = SidecarServeOutcome::kMissing;
    return plan;
  }
  for (const HeaderField& field : *parsed) {
    // By construction the gate never stores a name the serve path builds
    // (Cache-Control, validators, Expires, Content-*, Date, Age, ...): the
    // builder owns those and the block owns the rest, so the two cannot
    // produce conflicting lines.  A block that breaks this did not come from
    // the gate (a damaged or foreign cache file).  Every build refuses to
    // serve with it — the request becomes a MISS, the caller logs it, and the
    // record arm rewrites the block from the origin.  Deliberately not an
    // assert: a damaged file on disk must not take a debug server down.
    if (SidecarFieldCollidesWithServePath(field.name)) {
      plan.outcome = SidecarServeOutcome::kMissing;
      plan.fields.clear();
      plan.carried_vary = false;
      plan.carried_nosniff = false;
      plan.foreign_field = true;
      return plan;
    }
    if (HeaderNameEquals(field.name, "vary")) {
      plan.carried_vary = true;
    } else if (HeaderNameEquals(field.name, "x-content-type-options")) {
      plan.carried_nosniff = true;
    }
  }
  plan.fields = std::move(*parsed);
  plan.outcome = SidecarServeOutcome::kReplay;
  return plan;
}

bool ConditionalRevalidationAllowed(uint8_t entry_flags, bool expired_by_age) {
  const bool classified =
      (entry_flags & AlternateMetadata::kFlagHeadersClassified) != 0;
  return classified || !expired_by_age;
}

bool IsSidecarCarriedName(std::string_view name) {
  return NameIn(HeadersSidecarAllowlist(), name);
}

bool SidecarLinkCarriesHint(std::span<const HeaderField> fields,
                            std::string_view url, std::string_view rel,
                            bool crossorigin) {
  for (const HeaderField& field : fields) {
    if (!HeaderNameEquals(field.name, "link")) continue;
    std::string_view rest = field.value;
    while (true) {
      const size_t open = rest.find('<');
      if (open == std::string_view::npos) break;
      const size_t close = rest.find('>', open + 1);
      if (close == std::string_view::npos) break;
      const std::string_view target = rest.substr(open + 1, close - open - 1);
      rest.remove_prefix(close + 1);

      // The parameters run to the next comma outside a quoted string.
      size_t end = 0;
      bool quoted = false;
      while (end < rest.size() && (quoted || rest[end] != ',')) {
        if (rest[end] == '"') quoted = !quoted;
        ++end;
      }
      std::string_view params = rest.substr(0, end);
      rest.remove_prefix(end);

      if (target != url) continue;
      bool rel_matches = false;
      bool has_crossorigin = false;
      while (!params.empty()) {
        const size_t semi = params.find(';');
        std::string_view param = params.substr(0, semi);
        params = semi == std::string_view::npos ? std::string_view()
                                                : params.substr(semi + 1);
        param = TrimSpaces(param);
        const size_t eq = param.find('=');
        const std::string_view key = TrimSpaces(param.substr(0, eq));
        if (HeaderNameEquals(key, "crossorigin")) has_crossorigin = true;
        if (eq == std::string_view::npos || !HeaderNameEquals(key, "rel")) {
          continue;
        }
        std::string_view value = TrimSpaces(param.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }
        while (!value.empty()) {
          const size_t sp = value.find(' ');
          const std::string_view token = value.substr(0, sp);
          if (!token.empty() && HeaderNameEquals(token, rel)) {
            rel_matches = true;
          }
          if (sp == std::string_view::npos) break;
          value.remove_prefix(sp + 1);
        }
      }
      if (rel_matches && has_crossorigin == crossorigin) return true;
    }
  }
  return false;
}

bool StaleErrorDropsCarriedNames(uint8_t entry_flags) {
  return (entry_flags & AlternateMetadata::kFlagHeadersClassified) != 0;
}

bool StaleErrorServeHasHeaders(uint8_t entry_flags, bool have_block) {
  return have_block || !SidecarReadRequired(entry_flags,
                                            /*agent_markdown=*/false);
}

}  // namespace pagespeed
