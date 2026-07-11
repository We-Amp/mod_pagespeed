/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "net/instaweb/rewriter/public/js_replacer.h"

#include <cstddef>
#include <cstdint>

#include "base/logging.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/js/js_keywords.h"

using pagespeed::JsKeywords;
using pagespeed::js::JsTokenizer;

namespace net_instaweb {

namespace {

enum State : std::uint8_t {
  kStart,
  kSawIdent,
  kSawIdentDot,
  kSawIdentDotIdent,
  kSawIdentDotIdentEquals
};

void ResetState(State* state, GoogleString* maybe_object,
                GoogleString* maybe_field) {
  *state = kStart;
  maybe_object->clear();
  maybe_field->clear();
}

// Appends the UTF-8 encoding of code_point to *out.
void AppendUtf8(std::uint32_t code_point, GoogleString* out) {
  if (code_point <= 0x7F) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

// Parses exactly count hex digits from value starting at *pos.  On success
// advances *pos past them and returns the parsed value in *result.
bool ParseHex(StringPiece value, size_t* pos, int count,
              std::uint32_t* result) {
  if (*pos + count > value.length()) {
    return false;
  }
  std::uint32_t v = 0;
  for (int i = 0; i < count; ++i) {
    char ch = value[*pos + i];
    std::uint32_t digit;
    if (ch >= '0' && ch <= '9') {
      digit = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      digit = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      digit = ch - 'A' + 10;
    } else {
      return false;
    }
    v = (v << 4) | digit;
  }
  *pos += count;
  *result = v;
  return true;
}

// Decodes a JS string-literal body (outer quotes already removed, inner escapes
// intact) into its logical value.  Returns false on any unrecognized or
// malformed escape sequence; the caller must then leave the candidate alone.
bool UnescapeJsStringLiteral(StringPiece value, GoogleString* out) {
  out->clear();
  size_t i = 0;
  const size_t n = value.length();
  while (i < n) {
    char c = value[i];
    if (c != '\\') {
      out->push_back(c);
      ++i;
      continue;
    }
    ++i;  // Consume the backslash.
    if (i >= n) {
      return false;  // Dangling backslash.
    }
    char esc = value[i];
    ++i;
    switch (esc) {
      case '\\':
        out->push_back('\\');
        break;
      case '\'':
        out->push_back('\'');
        break;
      case '"':
        out->push_back('"');
        break;
      case '`':
        out->push_back('`');
        break;
      case '/':
        // EscapeToJsStringLiteral emits "\/" to break up "</script"; accept
        // it so decode composes with encode.
        out->push_back('/');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'v':
        out->push_back('\v');
        break;
      case '0':
        // A decoded NUL cannot be re-encoded (EscapeToJsStringLiteral leaves
        // raw NUL bare, which the HTML tokenizer mangles), so bail.
        return false;
      case 'x': {
        std::uint32_t code;
        if (!ParseHex(value, &i, 2, &code)) {
          return false;
        }
        if (code == 0) {
          return false;  // NUL: see the \0 case.
        }
        out->push_back(static_cast<char>(code));
        break;
      }
      case 'u': {
        std::uint32_t code;
        if (!ParseHex(value, &i, 4, &code)) {
          return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
          // High surrogate: must be followed by a low-surrogate \u escape.
          if (i + 1 >= n || value[i] != '\\' || value[i + 1] != 'u') {
            return false;
          }
          i += 2;
          std::uint32_t low;
          if (!ParseHex(value, &i, 4, &low) || low < 0xDC00 || low > 0xDFFF) {
            return false;
          }
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
          return false;  // Lone low surrogate.
        }
        if (code == 0 || code == 0x2028 || code == 0x2029) {
          // NUL (see the \0 case) and LS/PS (a syntax error when emitted raw
          // inside a pre-ES2019 string literal) cannot be re-encoded; bail.
          return false;
        }
        AppendUtf8(code, out);
        break;
      }
      case '\n':
        break;  // Line continuation.
      case '\r':
        if (i < n && value[i] == '\n') {
          ++i;  // Line continuation with CRLF.
        }
        break;
      default:
        return false;  // Unrecognized escape; preserve passthrough behavior.
    }
  }
  return true;
}

}  // namespace.

JsReplacer::~JsReplacer() {}

void JsReplacer::AddPattern(const GoogleString& object,
                            const GoogleString& field,
                            StringRewriter* rewriter) {
  patterns_.emplace_back(object, field, rewriter);
}

bool JsReplacer::Transform(StringPiece in, GoogleString* out) {
  State state = kStart;

  GoogleString maybe_object;
  GoogleString maybe_field;

  pagespeed::js::JsTokenizer tokenizer(js_tokenizer_patterns_, in);
  out->clear();
  while (true) {
    // Note that this may get modified in the switch below.
    StringPiece token;
    GoogleString replacement;  // in case we need to replace token.
    JsKeywords::Type type = tokenizer.NextToken(&token);
    // TODO(morlovich): This only matches object.field, not object['field'].
    switch (type) {
      case JsKeywords::kEndOfInput:
        return true;
      case JsKeywords::kError:
        return false;
      case JsKeywords::kComment:
      case JsKeywords::kWhitespace:
      case JsKeywords::kLineSeparator:
      case JsKeywords::kSemiInsert:
        // Whitespace is just passed through, and doesn't cause state machine
        // transitions.
        break;
      case JsKeywords::kIdentifier:
        switch (state) {
          case kStart:
          case kSawIdent:
          case kSawIdentDotIdent:
          case kSawIdentDotIdentEquals:
            state = kSawIdent;
            token.CopyToString(&maybe_object);
            break;
          case kSawIdentDot:
            state = kSawIdentDotIdent;
            token.CopyToString(&maybe_field);
            break;
        }
        break;
      case JsKeywords::kOperator:
        if (token == ".") {
          switch (state) {
            case kStart:
            case kSawIdentDot:
            case kSawIdentDotIdentEquals:
              // No clue on how some of these could parse.
              ResetState(&state, &maybe_object, &maybe_field);
              break;
            case kSawIdent:
              state = kSawIdentDot;
              break;
            case kSawIdentDotIdent:
              // This is something like a.b. -> so what we thought was a field
              // is now "object".
              state = kSawIdentDot;
              maybe_object = maybe_field;
              maybe_field.clear();
              break;
          }
        } else if (token == "=") {
          switch (state) {
            case kStart:
            case kSawIdent:
            case kSawIdentDot:
            case kSawIdentDotIdentEquals:
              // No clue on how some of these could parse.
              ResetState(&state, &maybe_object, &maybe_field);
              break;

            case kSawIdentDotIdent:
              state = kSawIdentDotIdentEquals;
              break;
          }
        } else {
          // Things other than . and = are uninteresting to us.
          ResetState(&state, &maybe_object, &maybe_field);
        }
        break;
      case JsKeywords::kStringLiteral:
        if (state == kSawIdentDotIdentEquals) {
          if (HandleCandidate(maybe_object, maybe_field, token, &replacement)) {
            token = replacement;
          }
        } else {
          ResetState(&state, &maybe_object, &maybe_field);
        }
        break;
      default:
        // Something unexpected --- reset matching.
        ResetState(&state, &maybe_object, &maybe_field);
    }

    StrAppend(out, token);
  }
}

bool JsReplacer::HandleCandidate(const GoogleString& object,
                                 const GoogleString& field, StringPiece value,
                                 GoogleString* out) {
  // Note that the token still has the quotes; we strip them before invoking
  // the callback and then restore them when serializing.
  CHECK_GE(value.length(), 2) << value;
  char quote = value[0];
  CHECK(quote == '\'' || quote == '"');
  CHECK_EQ(quote, value[value.length() - 1]);
  value.remove_prefix(1);
  value.remove_suffix(1);

  // Check patterns.
  for (int i = 0, n = patterns_.size(); i < n; ++i) {
    const Pattern& pat = patterns_[i];
    if (pat.object == object && pat.field == field) {
      // The candidate is the raw source lexeme with inner escapes intact.
      // Decode it to its logical value, run the rewriter on that, then
      // re-encode for splicing back into a JS string literal.  If the source
      // carries an escape we do not understand, leave the candidate untouched.
      GoogleString rewriter_inout;
      if (!UnescapeJsStringLiteral(value, &rewriter_inout)) {
        return false;
      }
      pat.rewriter->Run(&rewriter_inout);
      GoogleString quote_str(1, quote);
      GoogleString escaped;
      EscapeToJsStringLiteral(rewriter_inout, false /* add_quotes */, &escaped);
      out->clear();
      StrAppend(out, quote_str, escaped, quote_str);
      return true;
    }
  }

  return false;
}

}  // namespace net_instaweb
