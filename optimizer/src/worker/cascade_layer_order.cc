// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - cascade-layer order for the critical block. See the header.

#include "src/worker/cascade_layer_order.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lib/base/string_util.h"
#include "src/worker/html_css_injector.h"

namespace pagespeed {

namespace {

// The same recursion limit the @import flattener uses.
constexpr int kMaxImportDepth = 5;
// Block nesting beyond this is refused rather than recursed into.
constexpr int kMaxBlockDepth = 64;

bool IsCssSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool StartsWithCi(std::string_view s, size_t pos, std::string_view prefix) {
  return pos + prefix.size() <= s.size() &&
         net_instaweb::StringCaseEqual(s.substr(pos, prefix.size()), prefix);
}

// One past the comment opened at css[i] ("/*"); the end when unterminated.
size_t SkipComment(std::string_view css, size_t i) {
  size_t end = css.find("*/", i + 2);
  return end == std::string_view::npos ? css.size() : end + 2;
}

bool StartsComment(std::string_view css, size_t i) {
  return i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*';
}

// One past the string opened at css[i] (a quote). A newline ends a bad string.
size_t SkipString(std::string_view css, size_t i) {
  const char quote = css[i];
  ++i;
  while (i < css.size()) {
    const char c = css[i];
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == quote) return i + 1;
    if (c == '\n') return i;
    ++i;
  }
  return css.size();
}

size_t SkipSpaceAndComments(std::string_view css, size_t i, size_t end) {
  while (i < end) {
    if (IsCssSpace(css[i])) {
      ++i;
    } else if (StartsComment(css, i)) {
      i = SkipComment(css, i);
    } else {
      break;
    }
  }
  return i < end ? i : end;
}

// `s` without surrounding whitespace and comments.
std::string_view TrimSpaceAndComments(std::string_view s) {
  size_t b = SkipSpaceAndComments(s, 0, s.size());
  size_t e = s.size();
  while (e > b) {
    if (IsCssSpace(s[e - 1])) {
      --e;
    } else if (e - b >= 4 && s[e - 1] == '/' && s[e - 2] == '*') {
      // A trailing comment: find its opening.
      size_t open = s.rfind("/*", e - 2);
      if (open == std::string_view::npos || open < b) break;
      e = open;
    } else {
      break;
    }
  }
  return s.substr(b, e - b);
}

// Where an at-rule's prelude or a qualified rule's prelude ends, from `i`: the
// first `;`, `{` or `}` outside parentheses, brackets, strings and comments.
// `end` when none.
size_t FindPreludeEnd(std::string_view css, size_t i, size_t end) {
  int depth = 0;
  while (i < end) {
    const char c = css[i];
    if (StartsComment(css, i)) {
      i = SkipComment(css, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = SkipString(css, i);
      continue;
    }
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    } else if (depth == 0 && (c == ';' || c == '{' || c == '}')) {
      return i;
    }
    ++i;
  }
  return end;
}

// The `}` matching the `{` at css[open], or `end` when the block runs to the
// end (a browser closes it there).
size_t FindBlockEnd(std::string_view css, size_t open, size_t end) {
  int depth = 0;
  size_t i = open;
  while (i < end) {
    const char c = css[i];
    if (StartsComment(css, i)) {
      i = SkipComment(css, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = SkipString(css, i);
      continue;
    }
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return i;
    }
    ++i;
  }
  return end;
}

bool IsNameChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || u >= 0x80;
}

// One segment of a layer name, read with certainty: an identifier without
// escapes that is not a CSS-wide keyword.
bool IsReadableLayerSegment(std::string_view seg) {
  if (seg.empty()) return false;
  for (char c : seg) {
    if (!IsNameChar(c)) return false;
  }
  if (seg[0] >= '0' && seg[0] <= '9') return false;
  if (seg[0] == '-' && (seg.size() == 1 || (seg[1] >= '0' && seg[1] <= '9'))) {
    return false;
  }
  for (std::string_view kw :
       {"initial", "inherit", "unset", "revert", "revert-layer", "default"}) {
    if (net_instaweb::StringCaseEqual(seg, kw)) return false;
  }
  return true;
}

// `name` split on '.', each segment readable; empty when it is not.
std::vector<std::string_view> SplitLayerName(std::string_view name) {
  std::vector<std::string_view> segs;
  size_t start = 0;
  while (true) {
    size_t dot = name.find('.', start);
    std::string_view seg = name.substr(start, dot == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : dot - start);
    if (!IsReadableLayerSegment(seg)) return {};
    segs.push_back(seg);
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return segs;
}

// At-rules whose block holds rules rather than declarations.
bool IsGroupRule(std::string_view keyword) {
  for (std::string_view k : {"layer", "media", "supports", "container", "scope",
                             "document", "-moz-document", "starting-style"}) {
    if (net_instaweb::StringCaseEqual(keyword, k)) return true;
  }
  return false;
}

bool IsAllMedia(std::string_view media) {
  return net_instaweb::StringCaseEqual(TrimSpaceAndComments(media), "all");
}

// Where a rule sits in the layer tree.
struct Context {
  // The enclosing named layer, fully qualified; empty at the top level.
  std::string parent;
  // Inside a block or under a condition that may not apply.
  bool conditional = false;
  // Inside an anonymous layer: nothing in here can be named from outside.
  bool in_anonymous = false;
};

class Walker {
 public:
  // collect_only: record every named layer mentioned, ignoring conditions and
  // anonymous layers (used on the critical block, whose own mentions all follow
  // the statement). @import then always refuses.
  Walker(const LayerOrderImportLookup* lookup, bool collect_only)
      : lookup_(lookup), collect_only_(collect_only) {}

  void WalkSheet(std::string_view css, const Context& ctx,
                 std::string_view base_url, int import_depth) {
    WalkBlock(css, 0, css.size(), ctx, base_url, import_depth,
              /*allow_imports=*/true, /*block_depth=*/0, /*rule_list=*/true);
  }

  void Fail(std::string reason) {
    if (!failed_) {
      failed_ = true;
      reason_ = std::move(reason);
    }
  }

  bool failed() const { return failed_; }
  bool import_missing() const { return import_missing_; }
  // An anonymous layer's block may hold an `!important`, or an
  // anonymous `@import ... layer` brings in a sheet whose content is not read.
  bool important_in_anonymous() const { return important_in_anonymous_; }
  // An anonymous layer was met anywhere, nested ones included.
  bool saw_anonymous() const { return saw_anonymous_; }
  const std::string& reason() const { return reason_; }
  std::vector<std::string>& names() { return names_; }

 private:
  // `rule_list`: the block holds rules (a sheet's top level, or a group rule
  // such as @layer or @media outside any style rule), where a browser starts a
  // rule at every token; otherwise it holds declarations (a style rule's
  // block, @font-face and the like), where `;` ends a declaration.
  void WalkBlock(std::string_view css, size_t begin, size_t end,
                 const Context& ctx, std::string_view base_url,
                 int import_depth, bool allow_imports, int block_depth,
                 bool rule_list) {
    if (block_depth > kMaxBlockDepth) {
      Fail("the stylesheet nests blocks too deeply to read");
      return;
    }
    size_t i = begin;
    while (i < end && !failed_) {
      i = SkipSpaceAndComments(css, i, end);
      if (i >= end) break;
      const char c = css[i];
      // A `}` that closes nothing, or a `;` where a rule may start: a browser
      // reads it as the start of a qualified rule and drops it together with
      // whatever follows up to the next block, which may be an @layer rule
      // this walk would otherwise count. (`;` between declarations is fine.)
      if (c == '}' || (c == ';' && rule_list)) {
        Fail(std::string("a stray '") + c +
             "' where a rule may start: a browser drops the rule after it");
        return;
      }
      if (c == ';') {
        ++i;
        continue;
      }
      // CDO / CDC tokens are ignored where rules may start.
      if (StartsWithCi(css, i, "<!--")) {
        i += 4;
        continue;
      }
      if (StartsWithCi(css, i, "-->")) {
        i += 3;
        continue;
      }
      if (c == '@') {
        size_t name_end = i + 1;
        while (name_end < end &&
               (IsNameChar(css[name_end]) || css[name_end] == '\\')) {
          name_end += css[name_end] == '\\' ? 2 : 1;
        }
        if (name_end > end) name_end = end;
        const std::string_view keyword = css.substr(i + 1, name_end - i - 1);
        // `@l\61yer` IS @layer to a browser; do not try to read escapes.
        if (keyword.find('\\') != std::string_view::npos) {
          Fail("an at-rule keyword is written with an escape");
          return;
        }
        const size_t pe = FindPreludeEnd(css, name_end, end);
        const std::string_view prelude = css.substr(name_end, pe - name_end);
        const bool is_layer = net_instaweb::StringCaseEqual(keyword, "layer");
        if (pe < end && css[pe] == '}') {
          Fail("an at-rule ends at a '}' that closes nothing");
          return;
        }
        if (pe >= end || css[pe] != '{') {
          // A statement.
          if (net_instaweb::StringCaseEqual(keyword, "import")) {
            // Only honoured before any other rule; browsers drop it anywhere
            // else.
            if (allow_imports) {
              HandleImport(prelude, ctx, base_url, import_depth);
            }
          } else if (is_layer) {
            HandleLayerStatement(prelude, ctx);
          } else if (!net_instaweb::StringCaseEqual(keyword, "charset")) {
            allow_imports = false;
          }
          i = pe < end ? pe + 1 : end;
          continue;
        }
        // An at-rule with a block.
        allow_imports = false;
        const size_t be = FindBlockEnd(css, pe, end);
        Context child = ctx;
        if (is_layer) {
          if (!EnterLayerBlock(prelude, ctx, &child)) return;
          if (child.in_anonymous && !ctx.in_anonymous &&
              CssTextHasImportantBang(css.substr(pe + 1, be - pe - 1))) {
            important_in_anonymous_ = true;
          }
        } else if (!(net_instaweb::StringCaseEqual(keyword, "media") &&
                     IsAllMedia(prelude))) {
          child.conditional = true;
        }
        WalkBlock(css, pe + 1, be, child, base_url, import_depth,
                  /*allow_imports=*/false, block_depth + 1,
                  rule_list && IsGroupRule(keyword));
        i = be < end ? be + 1 : end;
        continue;
      }
      // A qualified rule (or a declaration inside one). Its block can only
      // mention a layer through a nested group rule, which counts as
      // conditional.
      allow_imports = false;
      const size_t pe = FindPreludeEnd(css, i, end);
      if (pe >= end || css[pe] != '{') {
        // Where rules are expected, a prelude that runs into `;` or `}` makes
        // a browser keep reading up to the next block and drop all of it.
        if (rule_list && pe < end) {
          Fail(
              "a rule without a block where rules are expected: a browser "
              "drops the rule after it");
          return;
        }
        i = pe < end ? pe + 1 : end;
        continue;
      }
      const size_t be = FindBlockEnd(css, pe, end);
      Context child = ctx;
      child.conditional = true;
      WalkBlock(css, pe + 1, be, child, base_url, import_depth,
                /*allow_imports=*/false, block_depth + 1,
                /*rule_list=*/false);
      i = be < end ? be + 1 : end;
    }
  }

  // `@layer a, b.c;`
  void HandleLayerStatement(std::string_view prelude, const Context& ctx) {
    size_t start = 0;
    while (start <= prelude.size()) {
      size_t comma = prelude.find(',', start);
      std::string_view item = TrimSpaceAndComments(prelude.substr(
          start, comma == std::string_view::npos ? std::string_view::npos
                                                 : comma - start));
      if (!RegisterPath(item, ctx)) return;
      if (comma == std::string_view::npos) break;
      start = comma + 1;
    }
  }

  // `@layer name { ... }` or `@layer { ... }`: registers the layer and sets up
  // `child` for the block's contents. False after a refusal.
  bool EnterLayerBlock(std::string_view prelude, const Context& ctx,
                       Context* child) {
    std::string_view name = TrimSpaceAndComments(prelude);
    if (name.empty()) {
      RegisterAnonymous(ctx);
      child->in_anonymous = true;
      return true;
    }
    if (!RegisterPath(name, ctx)) return false;
    child->parent = Qualify(ctx.parent, name);
    return true;
  }

  // `@import <url> [layer | layer(name)]? [supports(...)]? <media>?`
  void HandleImport(std::string_view prelude, const Context& ctx,
                    std::string_view base_url, int import_depth) {
    size_t i = SkipSpaceAndComments(prelude, 0, prelude.size());
    std::string href;
    if (StartsWithCi(prelude, i, "url(")) {
      size_t close = prelude.find(')', i + 4);
      if (close == std::string_view::npos) return;  // invalid: ignored
      std::string_view inner =
          TrimSpaceAndComments(prelude.substr(i + 4, close - i - 4));
      if (inner.size() >= 2 && (inner[0] == '"' || inner[0] == '\'') &&
          inner.back() == inner[0]) {
        inner = inner.substr(1, inner.size() - 2);
      }
      href = std::string(inner);
      i = close + 1;
    } else if (i < prelude.size() &&
               (prelude[i] == '"' || prelude[i] == '\'')) {
      size_t close = SkipString(prelude, i);
      if (close < i + 2 || prelude[close - 1] != prelude[i]) return;
      href = std::string(prelude.substr(i + 1, close - i - 2));
      i = close;
    } else {
      return;  // not an @import a browser would honour
    }

    // `l\61yer(x)` is a layer() condition to a browser; do not read escapes.
    if (prelude.substr(i).find('\\') != std::string_view::npos) {
      Fail("an @import condition is written with an escape");
      return;
    }
    Context child = ctx;
    bool named = false;
    bool anonymous = false;
    std::string_view layer_name;
    i = SkipSpaceAndComments(prelude, i, prelude.size());
    if (StartsWithCi(prelude, i, "layer")) {
      const size_t after = i + 5;
      if (after < prelude.size() && prelude[after] == '(') {
        size_t close = prelude.find(')', after);
        if (close == std::string_view::npos) {
          Fail("an @import layer() condition is not readable");
          return;
        }
        layer_name =
            TrimSpaceAndComments(prelude.substr(after + 1, close - after - 1));
        named = true;
        i = close + 1;
      } else if (after >= prelude.size() || IsCssSpace(prelude[after]) ||
                 StartsComment(prelude, after)) {
        anonymous = true;
        i = after;
      }
    }
    i = SkipSpaceAndComments(prelude, i, prelude.size());
    if (StartsWithCi(prelude, i, "supports(")) {
      child.conditional = true;
      size_t close = FindParenEnd(prelude, i + 8);
      i = close;
    }
    std::string_view media = TrimSpaceAndComments(prelude.substr(i));
    if (!media.empty() && !IsAllMedia(media)) child.conditional = true;

    if (named) {
      if (!RegisterPath(layer_name, child)) return;
      child.parent = Qualify(child.parent, layer_name);
    } else if (anonymous) {
      RegisterAnonymous(child);
      child.in_anonymous = true;
      // The imported sheet's declarations are in an anonymous layer, and
      // nothing here reads them: assume an `!important` among them.
      important_in_anonymous_ = true;
    }
    // Nothing inside an anonymous layer can be named, so its content does not
    // matter to the order.
    if (child.in_anonymous) return;

    if (collect_only_) {
      Fail("the block carries an @import");
      return;
    }
    if (import_depth >= kMaxImportDepth) {
      Fail("@import nesting is deeper than the flattener follows");
      return;
    }
    std::optional<LayerOrderImport> imported;
    if (lookup_ != nullptr && *lookup_) imported = (*lookup_)(base_url, href);
    if (!imported.has_value()) {
      if (!failed_) import_missing_ = true;
      Fail("the sheet @import-ed as \"" + href + "\" is not available");
      return;
    }
    WalkSheet(imported->css, child, imported->url, import_depth + 1);
  }

  // One past the ')' matching the '(' at s[open].
  static size_t FindParenEnd(std::string_view s, size_t open) {
    int depth = 0;
    size_t i = open;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"' || c == '\'') {
        i = SkipString(s, i);
        continue;
      }
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '(') ++depth;
      if (c == ')' && --depth == 0) return i + 1;
      ++i;
    }
    return s.size();
  }

  static std::string Qualify(std::string_view parent, std::string_view name) {
    if (parent.empty()) return std::string(name);
    std::string out(parent);
    out.push_back('.');
    out.append(name);
    return out;
  }

  // Register a (possibly dotted) name under ctx.parent. False after a refusal.
  bool RegisterPath(std::string_view name, const Context& ctx) {
    std::vector<std::string_view> segs = SplitLayerName(name);
    if (segs.empty()) {
      Fail("a layer name is not readable: \"" + std::string(name) + "\"");
      return false;
    }
    if (ctx.in_anonymous) return true;
    std::string parent = ctx.parent;
    for (std::string_view seg : segs) {
      std::string full = Qualify(parent, seg);
      if (!Register(full, parent, ctx.conditional)) return false;
      parent = std::move(full);
    }
    return true;
  }

  bool Register(const std::string& full, const std::string& parent,
                bool conditional) {
    if (seen_.count(full) != 0) return true;
    if (!collect_only_) {
      if (conditional) {
        Fail("layer " + full +
             " is first declared under a condition that may not apply");
        return false;
      }
      if (anonymous_parents_.count(parent) != 0) {
        Fail("an anonymous layer comes before the first mention of layer " +
             full);
        return false;
      }
    }
    seen_.insert(full);
    names_.push_back(full);
    return true;
  }

  void RegisterAnonymous(const Context& ctx) {
    saw_anonymous_ = true;
    if (!ctx.in_anonymous) anonymous_parents_.insert(ctx.parent);
  }

  const LayerOrderImportLookup* lookup_;
  const bool collect_only_;
  bool failed_ = false;
  // The first failure was an @import not in cache.
  bool import_missing_ = false;
  bool important_in_anonymous_ = false;
  bool saw_anonymous_ = false;
  std::string reason_;
  std::vector<std::string> names_;
  std::unordered_set<std::string> seen_;
  // Parents (fully qualified, "" for the top level) that already have an
  // anonymous child.
  std::unordered_set<std::string> anonymous_parents_;
};

}  // namespace

bool LayerOrderMediaIsUnconditional(std::string_view media) {
  std::string_view m = TrimSpaceAndComments(media);
  return m.empty() || net_instaweb::StringCaseEqual(m, "all");
}

std::string CascadeLayerOrder::Statement() const {
  if (names.empty()) return {};
  std::string out = "@layer ";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out.push_back(',');
    out.append(names[i]);
  }
  out.push_back(';');
  return out;
}

namespace {
bool TextHasAnonymousLayerBlock(std::string_view css);
}  // namespace

std::string CascadeLayerOrder::ValidationBinding(
    std::string_view combined_css) const {
  if (!CriticalCssMayUseCascadeLayer(combined_css)) return {};
  // "a2": the extractor leaves `!important` declarations inside anonymous
  // layers out of the block, so on a sheet that may have one
  // the block differs from what older records validated, proven or not.
  const bool a2 = CriticalCssMayCarryImportantInAnonymousLayer(combined_css);
  // "u2": the block goes after the sheets and the sheet has an anonymous
  // layer, so the extractor leaves every anonymous-layer rule out of the
  // block. Keyed on the combined sheet, as the extractor's
  // first decision is; a block that only the post-check on the finished block
  // moves to that mode is not marked.
  const bool u2 = CssTextHasAnonymousLayer(combined_css) &&
                  !CriticalCssBlockGoesFirst(combined_css, *this);
  if (!proven) {
    std::string out = "unproven";
    if (a2) out += " a2";
    if (u2) out += " u2";
    return out;
  }
  // "r2": the block is derived with @media retention narrowed to the device
  // class's windows. That changes which rules the block holds
  // (and, through force-include's relaxed match, can add one), so records
  // made before it describe another block and are made again. Retention
  // never narrows on CSS with an anonymous layer, so the two markers never
  // appear together; the order below is fixed anyway.
  std::string out = "proven ";
  if (CriticalCssRetentionMayNarrow(combined_css, *this)) out += "r2 ";
  if (a2) out += "a2 ";
  if (u2) out += "u2 ";
  return out + Statement();
}

bool CssTextHasImportantBang(std::string_view css) {
  for (size_t i = css.find('!'); i != std::string_view::npos;
       i = css.find('!', i + 1)) {
    size_t j = SkipSpaceAndComments(css, i + 1, css.size());
    // The keyword as written: name characters and escapes. A browser decodes
    // escapes anywhere in it (`!\69mportant`, `!i\6dportant`, `!imp\ortant`
    // are all `!important` in Chromium); any backslash counts as a possible
    // one rather than decoding it here.
    std::string word;
    bool escaped = false;
    while (j < css.size()) {
      if (css[j] == '\\') {
        escaped = true;
        j += 2;
        continue;
      }
      if (!IsNameChar(css[j])) break;
      word.push_back(css[j]);
      ++j;
    }
    if (escaped || net_instaweb::StringCaseEqual(word, "important")) {
      return true;
    }
  }
  return false;
}

bool CriticalCssMayCarryImportantInAnonymousLayer(std::string_view css) {
  Walker walker(nullptr, /*collect_only=*/true);
  walker.WalkSheet(css, Context{}, "", 0);
  if (walker.important_in_anonymous()) return true;
  // A walk that stopped early may not have reached the layer: answer from the
  // text, over-inclusively.
  if (!walker.failed()) return false;
  return CriticalCssMayUseCascadeLayer(css) && CssTextHasImportantBang(css);
}

CascadeLayerOrder ComputeCascadeLayerOrder(
    const std::vector<LayerOrderSource>& sources,
    const LayerOrderImportLookup& lookup) {
  CascadeLayerOrder out;
  Walker walker(&lookup, /*collect_only=*/false);
  // A script seen so far that may insert a stylesheet where it stands.
  bool script_may_insert = false;
  for (size_t n = 0; n < sources.size(); ++n) {
    const LayerOrderSource& src = sources[n];
    if (src.is_script) {
      script_may_insert = true;
      continue;
    }
    if (!src.available) {
      out.reason = "stylesheet source " + std::to_string(n + 1) + ": " +
                   (src.unavailable_reason.empty()
                        ? std::string("its content is not known")
                        : src.unavailable_reason);
      return out;
    }
    Context ctx;
    ctx.conditional = src.conditional;
    const size_t known_before = walker.names().size();
    walker.WalkSheet(src.css, ctx, src.base_url, 0);
    if (!walker.failed() && script_may_insert &&
        walker.names().size() > known_before) {
      // A sheet the script inserts would stand before this one, and a layer it
      // declares could come ahead of the ones first declared here.
      walker.Fail(
          "a script before it may insert a stylesheet, and it declares "
          "a new layer");
    }
    if (walker.failed()) {
      out.reason =
          "stylesheet source " + std::to_string(n + 1) + ": " + walker.reason();
      out.import_missing = walker.import_missing();
      return out;
    }
  }
  out.proven = true;
  out.reason.clear();
  out.names = std::move(walker.names());
  return out;
}

bool CriticalCssMayUseCascadeLayer(std::string_view css) {
  if (CriticalCssNamesCascadeLayer(css)) return true;
  // An anonymous `@layer {`: textual, over-inclusive like the detector above.
  // Also any at-keyword with an escape in it: `@l\61yer` is @layer to a
  // browser, and neither detector decodes escapes.
  for (size_t i = css.find('@'); i != std::string_view::npos;
       i = css.find('@', i + 1)) {
    if (StartsWithCi(css, i, "@layer") &&
        (i + 6 >= css.size() || !IsNameChar(css[i + 6]))) {
      return true;
    }
    size_t j = i + 1;
    while (j < css.size() && IsNameChar(css[j])) ++j;
    if (j < css.size() && css[j] == '\\') return true;
  }
  return false;
}

CriticalCssLayerPlacement DecideCriticalCssLayerPlacement(
    std::string_view block, const CascadeLayerOrder& order) {
  CriticalCssLayerPlacement out;
  if (!CriticalCssMayUseCascadeLayer(block)) return out;
  out.keep_fallback = true;
  if (!order.proven) return out;
  Walker walker(nullptr, /*collect_only=*/true);
  walker.WalkSheet(block, Context{}, "", 0);
  if (walker.failed()) return out;
  const std::unordered_set<std::string> known(order.names.begin(),
                                              order.names.end());
  for (const std::string& name : walker.names()) {
    if (known.count(name) == 0) return out;
  }
  out.keep_fallback = false;
  out.prefix = order.Statement();
  return out;
}

bool CriticalCssBlockGoesFirst(std::string_view combined_css,
                               const CascadeLayerOrder& order) {
  return !DecideCriticalCssLayerPlacement(combined_css, order).keep_fallback;
}

namespace {

// `@layer` followed (after whitespace and comments) by `{`, or an at-rule
// with a block whose keyword is written with an escape: textual and
// over-inclusive (a comment or a string counts), which only makes retention
// more conservative.
bool TextHasAnonymousLayerBlock(std::string_view css) {
  for (size_t i = css.find('@'); i != std::string_view::npos;
       i = css.find('@', i + 1)) {
    if (StartsWithCi(css, i, "@layer")) {
      size_t j = i + 6;
      if (j < css.size() && IsNameChar(css[j])) continue;
      j = SkipSpaceAndComments(css, j, css.size());
      if (j < css.size() && css[j] == '{') return true;
      continue;
    }
    // An at-keyword written with an escape (`@l\61yer`) is not decoded here,
    // as the placement side does not (CriticalCssMayUseCascadeLayer), so a
    // rule with a block under one may be an anonymous `@layer {`, whatever
    // its prelude. A statement (`@\69mport ...;`) cannot be.
    size_t j = i + 1;
    bool escaped = false;
    while (j < css.size() && (IsNameChar(css[j]) || css[j] == '\\')) {
      escaped = escaped || css[j] == '\\';
      j += css[j] == '\\' ? 2 : 1;
    }
    if (!escaped) continue;
    // The prelude's end, read past comments and strings: `@l\61yer/*;*/{` is
    // a block, not a statement.
    const size_t stop = FindPreludeEnd(css, j, css.size());
    if (stop < css.size() && css[stop] == '{') return true;
  }
  return false;
}

// `@import ... layer` without a name: the word `layer` (not `layer(`) in an
// @import rule's prelude. Textual and over-inclusive.
bool TextHasAnonymousImportLayer(std::string_view css) {
  for (size_t i = css.find('@'); i != std::string_view::npos;
       i = css.find('@', i + 1)) {
    if (!StartsWithCi(css, i, "@import")) continue;
    size_t end = css.find(';', i);
    if (end == std::string_view::npos) end = css.size();
    const std::string_view rule = css.substr(i + 7, end - i - 7);
    for (size_t k = 0; k + 5 <= rule.size(); ++k) {
      if (!StartsWithCi(rule, k, "layer")) continue;
      if (k > 0 && IsNameChar(rule[k - 1])) continue;
      size_t after = k + 5;
      if (after < rule.size() && IsNameChar(rule[after])) continue;
      after = SkipSpaceAndComments(rule, after, rule.size());
      if (after >= rule.size() || rule[after] != '(') return true;
    }
  }
  return false;
}

}  // namespace

bool CssAtRuleKeywordHasEscape(std::string_view rule) {
  size_t i = 0;
  while (i < rule.size() && IsCssSpace(rule[i])) ++i;
  if (i >= rule.size() || rule[i] != '@') return false;
  for (++i; i < rule.size(); ++i) {
    if (rule[i] == '\\') return true;
    if (!IsNameChar(rule[i])) return false;
  }
  return false;
}

bool CssTextHasAnonymousLayer(std::string_view css) {
  return TextHasAnonymousLayerBlock(css) || TextHasAnonymousImportLayer(css);
}

bool CriticalCssRetentionMayNarrow(std::string_view css,
                                   const CascadeLayerOrder& order) {
  if (!CriticalCssMayUseCascadeLayer(css)) return true;
  if (!order.proven) return false;
  if (TextHasAnonymousLayerBlock(css)) return false;
  Walker walker(nullptr, /*collect_only=*/true);
  walker.WalkSheet(css, Context{}, "", 0);
  if (walker.failed() || walker.saw_anonymous()) return false;
  const std::unordered_set<std::string> known(order.names.begin(),
                                              order.names.end());
  for (const std::string& name : walker.names()) {
    if (known.count(name) == 0) return false;
  }
  return true;
}

}  // namespace pagespeed
