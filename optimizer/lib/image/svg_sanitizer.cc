// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Sanitizer Implementation
//
// Lightweight state-machine XML parser designed for VTracer output.
// Allowlist-only: unknown elements/attributes are silently stripped.

#include "lib/image/svg_sanitizer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pagespeed {
namespace {

// -----------------------------------------------------------------
// Allowlists (case-sensitive, SVG is XML)
// -----------------------------------------------------------------

const std::unordered_set<std::string_view>& AllowedElements() {
  static const auto* kSet = new std::unordered_set<std::string_view>{
      "svg",      "path",           "rect",           "circle", "ellipse",
      "line",     "polyline",       "polygon",        "g",      "defs",
      "clipPath", "linearGradient", "radialGradient", "stop",
  };
  return *kSet;
}

// Elements whose entire subtree is stripped (dangerous).
const std::unordered_set<std::string_view>& DangerousElements() {
  static const auto* kSet = new std::unordered_set<std::string_view>{
      "script",
      "style",
      "foreignObject",
      "use",
      "animate",
      "set",
      "a",
      "metadata",
      "image",
      "animateTransform",
      "animateMotion",
      "animateColor",
  };
  return *kSet;
}

const std::unordered_set<std::string_view>& AllowedAttributes() {
  static const auto* kSet = new std::unordered_set<std::string_view>{
      "d",
      "fill",
      "stroke",
      "opacity",
      "fill-opacity",
      "stroke-opacity",
      "transform",
      "viewBox",
      "width",
      "height",
      "cx",
      "cy",
      "r",
      "rx",
      "ry",
      "x",
      "y",
      "x1",
      "y1",
      "x2",
      "y2",
      "points",
      "offset",
      "stop-color",
      "stop-opacity",
      "clip-path",
      "fill-rule",
      "stroke-width",
      "stroke-linecap",
      "stroke-linejoin",
      "id",
      "class",
      "xmlns",
      "shape-rendering",
  };
  return *kSet;
}

// -----------------------------------------------------------------
// Utility helpers
// -----------------------------------------------------------------

bool IsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool IsNameStartChar(char c) {
  return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_' ||
         c == ':';
}

bool IsNameChar(char c) {
  return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_' ||
         c == ':' || c == '-' || c == '.';
}

// Skip whitespace, return new position.
size_t SkipWS(std::string_view s, size_t pos) {
  while (pos < s.size() && IsWhitespace(s[pos])) ++pos;
  return pos;
}

// Check if s starts with prefix at position pos (case-sensitive).
bool StartsWith(std::string_view s, size_t pos, std::string_view prefix) {
  if (pos + prefix.size() > s.size()) return false;
  return s.substr(pos, prefix.size()) == prefix;
}

// Read a name token starting at pos.
std::string_view ReadName(std::string_view s, size_t pos, size_t* end) {
  size_t start = pos;
  while (pos < s.size() && IsNameChar(s[pos])) ++pos;
  *end = pos;
  return s.substr(start, pos - start);
}

// -----------------------------------------------------------------
// Path data coordinate rounding
// -----------------------------------------------------------------

// Round a decimal number string to `precision` decimal places.
// Returns the rounded string representation.
std::string RoundNumber(std::string_view num_str, int precision) {
  if (precision < 0) precision = 0;

  // Parse the number.
  double val = 0.0;
  bool negative = false;
  size_t i = 0;
  if (i < num_str.size() && (num_str[i] == '-' || num_str[i] == '+')) {
    negative = (num_str[i] == '-');
    ++i;
  }
  // Integer part.
  while (i < num_str.size() &&
         (std::isdigit(static_cast<unsigned char>(num_str[i])) != 0)) {
    val = val * 10.0 + (num_str[i] - '0');
    ++i;
  }
  // Fractional part.
  if (i < num_str.size() && num_str[i] == '.') {
    ++i;
    double frac = 0.0;
    double div = 1.0;
    while (i < num_str.size() &&
           (std::isdigit(static_cast<unsigned char>(num_str[i])) != 0)) {
      frac = frac * 10.0 + (num_str[i] - '0');
      div *= 10.0;
      ++i;
    }
    val += frac / div;
  }
  if (negative) val = -val;

  // Clamp to prevent UB from out-of-range double → long long cast.
  constexpr double kMaxCoord = 1e15;
  if (val > kMaxCoord) val = kMaxCoord;
  if (val < -kMaxCoord) val = -kMaxCoord;

  // Round to `precision` decimal places.
  double factor = std::pow(10.0, precision);
  double rounded = std::round(val * factor) / factor;

  // Format the result.
  // We build it manually to avoid locale issues and to control
  // trailing zeros.
  if (precision == 0) {
    auto iv = static_cast<long long>(rounded);
    return std::to_string(iv);
  }

  bool neg_out = rounded < 0.0;
  double abs_val = std::fabs(rounded);
  auto int_part = static_cast<long long>(abs_val);
  double frac_part = abs_val - static_cast<double>(int_part);

  std::string result;
  if (neg_out) result += '-';
  result += std::to_string(int_part);
  result += '.';

  for (int p = 0; p < precision; ++p) {
    frac_part *= 10.0;
    int digit = static_cast<int>(frac_part);
    if (digit > 9) digit = 9;
    result += static_cast<char>('0' + digit);
    frac_part -= digit;
  }

  return result;
}

// Round all numeric coordinate values in an SVG path `d` attribute.
// Path commands: M, m, L, l, H, h, V, v, C, c, S, s, Q, q, T, t,
//                A, a, Z, z
// We only round the floating-point numbers, leaving command letters
// intact.
std::string RoundPathCoordinates(std::string_view d, int precision) {
  std::string out;
  out.reserve(d.size());
  size_t i = 0;
  while (i < d.size()) {
    char c = d[i];
    // If this is the start of a number (digit, sign before digit,
    // or dot before digit).
    bool is_num_start = false;
    if ((std::isdigit(static_cast<unsigned char>(c)) != 0) || c == '.') {
      is_num_start = true;
    } else if ((c == '-' || c == '+') && i + 1 < d.size()) {
      char next = d[i + 1];
      if ((std::isdigit(static_cast<unsigned char>(next)) != 0) ||
          next == '.') {
        is_num_start = true;
      }
    }

    if (is_num_start) {
      // Collect the full number.
      size_t start = i;
      if (d[i] == '-' || d[i] == '+') ++i;
      while (i < d.size() &&
             (std::isdigit(static_cast<unsigned char>(d[i])) != 0))
        ++i;
      if (i < d.size() && d[i] == '.') {
        ++i;
        while (i < d.size() &&
               (std::isdigit(static_cast<unsigned char>(d[i])) != 0))
          ++i;
      }
      std::string_view num = d.substr(start, i - start);
      // Only round if it has a decimal point (coordinates with
      // fractions).
      if (num.find('.') != std::string_view::npos) {
        out += RoundNumber(num, precision);
      } else {
        out.append(num);
      }
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

// -----------------------------------------------------------------
// Post-processing
// -----------------------------------------------------------------

// Collapse runs of whitespace to a single space.
std::string CollapseWhitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool last_ws = false;
  for (char c : s) {
    if (IsWhitespace(c)) {
      if (!last_ws) {
        out += ' ';
        last_ws = true;
      }
    } else {
      out += c;
      last_ws = false;
    }
  }
  return out;
}

// Remove empty <g></g> groups (possibly with whitespace between).
std::string RemoveEmptyGroups(const std::string& s) {
  std::string result = s;
  // Iterate until no more removals (nested empties).
  for (;;) {
    std::string prev = result;
    // Find <g> ... </g> where ... is only whitespace.
    size_t pos = 0;
    std::string next;
    next.reserve(result.size());
    while (pos < result.size()) {
      if (StartsWith(result, pos, "<g>") || StartsWith(result, pos, "<g ")) {
        // Find the end of the opening tag.
        size_t tag_end = result.find('>', pos);
        if (tag_end == std::string::npos) {
          next += result[pos];
          ++pos;
          continue;
        }
        // Check if self-closing.
        if (result[tag_end - 1] == '/') {
          // <g ... /> -- self-closing empty group, strip it.
          pos = tag_end + 1;
          continue;
        }
        tag_end += 1;  // Past '>'.
        // Skip whitespace after opening tag.
        size_t inner = tag_end;
        while (inner < result.size() && IsWhitespace(result[inner])) ++inner;
        // Check for </g>.
        if (StartsWith(result, inner, "</g>")) {
          // Empty group -- skip it entirely.
          pos = inner + 4;
          continue;
        }
      }
      next += result[pos];
      ++pos;
    }
    result = std::move(next);
    if (result == prev) break;
  }
  return result;
}

// -----------------------------------------------------------------
// Main parser: state-machine XML processor
// -----------------------------------------------------------------

struct ParseState {
  std::string_view input;
  size_t pos = 0;
  std::string output;
  SvgSanitizeResult result;

  // Nesting depth inside a dangerous element (skip entire subtree).
  int skip_depth = 0;

  // Nesting depth inside a non-allowed, non-dangerous element.
  // We strip the element tags but keep child content.
  int unknown_depth = 0;

  // Track whether we found the root <svg>.
  bool found_svg = false;

  // SVG bomb guards.
  uint32_t current_depth = 0;
  uint32_t element_count = 0;
  uint32_t max_depth = 256;
  uint32_t max_elements = 100000;
  bool limits_exceeded = false;

  [[nodiscard]] bool Eof() const {
    return pos >= input.size() || limits_exceeded;
  }
  [[nodiscard]] char Peek() const { return input[pos]; }
  char Advance() { return input[pos++]; }
};

// Parse a quoted attribute value. pos should be on the quote char.
// Returns the value (without quotes) and advances past closing quote.
std::string_view ParseAttrValue(ParseState& st, size_t* end) {
  if (st.pos >= st.input.size()) {
    *end = st.pos;
    return {};
  }
  char quote = st.input[st.pos];
  if (quote != '"' && quote != '\'') {
    // Unquoted -- collect until whitespace or >.
    size_t start = st.pos;
    while (st.pos < st.input.size() && !IsWhitespace(st.input[st.pos]) &&
           st.input[st.pos] != '>' && st.input[st.pos] != '/')
      ++st.pos;
    *end = st.pos;
    return st.input.substr(start, st.pos - start);
  }
  ++st.pos;  // Skip opening quote.
  size_t start = st.pos;
  while (st.pos < st.input.size() && st.input[st.pos] != quote) ++st.pos;
  size_t val_end = st.pos;
  if (st.pos < st.input.size()) ++st.pos;  // Skip closing quote.
  *end = st.pos;
  return st.input.substr(start, val_end - start);
}

// Parse attributes from current position until '>' or '/>'.
// Returns true if self-closing. Advances pos past '>'.
struct AttrInfo {
  std::string name;
  std::string value;
};

struct TagParseResult {
  std::vector<AttrInfo> attrs;
  bool self_closing = false;
};

TagParseResult ParseAttributes(ParseState& st) {
  TagParseResult res;
  while (!st.Eof()) {
    st.pos = SkipWS(st.input, st.pos);
    if (st.Eof()) break;

    if (st.input[st.pos] == '/') {
      res.self_closing = true;
      ++st.pos;
      // Skip to >.
      while (!st.Eof() && st.input[st.pos] != '>') ++st.pos;
      if (!st.Eof()) ++st.pos;
      return res;
    }
    if (st.input[st.pos] == '>') {
      ++st.pos;
      return res;
    }

    // Read attribute name.
    if (!IsNameStartChar(st.input[st.pos]) && st.input[st.pos] != '-') {
      // Skip unexpected character.
      ++st.pos;
      continue;
    }
    size_t name_end;
    std::string_view name = ReadName(st.input, st.pos, &name_end);
    st.pos = name_end;

    std::string value;
    st.pos = SkipWS(st.input, st.pos);
    if (!st.Eof() && st.input[st.pos] == '=') {
      ++st.pos;  // Skip '='.
      st.pos = SkipWS(st.input, st.pos);
      size_t val_end;
      std::string_view val = ParseAttrValue(st, &val_end);
      st.pos = val_end;
      value = std::string(val);
    }

    res.attrs.push_back({std::string(name), std::move(value)});
  }
  return res;
}

// Emit an opening tag with allowed attributes.
void EmitOpenTag(ParseState& st, std::string_view tag,
                 const TagParseResult& parsed, bool is_svg_root,
                 const SvgSanitizeConfig& config) {
  st.output += '<';
  st.output.append(tag);

  bool has_width = false;
  bool has_height = false;

  for (const auto& attr : parsed.attrs) {
    // Strip event handlers (on* — case-insensitive to block XSS via
    // uppercase variants like ONCLICK or OnLoad).
    if (attr.name.size() >= 2 && (attr.name[0] == 'o' || attr.name[0] == 'O') &&
        (attr.name[1] == 'n' || attr.name[1] == 'N')) {
      continue;
    }
    // Strip href / xlink:href (case-sensitive is fine — uppercase variants
    // like HREF are not in AllowedAttributes and get stripped below).
    if (attr.name == "href" || attr.name == "xlink:href") {
      continue;
    }
    // Strip style attribute.
    if (attr.name == "style") {
      continue;
    }

    if (AllowedAttributes().count(attr.name) == 0u) {
      continue;
    }

    if (attr.name == "width") has_width = true;
    if (attr.name == "height") has_height = true;

    // For the root <svg>, override width/height if config says so.
    if (is_svg_root && attr.name == "width" && config.source_width > 0) {
      st.output += " width=\"";
      st.output += std::to_string(config.source_width);
      st.output += '"';
      continue;
    }
    if (is_svg_root && attr.name == "height" && config.source_height > 0) {
      st.output += " height=\"";
      st.output += std::to_string(config.source_height);
      st.output += '"';
      continue;
    }

    st.output += ' ';
    st.output += attr.name;
    st.output += "=\"";
    // Escape special characters to prevent attribute injection and
    // ensure valid XML attribute values.
    for (char c : attr.value) {
      if (c == '"') {
        st.output += "&quot;";
      } else if (c == '&') {
        st.output += "&amp;";
      } else if (c == '<') {
        st.output += "&lt;";
      } else {
        st.output += c;
      }
    }
    st.output += '"';
  }

  // Add missing width/height on root <svg>.
  if (is_svg_root) {
    if (!has_width && config.source_width > 0) {
      st.output += " width=\"";
      st.output += std::to_string(config.source_width);
      st.output += '"';
    }
    if (!has_height && config.source_height > 0) {
      st.output += " height=\"";
      st.output += std::to_string(config.source_height);
      st.output += '"';
    }
    if (config.crisp_edges) {
      st.output += " shape-rendering=\"crispEdges\"";
    }
  }

  if (parsed.self_closing) {
    st.output += "/>";
  } else {
    st.output += '>';
  }
}

// Main parse loop.
void Parse(ParseState& st, const SvgSanitizeConfig& config) {
  while (!st.Eof()) {
    // ----- Text content outside tags -----
    if (st.Peek() != '<') {
      if (st.skip_depth == 0) {
        st.output += st.Advance();
      } else {
        st.Advance();  // Discard.
      }
      continue;
    }

    // ----- We have '<' -----
    // Check for comment: <!-- ... -->
    if (StartsWith(st.input, st.pos, "<!--")) {
      size_t end = st.input.find("-->", st.pos + 4);
      if (end == std::string_view::npos) {
        st.pos = st.input.size();
      } else {
        st.pos = end + 3;
      }
      continue;
    }

    // Check for CDATA: <![CDATA[ ... ]]>
    if (StartsWith(st.input, st.pos, "<![CDATA[")) {
      size_t end = st.input.find("]]>", st.pos + 9);
      if (end == std::string_view::npos) {
        st.pos = st.input.size();
      } else {
        st.pos = end + 3;
      }
      // Strip CDATA entirely.
      continue;
    }

    // Check for DOCTYPE / ENTITY: <!DOCTYPE ... > or <!ENTITY ...>
    if (StartsWith(st.input, st.pos, "<!")) {
      // Skip to matching >.  Handle nested brackets for DOCTYPE
      // with internal subset: <!DOCTYPE svg [ ... ]>
      st.pos += 2;
      int bracket_depth = 0;
      while (!st.Eof()) {
        if (st.Peek() == '[') {
          ++bracket_depth;
          ++st.pos;
        } else if (st.Peek() == ']') {
          --bracket_depth;
          ++st.pos;
        } else if (st.Peek() == '>' && bracket_depth <= 0) {
          ++st.pos;
          break;
        } else {
          ++st.pos;
        }
      }
      continue;
    }

    // Check for processing instruction: <?...?>
    if (StartsWith(st.input, st.pos, "<?")) {
      size_t end = st.input.find("?>", st.pos + 2);
      if (end == std::string_view::npos) {
        st.pos = st.input.size();
      } else {
        st.pos = end + 2;
      }
      continue;
    }

    // ----- Closing tag: </tagname> -----
    if (StartsWith(st.input, st.pos, "</")) {
      st.pos += 2;
      size_t name_end;
      std::string_view tag = ReadName(st.input, st.pos, &name_end);
      st.pos = name_end;
      // Skip to >.
      while (!st.Eof() && st.Peek() != '>') ++st.pos;
      if (!st.Eof()) ++st.pos;

      if (st.current_depth > 0) --st.current_depth;

      if (st.skip_depth > 0) {
        // Check if this closes the dangerous element.
        --st.skip_depth;
        continue;
      }

      if (st.unknown_depth > 0) {
        --st.unknown_depth;
        continue;
      }

      // Emit closing tag for allowed elements.
      if (AllowedElements().count(tag) != 0u) {
        st.output += "</";
        st.output.append(tag);
        st.output += '>';
      }
      continue;
    }

    // ----- Opening tag: <tagname ...> or <tagname .../> -----
    st.pos += 1;  // Skip '<'.
    if (st.Eof() || (!IsNameStartChar(st.Peek()) && st.Peek() != '-')) {
      // Malformed -- just skip '<'.
      if (st.skip_depth == 0) st.output += '<';
      continue;
    }

    size_t name_end;
    std::string_view tag = ReadName(st.input, st.pos, &name_end);
    st.pos = name_end;

    // Parse attributes (advances past '>').
    TagParseResult parsed = ParseAttributes(st);

    // SVG bomb guards: count every element and track nesting depth.
    ++st.element_count;
    if (!parsed.self_closing) ++st.current_depth;
    if (st.element_count > st.max_elements || st.current_depth > st.max_depth) {
      st.limits_exceeded = true;
      st.result.error_message = st.element_count > st.max_elements
                                    ? "SVG element count limit exceeded"
                                    : "SVG nesting depth limit exceeded";
      return;
    }

    // --- Decide what to do with this tag ---
    if (st.skip_depth > 0) {
      // Inside a dangerous subtree -- skip everything.
      if (!parsed.self_closing) {
        ++st.skip_depth;
      }
      ++st.result.elements_stripped;
      continue;
    }

    if (DangerousElements().count(tag) != 0u) {
      // Dangerous element -- skip its entire subtree.
      ++st.result.elements_stripped;
      if (!parsed.self_closing) {
        st.skip_depth = 1;
      }
      continue;
    }

    if (AllowedElements().count(tag) == 0u) {
      // Unknown element -- strip the tag but keep children.
      ++st.result.elements_stripped;
      if (!parsed.self_closing) {
        ++st.unknown_depth;
      }
      continue;
    }

    // Allowed element -- emit it.
    bool is_svg_root = (tag == "svg" && !st.found_svg);
    if (tag == "svg") st.found_svg = true;
    if (tag == "path") ++st.result.path_count;

    EmitOpenTag(st, tag, parsed, is_svg_root, config);
  }
}

}  // namespace

// -----------------------------------------------------------------
// Public API
// -----------------------------------------------------------------

SvgSanitizeResult SanitizeSvg(std::string_view svg_input,
                              const SvgSanitizeConfig& config) {
  SvgSanitizeResult result;

  if (svg_input.empty()) {
    result.error_message = "Empty SVG input";
    return result;
  }

  ParseState st;
  st.input = svg_input;
  st.output.reserve(svg_input.size());
  st.max_depth = config.max_depth;
  st.max_elements = config.max_elements;

  Parse(st, config);

  if (st.limits_exceeded) {
    result.error_message = st.result.error_message;
    result.elements_stripped = st.result.elements_stripped;
    return result;
  }

  if (!st.found_svg) {
    result.error_message = "No <svg> element found";
    result.elements_stripped = st.result.elements_stripped;
    return result;
  }

  result = std::move(st.result);

  // Post-processing: round path coordinates.
  if (config.coordinate_precision >= 0) {
    // Find all d="..." attributes and round coordinates.
    std::string& svg = st.output;
    std::string processed;
    processed.reserve(svg.size());
    size_t i = 0;
    while (i < svg.size()) {
      // Look for d=" pattern.
      if (i + 2 < svg.size() && svg[i] == 'd' && svg[i + 1] == '=' &&
          svg[i + 2] == '"') {
        // Ensure 'd' is preceded by whitespace (attribute context).
        if (i == 0 || IsWhitespace(svg[i - 1])) {
          processed += "d=\"";
          i += 3;  // Past d="
          // Find closing quote.
          size_t end = svg.find('"', i);
          if (end == std::string::npos) end = svg.size();
          std::string_view path_data = std::string_view(svg).substr(i, end - i);
          processed +=
              RoundPathCoordinates(path_data, config.coordinate_precision);
          processed += '"';
          i = (end < svg.size()) ? end + 1 : end;
          continue;
        }
      }
      processed += svg[i];
      ++i;
    }
    st.output = std::move(processed);
  }

  // Collapse whitespace.
  st.output = CollapseWhitespace(st.output);

  // Remove empty groups.
  st.output = RemoveEmptyGroups(st.output);

  result.sanitized_svg = std::move(st.output);
  result.success = true;
  return result;
}

}  // namespace pagespeed
