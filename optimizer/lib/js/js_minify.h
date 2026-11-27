// SPDX-License-Identifier: Apache-2.0
// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-js-kernel.sh.
// Canonical source: mod_pagespeed 1.15, pagespeed/kernel/js/js_minify.h.
// Synced from the commit pinned in lib/js/JS_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-js-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/js/LICENSE.apache-2.0 for the license text.

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

#ifndef PAGESPEED_KERNEL_JS_JS_MINIFY_H_
#define PAGESPEED_KERNEL_JS_JS_MINIFY_H_

#include <cstdint>

#include "lib/js/compat/basictypes.h"
#include "lib/js/source_map.h"
#include "lib/js/compat/string.h"
#include "lib/js/compat/string_util.h"
#include "lib/js/js_keywords.h"
#include "lib/js/js_tokenizer.h"

namespace pagespeed {

namespace js {

// Represents the kind of whitespace between two tokens:
//   kNoWhitespace means that there is no whitespace between the tokens.
//   kSpace means there's been at least one space/tab, but no linebreaks.
//   kLinebreak means there's been at least one linebreak.
enum JsWhitespace : std::uint8_t { kNoWhitespace, kSpace, kLinebreak };

// This works like JsTokenizer, except that it only emits whitespace and
// comment tokens that are deemed necessary for the script to work.  IE
// conditional compilation comments are kept; other comments are removed.
// Whitespace tokens are only emitted if they are necessary to separate other
// tokens or for semicolon insertion, and any that are emitted will be
// collapsed to a single whitespace character.
class JsMinifyingTokenizer {
 public:
  // Creates a tokenizer that will tokenize the given input string (which must
  // outlive the JsMinifyingTokenizer object).
  JsMinifyingTokenizer(const JsTokenizerPatterns* patterns, StringPiece input);

  // Version that sets source mappings as well.
  // Note: Source Maps are only correct for ASCII text. Line and column numbers
  // will be incorrect if there are multi-byte chars in input.
  // TODO(sligocki): Fix this.
  JsMinifyingTokenizer(const JsTokenizerPatterns* patterns, StringPiece input,
                       net_instaweb::source_map::MappingVector* mappings);

  ~JsMinifyingTokenizer();

  // Gets the next token type from the input,
  JsKeywords::Type NextToken(StringPiece* token_out);

  // True if an error has been encountered.  All future calls to NextToken()
  // will return JsKeywords::kEndOfInput with an empty token string.
  bool has_error() const { return tokenizer_.has_error(); }

 private:
  JsKeywords::Type NextTokenHelper(
      StringPiece* token_out,
      net_instaweb::source_map::Mapping* token_out_position);

  // Determines whether we need to include whitespace to separate the given
  // token from the previous token.
  bool WhitespaceNeededBefore(JsKeywords::Type type, StringPiece token);

  JsTokenizer tokenizer_;
  JsWhitespace whitespace_;  // Whitespace since the previous token.
  JsKeywords::Type prev_type_;
  StringPiece prev_token_;
  JsKeywords::Type next_type_;
  StringPiece next_token_;
  // A retained conditional-compilation comment is grammatically inert, so the
  // semicolon-insertion signal of the token before it (a restricted-production
  // keyword like `return`) must be carried across the comment; these track
  // that carry for prev_/next_ respectively.  They carry the same signal for
  // a speculatively-classified operator word (await/yield/for-of "of"): a
  // linebreak after such a token is never removed (see
  // JsTokenizer::LastTokenWasSpeculativeOperator).
  bool prev_carries_asi_;
  bool next_carries_asi_;
  net_instaweb::source_map::MappingVector* mappings_;
  net_instaweb::source_map::Mapping current_position_;
  net_instaweb::source_map::Mapping next_position_;

  JsMinifyingTokenizer(const JsMinifyingTokenizer&) = delete;
  JsMinifyingTokenizer& operator=(const JsMinifyingTokenizer&) = delete;
};

// Minifies the given UTF8-encoded JavaScript code; returns true if the code
// parsed successfully, or false if a syntax error prevented complete
// minification.  Even if this function returns false, the output string will
// still be fully populated from the input; the portion of the input up to the
// parse error will be minified, and the remainder will be passed through
// unmodified.
//
// The input should be UTF8-encoded (or plain ASCII); the minifier does have
// some limited capability to tolerate invalid UTF8 bytes, so Latin1-encoded
// input will often work, but no guarantees are made.
bool MinifyUtf8Js(const JsTokenizerPatterns* patterns, StringPiece input,
                  GoogleString* output);

// Minify JS and returns a source mapping.  The input should be UTF8-encoded
// (or plain ASCII); the minifier does have some limited capability to tolerate
// invalid UTF8 bytes, so Latin1-encoded input will often work, but no
// guarantees are made.
bool MinifyUtf8JsWithSourceMap(
    const JsTokenizerPatterns* patterns, StringPiece input,
    GoogleString* output, net_instaweb::source_map::MappingVector* mappings);

}  // namespace js

}  // namespace pagespeed

#endif  // PAGESPEED_KERNEL_JS_JS_MINIFY_H_
