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

#include "pagespeed/kernel/js/js_minify.h"

#include "base/logging.h"
//#include "strings/stringpiece_utils.h"
#include "pagespeed/kernel/base/source_map.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/js/js_keywords.h"
#include "pagespeed/kernel/js/js_tokenizer.h"

using pagespeed::JsKeywords;

namespace pagespeed {

namespace js {

namespace {

bool IsNameNumberOrKeyword(JsKeywords::Type type) {
  switch (type) {
    case JsKeywords::kComment:
    case JsKeywords::kWhitespace:
    case JsKeywords::kLineSeparator:
    case JsKeywords::kSemiInsert:
    case JsKeywords::kRegex:
    case JsKeywords::kStringLiteral:
    case JsKeywords::kTemplateLiteral:
    case JsKeywords::kOperator:
    case JsKeywords::kEndOfInput:
    case JsKeywords::kError:
      return false;
    default:
      return true;
  }
}

// Returns true if a linebreak after this keyword always induces semicolon
// insertion, no matter what token follows -- ECMAScript's restricted
// productions, which forbid a LineTerminator between the keyword and its
// operand.  (`yield` is restricted only inside generators, but keeping a
// linebreak after its identifier uses is byte-safe: the output re-parses
// identically.)
bool IsAsiKeyword(JsKeywords::Type type) {
  switch (type) {
    case JsKeywords::kBreak:
    case JsKeywords::kContinue:
    case JsKeywords::kDebugger:
    case JsKeywords::kReturn:
    case JsKeywords::kThrow:
    case JsKeywords::kYield:
      return true;
    default:
      return false;
  }
}

// Updates *line and *col numbers based on the next incremental chunk of text.
// Note: This only works correctly for ASCII text. If text contains multi-byte
// UTF-8 chars, our updates will be incorrect.
void UpdateLineAndCol(StringPiece text, int* line, int* col) {
  for (int i = 0, n = text.size(); i < n; ++i) {
    if (text[i] == '\n') {
      // TODO(sligocki): We should allow all Unicode newline chars.
      *line += 1;
      *col = 0;
    } else {
      // TODO(sligocki): Count number of Unicode chars, not number of bytes.
      *col += 1;
    }
  }
}

bool ShouldRecordStep(const net_instaweb::source_map::MappingVector& mapping,
                      const net_instaweb::source_map::Mapping& next) {
  // Should record first mapping.
  if (mapping.empty()) {
    return true;
  }

  const net_instaweb::source_map::Mapping& prev = mapping.back();
  if (next.gen_line == prev.gen_line) {
    // Should record iff different number of newlines or different num of cols.
    return (next.src_line != prev.src_line ||
            next.gen_col - prev.gen_col != next.src_col - prev.src_col);
  }

  // If line changes, we should record it.
  return true;
}

}  // namespace

JsMinifyingTokenizer::JsMinifyingTokenizer(const JsTokenizerPatterns* patterns,
                                           StringPiece input)
    : tokenizer_(patterns, input),
      whitespace_(kNoWhitespace),
      prev_type_(JsKeywords::kEndOfInput),
      prev_token_(),
      next_type_(JsKeywords::kEndOfInput),
      next_token_(),
      prev_carries_asi_(false),
      next_carries_asi_(false),
      mappings_(nullptr) {}

JsMinifyingTokenizer::JsMinifyingTokenizer(
    const JsTokenizerPatterns* patterns, StringPiece input,
    net_instaweb::source_map::MappingVector* mappings)
    : tokenizer_(patterns, input),
      whitespace_(kNoWhitespace),
      prev_type_(JsKeywords::kEndOfInput),
      prev_token_(),
      next_type_(JsKeywords::kEndOfInput),
      next_token_(),
      prev_carries_asi_(false),
      next_carries_asi_(false),
      mappings_(mappings),
      current_position_(0, 0, 0, 0, 0),
      next_position_(0, 0, 0, 0, 0) {}

JsMinifyingTokenizer::~JsMinifyingTokenizer() {}

JsKeywords::Type JsMinifyingTokenizer::NextToken(StringPiece* token_out) {
  net_instaweb::source_map::Mapping token_out_position;
  const JsKeywords::Type type = NextTokenHelper(token_out, &token_out_position);
  if (mappings_ != nullptr && type != JsKeywords::kEndOfInput &&
      ShouldRecordStep(*mappings_, token_out_position)) {
    mappings_->push_back(token_out_position);
  }
  // Update generated file line and col # with the output token.
  // Note: We use a helper function to avoid having to add this before every
  // return in NextTokenHelper.
  UpdateLineAndCol(*token_out, &current_position_.gen_line,
                   &current_position_.gen_col);
  return type;
}

JsKeywords::Type JsMinifyingTokenizer::NextTokenHelper(
    StringPiece* token_out, net_instaweb::source_map::Mapping* position_out) {
  if (next_type_ != JsKeywords::kEndOfInput) {
    prev_type_ = next_type_;
    prev_token_ = next_token_;
    prev_carries_asi_ = next_carries_asi_;
    next_carries_asi_ = false;
    *token_out = next_token_;
    *position_out = next_position_;
    // next_position_.gen_line and .gen_col are out of date because they were
    // computed in the previous call to NextTokenHelper().
    position_out->gen_line = current_position_.gen_line;
    position_out->gen_col = current_position_.gen_col;

    next_type_ = JsKeywords::kEndOfInput;
    next_token_ = StringPiece();
    return prev_type_;
  }
  net_instaweb::source_map::Mapping first_position = current_position_;
  while (true) {
    StringPiece token;
    const JsKeywords::Type type = tokenizer_.NextToken(&token);
    // Position of start of token
    net_instaweb::source_map::Mapping token_position = current_position_;
    // Update source file line and col # with the consumed input token.
    UpdateLineAndCol(token, &current_position_.src_line,
                     &current_position_.src_col);
    if (type == JsKeywords::kWhitespace) {
      if (whitespace_ == kNoWhitespace) {
        whitespace_ = kSpace;
      }
    } else if (type == JsKeywords::kLineSeparator) {
      whitespace_ = kLinebreak;
    } else if (type == JsKeywords::kSemiInsert) {
      whitespace_ = kNoWhitespace;
      prev_type_ = type;
      prev_token_ = "\n";
      prev_carries_asi_ = false;
      *token_out = prev_token_;
      *position_out = first_position;  // Beginning of whitespace/comments.
      return type;
    } else if (type == JsKeywords::kComment) {
      // Emit comments that look like they might be IE conditional compilation
      // comments; treat all other comments as whitespace.  A leading
      // `#!...` hashbang line is likewise emitted verbatim (including its
      // terminating linebreak, so node-executable scripts stay directly
      // executable).
      //   all comments matching a user-specified pattern.  It might also be
      //   nice to make retaining of IE conditional compilation comments
      //   optional, so we can turn it off for non-IE browsers.
      if (strings::StartsWith(token, "#!")) {
        // A hashbang can only be the very first thing in the input, and it
        // ends with its own linebreak, so it has no neighbour to separate it
        // from at either end.
        *token_out = token;
        *position_out = first_position;  // Beginning of whitespace/comments.
        return type;
      } else if (token.size() >= 6 && strings::StartsWith(token, "/*@") &&
                 strings::EndsWith(token, "@*/")) {
        // A retained IE conditional-compilation comment is emitted as a real
        // token, so it must pass through the same whitespace machinery as any
        // other emitted token.  Two glue hazards otherwise slip through: the
        // leading "/*@" can weld onto a preceding "/" (a regex or division
        // close) to form "//" or "/*", and the trailing "@*/" ends in "/", so
        // a following "/" or "*" token can weld onto it likewise.  Mirror the
        // general-token branch below, treating the comment as an emitted
        // token.
        const JsWhitespace whitespace = whitespace_;
        whitespace_ = kNoWhitespace;
        // A comment is grammatically inert, so a linebreak *after* it must
        // still trigger semicolon insertion when the token *before* it was a
        // restricted-production keyword or a speculatively-classified
        // operator word.  Carry that signal across the comment; preserving a
        // linebreak is always semantics-neutral.
        const bool inherit_asi = IsAsiKeyword(prev_type_) || prev_carries_asi_;
        if (whitespace != kNoWhitespace &&
            (WhitespaceNeededBefore(JsKeywords::kComment, token) ||
             (whitespace == kLinebreak && inherit_asi))) {
          next_type_ = JsKeywords::kComment;
          next_token_ = token;
          next_carries_asi_ = inherit_asi;
          next_position_ = token_position;
          *position_out = first_position;  // Beginning of whitespace/comments.
          if (whitespace == kLinebreak) {
            *token_out = "\n";
            return JsKeywords::kLineSeparator;
          } else {
            *token_out = " ";
            return JsKeywords::kWhitespace;
          }
        }
        prev_type_ = JsKeywords::kComment;
        prev_token_ = token;
        prev_carries_asi_ = inherit_asi;
        *token_out = token;
        *position_out = token_position;
        return type;
      } else {
        // A block comment containing a line terminator counts as a
        // linebreak for semicolon insertion (e.g. "return/*\n*/x" must not
        // become "return x"), so promote the pending whitespace
        // accordingly.  U+2028/U+2029 are line terminators too.
        const bool has_newline =
            token.find('\n') != StringPiece::npos ||
            token.find('\r') != StringPiece::npos ||
            token.find("\xE2\x80\xA8") != StringPiece::npos ||
            token.find("\xE2\x80\xA9") != StringPiece::npos;
        const JsWhitespace whitespace = has_newline ? kLinebreak : kSpace;
        if (whitespace > whitespace_) {
          whitespace_ = whitespace;
        }
      }
    } else {
      const bool speculative = tokenizer_.LastTokenWasSpeculativeOperator();
      const JsWhitespace whitespace = whitespace_;
      whitespace_ = kNoWhitespace;
      if (whitespace != kNoWhitespace &&
          (WhitespaceNeededBefore(type, token) ||
           // A linebreak after a restricted-production keyword always
           // induces semicolon insertion regardless of the next token.
           // This matters chiefly when the linebreak was inside a block
           // comment (e.g. "return/*\n*/(x)" must keep its linebreak): a
           // real linebreak after such a keyword arrives as kSemiInsert
           // and is emitted directly, except before `;` or `}`, where
           // keeping the linebreak is merely harmless.  A linebreak after a
           // retained conditional-compilation comment likewise inserts a
           // semicolon when the token before the comment was such a keyword
           // (the comment is grammatically inert); prev_carries_asi_ carries
           // that signal across the comment.  A linebreak after a
           // speculatively-classified operator word (await/yield/for-of
           // "of") is likewise never removed: if the word is really a plain
           // identifier, the linebreak may be load-bearing for semicolon
           // insertion, and preserving it is always semantics-neutral.
           (whitespace == kLinebreak &&
            (IsAsiKeyword(prev_type_) || prev_carries_asi_)))) {
        next_type_ = type;
        next_token_ = token;
        next_carries_asi_ = speculative;
        next_position_ = token_position;
        *position_out = first_position;  // Beginning of whitespace/comments.
        if (whitespace == kLinebreak) {
          *token_out = "\n";
          return JsKeywords::kLineSeparator;
        } else {
          *token_out = " ";
          return JsKeywords::kWhitespace;
        }
      }
      prev_type_ = type;
      prev_token_ = token;
      prev_carries_asi_ = speculative;
      *token_out = token;
      *position_out = token_position;
      return type;
    }
  }
}

bool JsMinifyingTokenizer::WhitespaceNeededBefore(JsKeywords::Type type,
                                                  StringPiece token) {
  // Whitespace is needed 1) to separate words and numbers, 2) to prevent from
  // glomming a period onto the end of numeric literal that will absorb it as a
  // decimal point, and 3) to prevent us from joining operators together to
  // form line comments or other operators.
  if (IsNameNumberOrKeyword(type)) {
    return (IsNameNumberOrKeyword(prev_type_) ||
            prev_type_ == JsKeywords::kRegex);
  } else if (token == ".") {
    // To avoid merging tokens, we can't append a period to the end of a number
    // literal that...
    return (prev_type_ == JsKeywords::kNumber &&
            // ...doesn't already have a decimal point or exponent, and...
            prev_token_.find_first_of(".eE") == StringPiece::npos &&
            // ...either doesn't start with a zero digit, or...
            (!strings::StartsWith(prev_token_, "0") ||
             // ...is bare "0" (decimal, can absorb a decimal point), or...
             prev_token_ == "0" ||
             // ...does start with a zero digit, but is neither hex nor octal.
             (prev_token_.find_first_of("xX") == StringPiece::npos &&
              prev_token_.find_first_of("89") != StringPiece::npos)));
  } else if (strings::EndsWith(prev_token_, "/")) {
    // "/"+"/" would form a line comment; "/"+"*" a block comment (a hazard
    // whenever a downstream lexer reads the slash as division, e.g. a regex
    // scanned after an "await" that is really a plain identifier).
    return strings::StartsWith(token, "/") || strings::StartsWith(token, "*");
  } else if (strings::EndsWith(prev_token_, "+")) {
    return strings::StartsWith(token, "+");
  } else if (strings::EndsWith(prev_token_, "<")) {
    return strings::StartsWith(token, "!");
  } else if (strings::EndsWith(prev_token_, "!") ||
             strings::EndsWith(prev_token_, "-")) {
    return strings::StartsWith(token, "-");
  }
  return false;
}

bool MinifyUtf8Js(const JsTokenizerPatterns* patterns, StringPiece input,
                  GoogleString* output) {
  return MinifyUtf8JsWithSourceMap(patterns, input, output, nullptr);
}

bool MinifyUtf8JsWithSourceMap(
    const JsTokenizerPatterns* patterns, StringPiece input,
    GoogleString* output, net_instaweb::source_map::MappingVector* mappings) {
  JsMinifyingTokenizer tokenizer(patterns, input, mappings);
  while (true) {
    StringPiece token;
    switch (tokenizer.NextToken(&token)) {
      case JsKeywords::kEndOfInput:
        DCHECK(token.empty());
        DCHECK(!tokenizer.has_error());
        return true;
      case JsKeywords::kError:
        DCHECK(tokenizer.has_error());
        output->append(token.data(), token.size());
        return false;
      default:
        output->append(token.data(), token.size());
        break;
    }
  }
}

}  // namespace js

}  // namespace pagespeed
