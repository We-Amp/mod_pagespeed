// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The worker's HTML mode: what it does with an HTML notification.
//
//   off                no HTML processing at all (the old disable_html=true)
//   markdown-static    no optimized HTML variant; the Markdown copy for agents
//                      is the only HTML-derived output
//   markdown-rendered  as markdown-static
//
// The two markdown-* modes differ only in how the copy is built for pages a
// module stores (static extraction vs a render).  For pages the nginx front
// stores they currently behave the same: the copy comes from the browser
// render, as in full.  Switching from full to a markdown-* mode does not drop
// HTML variants already stored; they are served until the page changes, is
// refreshed or is purged.
//
//   full               today's behaviour: optimized HTML variants, plus the
//                      Markdown copy when agent optimization is on (the old
//                      disable_html=false, and the default)
//
// The setting replaces the boolean disable_html.  Every surface that took the
// boolean (CLI, pagespeed.json, PATCH /v1/config, pagespeed-shared.conf) still
// takes it, under one precedence rule: an explicit, valid html_mode wins;
// otherwise disable_html=true means off and disable_html=false means full.
// Writers keep emitting disable_html, derived as "mode is off", so a reader
// that predates html_mode keeps its old behaviour.
//
// Header-only and dependency-free: the nginx module's inline shared-config
// reader includes it as well, so both readers resolve the two keys alike.

#ifndef SRC_WORKER_HTML_MODE_H_
#define SRC_WORKER_HTML_MODE_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace pagespeed {

enum class HtmlMode : uint8_t {
  kOff = 0,
  kMarkdownStatic = 1,
  kMarkdownRendered = 2,
  kFull = 3,
};

// The default when no key says otherwise: today's behaviour.
inline constexpr HtmlMode kDefaultHtmlMode = HtmlMode::kFull;

// The accepted values, for help and error text.
inline constexpr std::string_view kHtmlModeValues =
    "off, markdown-static, markdown-rendered, full";

constexpr std::string_view HtmlModeName(HtmlMode mode) {
  switch (mode) {
    case HtmlMode::kOff:
      return "off";
    case HtmlMode::kMarkdownStatic:
      return "markdown-static";
    case HtmlMode::kMarkdownRendered:
      return "markdown-rendered";
    case HtmlMode::kFull:
      return "full";
  }
  return "full";
}

// Exact, case-sensitive match on the wire names (the same rule as the other
// enumerated keys, e.g. cache_mode).  Anything else is nullopt, which every
// reader treats as "html_mode not given" so the disable_html fallback applies.
constexpr std::optional<HtmlMode> ParseHtmlMode(std::string_view value) {
  if (value == "off") return HtmlMode::kOff;
  if (value == "markdown-static") return HtmlMode::kMarkdownStatic;
  if (value == "markdown-rendered") return HtmlMode::kMarkdownRendered;
  if (value == "full") return HtmlMode::kFull;
  return std::nullopt;
}

// The legacy mapping: disable_html=true is off, false is full.
constexpr HtmlMode HtmlModeFromDisableHtml(bool disable_html) {
  return disable_html ? HtmlMode::kOff : HtmlMode::kFull;
}

// The precedence rule, in one place: an explicit, valid html_mode wins;
// otherwise the legacy boolean decides; with neither, the default.
constexpr HtmlMode ResolveHtmlMode(std::optional<HtmlMode> explicit_mode,
                                   std::optional<bool> disable_html) {
  if (explicit_mode.has_value()) return *explicit_mode;
  if (disable_html.has_value()) return HtmlModeFromDisableHtml(*disable_html);
  return kDefaultHtmlMode;
}

// The disable_html value written for readers that predate html_mode: true
// only when HTML processing is off.  Such a reader treats every markdown-*
// mode as full, which is what it did before html_mode existed.
constexpr bool HtmlModeDisablesHtml(HtmlMode mode) {
  return mode == HtmlMode::kOff;
}

constexpr bool HtmlModeIsMarkdown(HtmlMode mode) {
  return mode == HtmlMode::kMarkdownStatic ||
         mode == HtmlMode::kMarkdownRendered;
}

// Only full writes an optimized HTML variant, for any sender.
constexpr bool HtmlModeWritesHtmlVariant(HtmlMode mode) {
  return mode == HtmlMode::kFull;
}

// nginx front: whether a newly stored HTML response is sent to the worker.
// Every mode but off, because the markdown-* modes still derive the agent
// copy from it.
constexpr bool HtmlModeNotifiesHtmlStore(HtmlMode mode) {
  return mode != HtmlMode::kOff;
}

// nginx front: whether a cache hit on HTML that did not match the client's
// variant may re-notify the worker.  In full it may (the worker builds the
// missing variant).  In a markdown-* mode no HTML variant is ever written, so
// such a re-notify could only repeat; it is kept only for an agent request,
// which is the demand signal for the Markdown copy.  Off never re-notifies.
constexpr bool HtmlModeRenotifiesHtmlHit(HtmlMode mode, bool agent_demand) {
  if (mode == HtmlMode::kFull) return true;
  if (HtmlModeIsMarkdown(mode)) return agent_demand;
  return false;
}

}  // namespace pagespeed

#endif  // SRC_WORKER_HTML_MODE_H_
