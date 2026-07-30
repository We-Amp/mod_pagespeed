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

// Tokenizing JavaScript is tricky.  Most programming languages can be lexed
// and parsed separately; for example, in Java, given the code fragment "(x +
// y) / z", you can divide it up into tokens "(", "x", "+", and so on without
// keeping track of previous tokens, whether the parens match up, etc., and
// once tokenized you can parse based on that token stream without remembering
// any of the whitespace or comments that appeared between the tokens.  In
// JavaScript, neither of these things are true.  In the above Java example,
// that slash is a division operator, but in JavaScript it *could* instead be
// the start of a regex literal if the token before the "(" was e.g. "if";
// therefore you have to keep track of the parse state.  Moreover, whitespace
// can sometimes matter in JavaScript due to semicolon insertion, and
// determining whether a given piece of whitespace matters or not requires not
// only *previous* parse state, but also the ability to look *ahead* to the
// next token (something that even other whitespace-significant languages, like
// Python or Haskell, don't require).  The goal of this class is to correctly
// tokenize JavaScript code with as little code as possible, by not being a
// full parser but still keeping track of some minimal parse state.
//
// We keep a stack of ParseState values, and in general most tokens will push a
// new state onto the stack, possibly after popping off other states.
// Examining the stack helps us to disambiguate the meanings of certain
// characters (like slashes).  So how many different ParseState values do we
// need?  The big three questions we have to be able to answer are: (1) Is this
// slash division or a regex?  (2) Are these braces a code block or an object
// literal?  (This matters primarily because a slash after a code block is a
// regex, and a slash after an object literal is division.)  (3) Does this
// linebreak induce semicolon insertion or not?  The different ParseState
// values we have exist to answer these questions.
//
// - kStartOfInput exists as a convenience.  It is only ever used at the bottom
//   of the stack, and the bottom of the stack is always kStartOfInput.  It's
//   just there so that we can always assume the stack is nonempty and thus we
//   can always read its top value.
//
// - kExpression is for expressions.  A slash after this is division.  An open
//   brace after this is an error.  A linebreak after this may or may not
//   insert a semicolon, depending on the next token.
//
// - kOperator is for prefix and binary operators, including keywords like
//   "in".  A slash after this is a regex, and braces after this are an object
//   literal.  (Note that postfix operators don't need a parse state, because a
//   postfix operator must follow an expression, and an expression followed by
//   a postfix operator is still just an expression.)
//
// - kPeriod is for the "." operator (this parse state is *not* used for
//   decimal points in numeric literals).  It is similar to other operators,
//   but a reserved word just after a period is an identifier.  For example,
//   even though "if" is normally a reserved word, "foo.if" is legal code, and
//   is equivalent to "foo['if']".
//
// - kQuestionMark is for the "?" character.  It behaves just like other
//   operators, but we must track it separately in order to determine whether a
//   given ":" character is for a label or a ternary operator.  This matters
//   because "foo:{}" is a label and code block, while "a?foo:{}" is a ternary
//   operator and object literal.
//
// - kOpenBrace, kOpenBracket, and kOpenParen are for opening delimiters.  When
//   we encounter a closing delimiter, we pop back to the matching open
//   delimiter and then modify the stack from there depending on what was just
//   created (e.g. an expression, or a block header, or something else).
//
// - kBlockKeyword is for keywords like "if" and "for" that are followed by
//   parentheses.  We track these so we know whether a pair of parens forms an
//   expression like "(a+b)" (after which a slash is division) or a block
//   header like "if(a>b)" (after which a slash is a regex).
//
// - kBlockHeader is a completed block header, like "if(a>b)".  Certain other
//   keywords like "do" and "else" are block headers on their own.
//
// - Lastly, we're left with eight keywords that don't fit into any of the
//   above categories.  We group these into three parse states:
//
//     - kReturnThrow for "return", "throw", and "yield".  They're sort of
//       like prefix operators in that a slash after these is a regex, but a
//       linebreak after these *always* inserts a semicolon.  (`yield` is a
//       keyword only inside generators, but the always-insert rule is
//       byte-safe for its sloppy-mode identifier uses too: a kept newline
//       re-parses identically.)
//
//     - kJumpKeyword for "break", "continue", and "debugger".  A slash after
//       these is an error, and a linebreak after these *always* inserts a
//       semicolon.
//
//     - kOtherKeyword for "default" (and for the initializer marker that
//       an `=` installs over a declaration keyword).  A slash after it is
//       an error too, but a linebreak after it *never* inserts a semicolon.
//
//     - kModuleDecl for "import" and "export" at statement position.  The
//       marker anchors a module declaration: while it is on the stack, a
//       linebreak at the declaration's grammatical end inserts a semicolon
//       no matter what token follows, while the declaration continuations
//       "from", ",", "=" and an open initializer expression still suppress
//       insertion.  A "(" or "." directly after "import" converts the
//       marker back to a plain kOperator (dynamic import() and import.meta
//       are expressions).
//
//     - kFromClause for the from-clause of an import/export declaration.
//       The contextual keyword "from" pushes it (awaiting the module
//       specifier), a module specifier string directly over kModuleDecl
//       (a bare import) pushes it too, and a completed
//       "export [async] function" body installs it.  A kExpression
//       directly over it is always the completed specifier or declaration:
//       nothing can continue there, so a linebreak always inserts.
//
//     - kModuleVarKeyword for the let/const/var keyword of a variable
//       declaration (a plain one, or the declaration of an `export`).  The
//       declared binding lands directly on it; after the bare binding only
//       "," or "=" can continue.
//
//     - kArrow for the `=>` of an arrow function (still emitted as
//       separate `=` and `>` tokens).  It sits under the arrow body like
//       an operator: an expression body keeps the ordinary continuation
//       rules, while a `{...}` block body closes into a terminal
//       expression -- per ECMA-262 an ArrowFunction is an
//       AssignmentExpression that no operator, call, or index access can
//       continue, so a linebreak after a block-bodied arrow always
//       inserts a semicolon.
//
//     - kObjectValue for the `:` of an object-literal property.  It acts
//       like an operator, but the expression collapse does not eat it, so
//       a property value never lands directly on the literal's brace --
//       leaving only a property NAME there, which lets ConsumeOpenParen
//       recognize method shorthand (`{ m() {} }`) without mistaking a
//       call in value position (`{ a: f() }`) for one.
//
//     - kClassKeyword for `class` and its heritage span (the name is
//       ignored; `extends` pushes an operator and the heritage expression
//       collapses back).  The `{` of the body completes the header into a
//       block header and opens a kClassBrace, the class body: element
//       names sit directly on it (so the method-shorthand gate fires for
//       `m(){}`, `get`/`set`, `static`, computed, and `#` names), a `{`
//       after a name is a static block, a field initializer is an
//       ordinary expression with ordinary ASI rules, `;` rolls back to
//       it, and the closing `}` pops the header -- rolling a declaration
//       back to statement base or collapsing a class expression into an
//       expression.
//
// To help make the above more concrete, suppose we're parsing the code:
//
//   if ([]) {
//     foo: while(true) break;
//   } else /x/.test('y');
//
// The progression of the parse stack would look like this:
//
//   if     -> BkKwd               "if" is a block keyword, so it needs (...).
//   (      -> BkKwd (
//   [      -> BkKwd ( [
//   ]      -> BkKwd ( Expr        [] is an expression (array literal).
//   )      -> BkHdr               Now "if (...)" is a complete block header.
//   {      -> BkHdr {
//   foo    -> BkHdr { Expr        An identifier is usually an expression...
//   :      -> BkHdr {             ...nevermind, a label.  Roll back statement.
//   while  -> BkHdr { BkKwd       "while" is a block keyword, just like "if".
//   (true) -> BkHdr { BkHdr       Three more tokens gives us the block header.
//   break  -> BkHdr { BkHdr Jump  "break" is special, slashes can't follow it.
//   ;      -> BkHdr {             Semicolon, roll back to start-of-statement.
//   }      ->                     Block finished.
//   else   -> BkHdr               "else" is a block header by itself.
//   /x/    -> BkHdr Expr          A slash after BkHdr is a regex.
//   .      -> BkHdr Expr Oper     A period is essentially a binary operator.
//   test   -> BkHdr Expr          "Expr Oper Expr" collapses to "Expr"
//   (      -> BkHdr Expr (
//   'y'    -> BkHdr Expr ( Expr
//   )      -> BkHdr Expr          Method call collapses into a single Expr.
//   ;      ->                     Semicolon, roll back to start-of-statement.
//
// In general, this class is focused on tokenizing, not actual parsing or
// detecting syntax errors, so there are many kinds of syntax errors that we
// don't detect and will simply ignore (such as "break 42;", which can be
// reasonably split into tokens even if it doesn't actually parse).  But we
// *must* abort whenever the parse state becomes too mangled for us to make
// meaningful decisions about what slashes mean.  For example, in the code
// "[a}/x/i", are those slashes a regex literal or division?  The question has
// no answer.  They'd be division if the code were "[a]/x/i", and a regex if
// the code were "{a}/x/i", but faced with "[a}", we have little choice but to
// abort.
//
// More information about semicolon insertion can be found here:
//   http://inimino.org/~inimino/blog/javascript_semicolons

#include "pagespeed/kernel/js/js_tokenizer.h"

#include <cstddef>
#include <vector>

#include "base/logging.h"
//#include "strings/stringpiece_utils.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/js/js_keywords.h"
#include "pagespeed/kernel/util/re2.h"

namespace pagespeed {

namespace js {

namespace {

// Maximum parse stack depth, to prevent memory exhaustion from crafted
// deeply nested input (e.g. "[[[[[...").  When the cap is reached the
// tokenizer reports an error (preserving the input byte-for-byte, like any
// other tokenizer error) rather than growing the stack without bound.  The
// guard sits at every push site that crafted input can repeat without an
// intervening pop: the open delimiters, `?`, and `${`.
const size_t kMaxParseStackDepth = 4096;

// Returns true if the given comment token contains a line terminator
// (\n, \r, U+2028, or U+2029).  Such a comment counts as a line terminator
// for automatic semicolon insertion (ECMA-262).
bool CommentHasLineTerminator(StringPiece token) {
  return (token.find('\n') != StringPiece::npos ||
          token.find('\r') != StringPiece::npos ||
          token.find("\xE2\x80\xA8") != StringPiece::npos ||
          token.find("\xE2\x80\xA9") != StringPiece::npos);
}

// Returns true if the given comment token is shaped like an IE conditional
// compilation comment (/*@...@*/), which the minifier retains verbatim.
bool IsConditionalCompilationComment(StringPiece token) {
  return (token.size() >= 6 && strings::StartsWith(token, "/*@") &&
          strings::EndsWith(token, "@*/"));
}

// Regex to match JavaScript identifiers.  For details, see page 18 of
// http://www.ecma-international.org/publications/files/ECMA-ST/Ecma-262.pdf
const char* const kIdentifierRegex =
    // An identifier must begin with a $, _, unicode letter (more specifically,
    // a character in the Lu, Ll, Lt, Lm, Lo, or Nl category), or unicode
    // escape.
    "([$_\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}]|\\\\u[0-9A-Fa-f]{4})"
    // After that, an identifier may have zero or more characters that are one
    // of the above, a combining mark (Mn or Mc), a digit (Nd), a connector
    // punctuation (Pc) or one of the characters ZERO WIDTH NON-JOINER (U+200C)
    // or ZERO WIDTH JOINER (U+200D).
    "([$_\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}\\p{Mn}\\p{Mc}\\p{Nd}"
    "\\p{Pc}\xE2\x80\x8C\xE2\x80\x8D]|\\\\u[0-9A-Fa-f]{4})*";

// Regex to match JavaScript line comments.  This regex contains exactly one
// capturing group, which will match the linebreak (or end-of-input) that
// terminated the line comment.
const char* const kLineCommentRegex =
    "(?://|<!--|-->)\\C*?([\r\n\\p{Zl}\\p{Zp}]|\\z)";

// Regex to match JavaScript numeric literals.  This must be compiled in POSIX
// mode, so that the |'s are leftmost-longest rather than leftmost-first.
const char* const kNumericLiteralPosixRegex =
    // A number can be a hexadecimal literal, or...
    "0[xX][0-9a-fA-F]+|"
    // ...it can be a octal literal, or...
    "0[0-7]+|"
    // ...it can be a decimal literal.  To qualify as a decimal literal, it
    // must 1) start with a nonzero digit, or 2) start with zero but contain
    // a non-octal digit (8 or 9) in there somewhere, or 3) be a single zero
    // digit.
    "(([1-9][0-9]*|0([0-9]*[89][0-9]*)?)"
    // A decimal literal may optionally be followed by a decimal point and
    // fractional part:
    "(\\.[0-9]*)?"
    // Alternatively, instead of all that, a decimal literal may instead
    // start with a decimal point (instead of starting with a digit).
    "|\\.[0-9]+)"
    // Finally, any of the above kinds of decimal literal may optionally be
    // followed by an exponent.
    "([eE][+-]?[0-9]+)?";

// Regex to match most JavaScript operators (some operators, such as comma,
// period, question mark, and colon are special-cased elsewhere).
const char* const kOperatorRegex =
    // && || ++ -- ~
    "&&|\\|\\||\\+\\+|--|~|"
    // * *= / /= % %= ^ ^= & &= | |= + += - -=
    "[*/%^&|+-]=?|"
    // ! != !== = == ===
    "[!=]={0,2}|"
    // < <= << <<=
    "<{1,2}=?|"
    // > >= >> >>= >>> >>>=
    ">{1,3}=?";

// Regex to match JavaScript regex literals.  For details, see page 25 of
// http://www.ecma-international.org/publications/files/ECMA-ST/Ecma-262.pdf
const char* const kRegexLiteralRegex =
    // Regex literals can contain characters that aren't slashes, backslashes,
    // open brackets, or linebreaks.
    "/([^/\\\\\\[\r\n\\p{Zl}\\p{Zp}]|"
    // They can also contain character classes, which are enclosed in square
    // brackets.  Within the brackets, close brackets and backslashes must be
    // escaped.  Linebreaks are *never* permitted -- not even if escaped.
    "\\[([^\\]\\\\\r\n\\p{Zl}\\p{Zp}]|"
    "\\\\[^\r\n\\p{Zl}\\p{Zp}])*\\]|"
    // Finally, they can contain escape sequences.  Again, linebreaks are
    // forbidden and cannot be escaped.
    "\\\\[^\r\n\\p{Zl}\\p{Zp}])+/"
    // Regex literals may optionally be followed by zero or more flags, which
    // can consist of any characters allowed within identifiers (even \uXXXX
    // escapes!); see kIdentifierRegex for details.  (Very few of these
    // characters are actually semantically valid regex flags, but they're all
    // lexically valid.)
    "([$_\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}\\p{Mn}\\p{Mc}\\p{Nd}"
    "\\p{Pc}\xE2\x80\x8C\xE2\x80\x8D]|\\\\u[0-9A-Fa-f]{4})*";

// Regex to match JavaScript string literals.  For details, see page 22 of
// http://www.ecma-international.org/publications/files/ECMA-ST/Ecma-262.pdf
// This regex will still match when given a string literal containing an
// unescaped linebreak, but the match will terminate after the linebreak; the
// caller must then check whether the start and end characters of the match are
// the same (both single quote or both double quote), and reject it if not.
// Note that since ES2019 (the JSON-superset change), raw U+2028 and U+2029
// are legal inside string literals, so the terminator classes below cover
// only the quote characters and the CR/LF linebreaks -- U+2028/U+2029 are
// matched by the \C body like any other character and never trigger
// linebreak logic inside a string.
const char* const kStringLiteralRegex =
    // Single-quoted string literals can contain any characters that aren't
    // single quotes, backslashes, or linebreaks.  They can also contain escape
    // sequences, which is a backslash followed either by a linebreak or by any
    // one character.  But note that the sequence \r\n counts as *one*
    // linebreak for this purpose, as does \n\r.  Finally, we use RE2's \C
    // escape for matching arbitrary bytes, along with very careful use of
    // greedy and non-greedy operators, to allow the string literal to contain
    // invalid UTF-8 characters, in case we're given e.g. Latin1-encoded input.
    // This is subtle and fragile, but fortunately we have unit tests that will
    // break if we ever get this wrong.
    //
    // This would be easier if there were a way to say "match an invalid UTF8
    // byte only", but apparently there is no way to do this in RE2.
    // See https://groups.google.com/forum/#!topic/re2-dev/26wVIHcowh4
    "'(\\C*?(\\\\(\r\n|\n\r|\n|.))?)*?['\n\r]|"
    // A string literal can also be double-quoted instead, which is the same,
    // except that double quotes must be escaped instead of single quotes.
    "\"(\\C*?(\\\\(\r\n|\n\r|\n|.))?)*?[\"\n\r]";

// Regex to match JavaScript whitespace.  For details, see page 15 of
// http://www.ecma-international.org/publications/files/ECMA-ST/Ecma-262.pdf
// This regex contains exactly one capturing group; iff it captures anything,
// then the whitespace contains at least one linebreak.
const char* const kWhitespaceRegex =
    // Line separators include \n, \r, and characters in the "Line Separator"
    // (Zl) and "Paragraph Separator" (Zp) Unicode categories.
    "(?:([\n\r\\p{Zl}\\p{Zp}])|"
    // Horizontal whitespace includes space, \f, \t, \v, BYTE ORDER MARK
    // (U+FEFF), and characters in the "Space Separator" (Zs) Unicode category.
    "[ \f\t\v\xEF\xBB\xBF\\p{Zs}])+";

// Regex to check if the next token in the remaining input could continue the
// current statement, assuming the current statement currently ends with an
// expression.  (Note that this regex will not necessarily capture the entire
// next token; the only useful information to be had from it is whether it
// matches at all or not).
const char* const kLineContinuationRegex =
    // Any operator (even a multicharacter operator) starting with one of the
    // following characters can continue the current expression.
    "[(*/%^&|<>?:,.]|"
    // An = can continue immediately after an expression, but an => cannot:
    // ECMA-262 forbids a LineTerminator between an arrow head and its =>
    // token, so a linebreak before => always inserts a semicolon.  (Without
    // this, the already-invalid "a = x\n=> y" would minify to the VALID
    // "a=x=>y" -- an invalid-to-valid transformation.)
    "=($|[^>])|"
    // A != can continue immediately after an expression, but not a !.
    "!=|"
    // A + or - can continue after an expression, but not a ++ or -- (because
    // JavaScript's grammar specifically forbids linebreaks between the two
    // tokens in "i++" or in "i--").
    "\\+($|[^+])|-($|[^-])|"
    // Finally, the in or instanceof operators can continue, though we have to
    // be sure we're not just looking at an identifier that starts with "in",
    // so make sure the "in" or "instanceof" is not followed by an identifier
    // character (see kIdentifierRegex for details).
    "(in|instanceof)($|[^$_\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}\\p{Mn}"
    "\\p{Mc}\\p{Nd}\\p{Pc}\xE2\x80\x8C\xE2\x80\x8D\\\\])";

// Regex to check if the next token in the remaining input could continue an
// import or export declaration when the parse stack is at a module
// declaration point (kModuleDecl directly below a kExpression): after a
// default-import binding, an import/export clause, or a namespace binding.
// Only `from` (a from-clause or re-export) and `,` (binding and clause
// lists) can continue the declaration there; anything else ends it, so the
// linebreak inserts a semicolon.  (Note that this regex will not necessarily
// capture the entire next token; the only useful information to be had from
// it is whether it matches at all or not).
const char* const kModuleContinuationRegex =
    ",|"
    // `from` must not merely be the prefix of a longer identifier, so make
    // sure it is not followed by an identifier character (see
    // kIdentifierRegex for details).
    "from($|[^$_\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}\\p{Mn}"
    "\\p{Mc}\\p{Nd}\\p{Pc}\xE2\x80\x8C\xE2\x80\x8D\\\\])";

// Regex to check if the next token in the remaining input could continue an
// export variable declaration at its bare-binding point (kModuleVarKeyword
// directly below a kExpression): only `,` (the next declarator) and `=` (the
// initializer) can continue there; anything else ends the declaration, so
// the linebreak inserts a semicolon.
const char* const kModuleVarContinuationRegex = "[,=]";

}  // namespace

JsTokenizer::JsTokenizer(const JsTokenizerPatterns* patterns, StringPiece input)
    : patterns_(patterns),
      input_(input),
      json_step_(kJsonStart),
      start_of_line_(true),
      error_(false),
      arrow_body_asi_pending_(false),
      postfix_update_pending_(false) {
  parse_stack_.push_back(kStartOfInput);
}

JsTokenizer::~JsTokenizer() {}

JsKeywords::Type JsTokenizer::NextToken(StringPiece* token_out) {
  // Empty out the lookahead queue before we scan any further.
  if (!lookahead_queue_.empty()) {
    const JsKeywords::Type type = lookahead_queue_.front().first;
    *token_out = lookahead_queue_.front().second;
    lookahead_queue_.pop_front();
    return type;
  }
  // If we've already encountered an error, just keep returning an error token.
  if (error_) {
    return Error(token_out);
  }
  // If we've cleanly reached the end of the input, we're done.
  if (input_.empty()) {
    parse_stack_.clear();
    *token_out = StringPiece();
    return JsKeywords::kEndOfInput;
  }
  // Invariant: until we reach the end of the input, the parse stack is never
  // empty, and the bottom entry is always kStartOfInput.  This is for
  // convenience, so that elsewhere we don't have to keep testing whether the
  // parse stack is empty before looking at the top entry.
  DCHECK(!parse_stack_.empty());
  DCHECK_EQ(kStartOfInput, parse_stack_[0]);
  // Backstop against unbounded parse-stack growth.  The per-consumer guards
  // below cap the delimiter-nesting paths, but token kinds that push without
  // a matching pop (repeated restricted-production or block keywords,
  // `a.b.c...` member chains, etc.) would otherwise grow the stack one entry
  // per token.  Bounding here -- before dispatch -- closes the whole class:
  // a single NextToken() call pushes a small constant number of entries, so
  // the stack can never exceed kMaxParseStackDepth by more than that.
  // Erroring is byte-preserving (Error() passes the remainder through).
  if (parse_stack_.size() >= kMaxParseStackDepth) {
    return Error(token_out);
  }
  // A hashbang (`#!...`) line is valid only as the very first bytes of a
  // script or module.  Consume the whole line -- including its terminating
  // linebreak -- as a comment, so the minifier can retain it verbatim
  // (the linebreak inside the token is what keeps the following code off
  // the hashbang line).  After leading trivia this is not a legal
  // hashbang, but accepting it stays byte-preserving; a `#` anywhere else
  // is an error.
  if (json_step_ == kJsonStart && input_.size() >= 2 && input_[0] == '#' &&
      input_[1] == '!') {
    int size = 0;
    const int input_size = input_.size();
    while (size < input_size) {
      const unsigned char c = input_[size];
      if (c == '\n' || c == '\r') {
        ++size;
        break;
      }
      // U+2028/U+2029 (E2 80 A8/A9) are line terminators too.
      if (c == 0xE2 && size + 2 < input_size &&
          static_cast<unsigned char>(input_[size + 1]) == 0x80 &&
          (static_cast<unsigned char>(input_[size + 2]) == 0xA8 ||
           static_cast<unsigned char>(input_[size + 2]) == 0xA9)) {
        size += 3;
        break;
      }
      ++size;
    }
    // \r\n is a single LineTerminatorSequence.
    if (size < input_size && input_[size - 1] == '\r' && input_[size] == '\n') {
      ++size;
    }
    return Emit(JsKeywords::kComment, size, token_out);
  }
  // Scan and return the next token.
  const char ch = input_[0];
  switch (ch) {
    case ' ':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
    case '\v':
      // This covers ASCII whitespace (which is the common case).  Unicode
      // whitespace is detected in the default case below.
      {
        JsKeywords::Type type;
        if (!TryConsumeWhitespace(true, &type, token_out)) {
          LOG(DFATAL) << "TryConsumeWhitespace failed on ASCII whitespace: "
                      << static_cast<int>(ch);
          return Error(token_out);
        }
        return type;
      }
    case '{':
      return ConsumeOpenBrace(token_out);
    case '}':
      return ConsumeCloseBrace(token_out);
    case '[':
      return ConsumeOpenBracket(token_out);
    case ']':
      return ConsumeCloseBracket(token_out);
    case '(':
      return ConsumeOpenParen(token_out);
    case ')':
      return ConsumeCloseParen(token_out);
    case ':':
      return ConsumeColon(token_out);
    case ',':
      return ConsumeComma(token_out);
    case '#': {
      // A private name (`#x`), valid inside class bodies (and tolerated
      // byte-preservingly elsewhere).  Anything else (`#` alone, or not
      // followed by an identifier-start) is an error.
      JsKeywords::Type type;
      if (TryConsumePrivateName(&type, token_out)) {
        return type;
      }
      return Error(token_out);
    }
    case '.':
      return ConsumePeriod(token_out);
    case '?':
      return ConsumeQuestionMark(token_out);
    case ';':
      return ConsumeSemicolon(token_out);
    case '/':
      return ConsumeSlash(token_out);
    case '\'':
    case '"':
      return ConsumeString(token_out);
    case '`':
      // Backtick unambiguously begins an ES6 template literal, regardless
      // of parse state (a tagged template like tag`x` is still a template).
      return ConsumeTemplateChunk(token_out);
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      // Numeric literals (whether decimal, hex, or octal) start either with a
      // digit or with a period.  This line covers the starts-with-digit case,
      // while ConsumePeriod above checks for the starts-with-period case.
      return ConsumeNumber(token_out);
    default: {
      JsKeywords::Type type;
      if (TryConsumeIdentifierOrKeyword(&type, token_out) ||
          TryConsumeComment(&type, token_out) ||
          TryConsumeWhitespace(true, &type, token_out)) {
        return type;
      }
      // If all else fails, maybe this is an operator.  If not,
      // ConsumeOperator will return an error token.
      return ConsumeOperator(token_out);
    }
  }
}

GoogleString JsTokenizer::ParseStackForTest() const {
  GoogleString output;
  for (std::vector<ParseState>::const_iterator iter = parse_stack_.begin();
       iter != parse_stack_.end(); ++iter) {
    if (!output.empty()) {
      output.push_back(' ');
    }
    switch (*iter) {
      case kStartOfInput:
        output.append("Start");
        break;
      case kExpression:
        output.append("Expr");
        break;
      case kOperator:
        output.append("Oper");
        break;
      case kPeriod:
        output.append(".");
        break;
      case kQuestionMark:
        output.append("?");
        break;
      case kOptionalChain:
        output.append("?.");
        break;
      case kOpenBrace:
        output.append("{");
        break;
      case kOpenBracket:
        output.append("[");
        break;
      case kOpenParen:
        output.append("(");
        break;
      case kTemplateInterp:
        output.append("${");
        break;
      case kBlockKeyword:
        output.append("BkKwd");
        break;
      case kBlockHeader:
        output.append("BkHdr");
        break;
      case kReturnThrow:
        output.append("RetTh");
        break;
      case kJumpKeyword:
        output.append("Jump");
        break;
      case kOtherKeyword:
        output.append("Other");
        break;
      case kModuleDecl:
        output.append("Mod");
        break;
      case kFromClause:
        output.append("From");
        break;
      case kModuleVarKeyword:
        output.append("MVar");
        break;
      case kArrow:
        output.append("=>");
        break;
      case kObjectValue:
        output.append("OVal");
        break;
      case kClassKeyword:
        output.append("Cls");
        break;
      case kClassBrace:
        output.append("Cls{");
        break;
      default:
        LOG(DFATAL) << "Unknown parse state: " << *iter;
        output.append("UNKNOWN");
        break;
    }
  }
  return output;
}

JsKeywords::Type JsTokenizer::ConsumeOpenBrace(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('{', input_[0]);
  // Invariant: a "{" after a speculatively-classified operator word
  // (await/yield/for-of "of" -- each of which may really be a plain
  // identifier) must not be committed to the object-literal reading when
  // the identifier reading is live: in that reading (an ASI-separated
  // statement) the braces are a BLOCK, after which a slash is a regex
  // rather than division.  "(" and "[" continue the expression under
  // either reading; "{" is the one delimiter whose reading diverges.
  // The identifier reading is live ONLY when a line terminator separates
  // the word from the "{" (speculative_linebreak_; a terminator inside a
  // block comment counts, per the spec's ASI rules): without one,
  // "expr {" is a SyntaxError, so the "{" is provably the operand's
  // object literal (`yield {a: 1}` in a generator) and falls through to
  // the normal handling.  With one, declining (error) is fail-safe:
  // minification fails and the caller serves the original input
  // unmodified.
  if (speculative_operator_ && speculative_linebreak_) {
    return Error(token_out);
  }
  if (parse_stack_.size() >= kMaxParseStackDepth) {
    return Error(token_out);
  }
  const ParseState state = parse_stack_.back();
  if (state == kBlockKeyword) {
    // ES2019 optional catch binding: `catch {` has no parenthesized
    // parameter, so the block keyword is followed directly by its block.
    // Complete the block header ourselves, exactly as if `(...)` had been
    // present.  (This also tokenizes invalid input like `if {` as a block;
    // syntax checking is a non-goal of this class, and the slash
    // classification only shifts for input that was already invalid JS.)
    parse_stack_.pop_back();
    PushBlockHeader();
    parse_stack_.push_back(kOpenBrace);
    return Emit(JsKeywords::kOperator, 1, token_out);
  }
  // The `{` of a class body completes the class header into a block header
  // and opens the class body brace: directly over the keyword for an
  // anonymous class (`class {}`, `export default class {}`), or over the
  // heritage expression (`class X extends Y {}`).
  if (state == kClassKeyword ||
      (state == kExpression && parse_stack_.size() >= 2 &&
       parse_stack_[parse_stack_.size() - 2] == kClassKeyword)) {
    if (state == kExpression) {
      parse_stack_.pop_back();
    }
    parse_stack_.pop_back();
    PushBlockHeader();
    parse_stack_.push_back(kClassBrace);
    return Emit(JsKeywords::kOperator, 1, token_out);
  }
  // A `{` directly after a name inside a class body is a static block
  // (`static { ... }`): complete it into a block of ordinary statements.
  // (A bare `x {` was already invalid JS, tolerated byte-preservingly.)
  if (state == kExpression && parse_stack_.size() >= 2 &&
      parse_stack_[parse_stack_.size() - 2] == kClassBrace) {
    parse_stack_.pop_back();
    PushBlockHeader();
    parse_stack_.push_back(kOpenBrace);
    return Emit(JsKeywords::kOperator, 1, token_out);
  }
  // Note that kOtherKeyword is intentionally permitted here (and in
  // ConsumeOpenBracket): `export default {a: 1}` starts an object
  // expression.  (Destructuring binding patterns arrive via
  // kModuleVarKeyword instead.)
  if (state == kExpression || state == kPeriod || state == kOptionalChain ||
      state == kJumpKeyword) {
    return Error(token_out);
  }
  parse_stack_.push_back(kOpenBrace);
  return Emit(JsKeywords::kOperator, 1, token_out);
}

bool JsTokenizer::PopToMatchingOpen(
    ParseState target, std::initializer_list<ParseState> error_states,
    StringPiece* token_out) {
  while (true) {
    DCHECK(!parse_stack_.empty());
    const ParseState state = parse_stack_.back();
    if (state == target) {
      parse_stack_.pop_back();
      return true;
    }
    for (ParseState es : error_states) {
      if (state == es) {
        Error(token_out);
        return false;
      }
    }
    parse_stack_.pop_back();
  }
}

JsKeywords::Type JsTokenizer::ConsumeCloseBrace(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('}', input_[0]);
  // If the nearest enclosing open delimiter is a template interpolation,
  // then this '}' does not close a brace: it resumes the enclosing template
  // literal (a TemplateMiddle or TemplateTail chunk).  Pop the
  // interpolation's expression states and the kTemplateInterp marker, then
  // scan the template chunk starting at this '}'.
  if (NearestOpenDelimiterIsTemplateInterp()) {
    while (parse_stack_.back() != kTemplateInterp) {
      parse_stack_.pop_back();
      DCHECK(!parse_stack_.empty());
    }
    parse_stack_.pop_back();  // Pop the kTemplateInterp marker.
    return ConsumeTemplateChunk(token_out);
  }
  // If the nearest enclosing open delimiter is a class body brace, this
  // '}' closes the class body: pop everything down to it (an element name,
  // a field initializer, or nothing) and the brace itself, then continue
  // exactly as if a block had just closed (the class header's block header
  // is popped below, rolling a declaration back to statement base or
  // collapsing a class expression into an expression).
  if (NearestOpenDelimiterIsClassBrace()) {
    while (parse_stack_.back() != kClassBrace) {
      parse_stack_.pop_back();
      DCHECK(!parse_stack_.empty());
    }
    parse_stack_.pop_back();  // Pop the class body brace.
  } else if (!PopToMatchingOpen(kOpenBrace,
                                {kStartOfInput, kOpenBracket, kOpenParen,
                                 kBlockKeyword, kTemplateInterp},
                                token_out)) {
    // Pop the most recent kOpenBrace (and everything above it) off the stack.
    return JsKeywords::kError;
  }
  // If the open brace was preceeded by a BlockHeader, we can pop that off the
  // stack at this point.  The presence of a BlockHeader means these braces
  // were a block (rather than an object literal), and usually after popping it
  // off we'll now be back at a start-of-statement (in which case we'll
  // correctly deduce below that this was a block).  The one exception is
  // anonymous function literals, which is the one case where the block header
  // will (necessarily) be preceeded by an operator, or open paran, or
  // something else indicating an expression (e.g. foo=function(){};).  In that
  // case, after popping the BlockHeader, we will correctly conclude below that
  // we have just created an Expression.
  //
  // (If there were no braces after the BlockHeader (e.g. "if (x) return;"),
  // then that BlockHeader will be popped when we roll back to
  // start-of-statement for some other reason, such as encountering a
  // semicolon.)
  const bool popped_block_header = (parse_stack_.back() == kBlockHeader);
  if (popped_block_header) {
    parse_stack_.pop_back();
  }
  // Depending on the parse state that came before the kOpenBrace, we just
  // closed either an object literal (which is a kExpression), or a block
  // (which isn't).  One refinement: a kOtherKeyword can precede an object
  // literal only as the start of an `export default {...}` object
  // expression (which involves no block header).  If we just popped a block
  // header while a kOtherKeyword lies beneath, these braces were a block,
  // not an expression, so do not push one.  This preserves the behavior for
  // EVERY block-header-over-kOtherKeyword shape: the genuine
  // `export default function(){}` (a declaration; a slash directly after it
  // then errors out, preserving the input -- see the kExport comment in
  // TryConsumeIdentifierOrKeyword) as well as invalid-JS shapes like
  // `default do{}` that reach here via the block-header routes.
  DCHECK(!parse_stack_.empty());
  // A `}` closing a brace that sits directly on a kArrow ends the arrow's
  // block body.  The completed ArrowFunction is grammatically terminal: per
  // ECMA-262 it is an AssignmentExpression, not a UnaryExpression, so no
  // operator, call, or index access can continue it, and a linebreak
  // before the next token always inserts a semicolon.  Collapse it like
  // any expression and arm the one-shot flag that
  // TryInsertLinebreakSemicolon consults at the next linebreak.  (A
  // function EXPRESSION body, `x => function(){}`, does NOT take this
  // path: its closing brace pops a block header, and the function
  // expression can still be called or divided.)
  if (!popped_block_header && parse_stack_.back() == kArrow) {
    parse_stack_.pop_back();
    PushExpression();
    const JsKeywords::Type type = Emit(JsKeywords::kOperator, 1, token_out);
    arrow_body_asi_pending_ = true;
    return type;
  }
  // The closing brace of a function-bodied export declaration: the
  // declaration is grammatically complete there -- nothing can continue
  // it (node-verified: a following slash starts a regex statement, with
  // or without a linebreak).  Peel the states the header forms interpose
  // -- an `async` identifier's kExpression and the `default` kOtherKeyword
  // -- and when a module marker lies beneath, move the declaration to the
  // from-clause shape, where ASI always fires.  This covers
  // `export function f(){}`, `export async function f(){}`,
  // `export default [async] function(){}`, and `export default class {}`
  // (the last two kept the kOtherKeyword carve-out until now).
  if (popped_block_header) {
    if (parse_stack_.back() == kExpression && parse_stack_.size() >= 2 &&
        (parse_stack_[parse_stack_.size() - 2] == kModuleDecl ||
         parse_stack_[parse_stack_.size() - 2] == kOtherKeyword)) {
      parse_stack_.pop_back();
    }
    if (parse_stack_.back() == kOtherKeyword && parse_stack_.size() >= 2 &&
        parse_stack_[parse_stack_.size() - 2] == kModuleDecl) {
      parse_stack_.pop_back();
    }
  }
  if (popped_block_header && parse_stack_.back() == kModuleDecl) {
    parse_stack_.push_back(kFromClause);
    PushExpression();
  } else if ((popped_block_header && parse_stack_.back() == kArrow) ||
             (CanPreceedObjectLiteral(parse_stack_.back()) &&
              !(popped_block_header && parse_stack_.back() == kOtherKeyword))) {
    // A function-EXPRESSION arrow body (`x => function(){}`) collapses into
    // the body expression (kArrow is deliberately not object-literal-shaped
    // for the property-name/method/colon checks, so this one case names it
    // directly).
    PushExpression();
  }
  // Emit a token for the close brace.
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeOpenBracket(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('[', input_[0]);
  const ParseState state = parse_stack_.back();
  // kOtherKeyword is permitted: `export default [1]` starts an array
  // expression (destructuring declarations arrive via kModuleVarKeyword).
  // kOptionalChain is permitted too: `a?.[i]`.
  if (state == kPeriod || state == kBlockKeyword || state == kJumpKeyword ||
      state == kClassKeyword) {
    return Error(token_out);
  }
  if (parse_stack_.size() >= kMaxParseStackDepth) {
    return Error(token_out);
  }
  parse_stack_.push_back(kOpenBracket);
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeCloseBracket(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ(']', input_[0]);
  // Pop the most recent kOpenBracket (and everything above it) off the stack.
  if (!PopToMatchingOpen(kOpenBracket,
                         {kStartOfInput, kOpenBrace, kOpenParen, kBlockKeyword,
                          kBlockHeader, kTemplateInterp, kClassBrace},
                         token_out)) {
    return JsKeywords::kError;
  }
  PushExpression();
  // Emit a token for the close bracket.
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeOpenParen(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('(', input_[0]);
  if (parse_stack_.size() >= kMaxParseStackDepth) {
    return Error(token_out);
  }
  // `import(...)` is a dynamic import call, not a declaration: drop the
  // module-declaration marker and treat the construct exactly as if `import`
  // had pushed a plain kOperator (the close paren then collapses to an
  // expression).  `export (...)` is invalid JS either way; converting it
  // here keeps the tokenization byte-preserving.
  if (parse_stack_.back() == kModuleDecl) {
    parse_stack_.pop_back();
    PushOperator();
  }
  const ParseState state = parse_stack_.back();
  // A paren after a variable-declaration keyword is never valid (`var (x)`).
  // A paren after the `default` kOtherKeyword is valid only as
  // `export default (...)`, where it begins a parenthesized expression or an
  // arrow parameter list (`export default () => {}`); the completed
  // expression then collapses onto the kOtherKeyword exactly like
  // `export default 5` does.  Everywhere else (e.g. a switch's
  // `default (x)`) it stays a byte-preserving error.
  const bool other_allows_paren =
      state == kOtherKeyword && parse_stack_.size() >= 2 &&
      parse_stack_[parse_stack_.size() - 2] == kModuleDecl;
  if (state == kPeriod || state == kJumpKeyword || state == kModuleVarKeyword ||
      state == kClassKeyword ||
      (state == kOtherKeyword && !other_allows_paren)) {
    return Error(token_out);
  }
  // Method shorthand in an object literal (`{ m() {} }`, `{ get v() {} }`,
  // `{ async *n() {} }`, `{ ['k']() {} }`) or a class body
  // (`class X { m() {} }`): a `(` directly after a property or element
  // name -- an expression sitting directly on an object-literal kOpenBrace
  // or a kClassBrace -- is the parameter list of a method, and the `{...}`
  // after it is a block body.  Push a block keyword so the parens complete
  // into a block header, exactly like a function's; the body then closes
  // back to the property position of the literal or the class body.  Only
  // a property name ever sits directly on the literal's brace: a value
  // expression is held off it by the kObjectValue that the property colon
  // installs (so `{ a: f() }` and `{ a: (function(){})() }` are calls, not
  // methods), a parenthesized property name is not legal, a shorthand
  // property cannot be followed by `(` inside its own literal, and a `{`
  // that is NOT object-literal-shaped (e.g. at statement position) fails
  // the CanPreceedObjectLiteral check below its brace.
  if (state == kExpression && parse_stack_.size() >= 2 &&
      ((parse_stack_[parse_stack_.size() - 2] == kOpenBrace &&
        parse_stack_.size() >= 3 &&
        CanPreceedObjectLiteral(parse_stack_[parse_stack_.size() - 3])) ||
       parse_stack_[parse_stack_.size() - 2] == kClassBrace)) {
    parse_stack_.push_back(kBlockKeyword);
  }
  parse_stack_.push_back(kOpenParen);
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeCloseParen(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ(')', input_[0]);
  // Pop the most recent kOpenParen (and everything above it) off the stack.
  if (!PopToMatchingOpen(
          kOpenParen,
          {kStartOfInput, kOpenBrace, kOpenBracket, kBlockKeyword, kBlockHeader,
           kTemplateInterp, kClassBrace},
          token_out)) {
    return JsKeywords::kError;
  }
  // If this is the closing paren of e.g. "if (...)", then we've just created a
  // kBlockHeader.  Otherwise, we've just created a kExpression.
  DCHECK(!parse_stack_.empty());
  if (parse_stack_.back() == kBlockKeyword) {
    parse_stack_.pop_back();
    PushBlockHeader();
  } else {
    PushExpression();
  }
  // Emit a token for the close parenthesis.
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeBlockComment(StringPiece* token_out) {
  DCHECK_GE(input_.size(), 2u);
  DCHECK_EQ('/', input_[0]);
  DCHECK_EQ('*', input_[1]);
  const stringpiece_ssize_type index = input_.find("*/", 2);
  if (index == StringPiece::npos) {
    return Error(token_out);
  }
  return Emit(JsKeywords::kComment, index + 2, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeLineComment(StringPiece* token_out) {
  Re2StringPiece unconsumed = StringPieceToRe2(input_);
  Re2StringPiece linebreak;
  if (!RE2::Consume(&unconsumed, patterns_->line_comment_pattern, &linebreak)) {
    // We only call ConsumeLineComment when we're sure we're looking at a line
    // comment, so this ought not happen even for pathalogical input.
    LOG(DFATAL) << "Failed to match line comment pattern: "
                << input_.substr(0, 50);
    return Error(token_out);
  }
  return Emit(JsKeywords::kComment,
              input_.size() - unconsumed.size() - linebreak.size(), token_out);
}

bool JsTokenizer::TryConsumeComment(JsKeywords::Type* type_out,
                                    StringPiece* token_out) {
  DCHECK(!input_.empty());
  if (strings::StartsWith(input_, "/*")) {
    *type_out = ConsumeBlockComment(token_out);
    return true;
  }
  if (strings::StartsWith(input_, "//") ||
      strings::StartsWith(input_, "<!--") ||
      (start_of_line_ && strings::StartsWith(input_, "-->"))) {
    *type_out = ConsumeLineComment(token_out);
    return true;
  }
  return false;
}

bool JsTokenizer::TryConsumePrivateName(JsKeywords::Type* type_out,
                                        StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('#', input_[0]);
  // Consume `#` plus an ASCII identifier as a single identifier token.
  // (Non-ASCII private names fall through to the byte-preserving error,
  // like non-ASCII binding lookaheads elsewhere in this class.)
  const int size = input_.size();
  if (size >= 2) {
    const unsigned char first = input_[1];
    if (('a' <= first && first <= 'z') || first == '_' ||
        ('A' <= first && first <= 'Z') || first == '$' || first == '\\') {
      int index = 2;
      for (; index < size; ++index) {
        const unsigned char ch = input_[index];
        if (!net_instaweb::IsAsciiAlphaNumeric(ch) && ch != '_' && ch != '$' &&
            ch != '\\') {
          break;
        }
      }
      PushExpression();
      *type_out = Emit(JsKeywords::kIdentifier, index, token_out);
      return true;
    }
  }
  return false;
}

JsKeywords::Type JsTokenizer::ConsumeColon(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ(':', input_[0]);
  if (parse_stack_.back() == kOperator && speculative_operator_) {
    // A colon directly after a speculatively-classified operator word: as in
    // ConsumeComma, the word (`await`, or a for-of `of`) has no operand, so
    // it was really a plain identifier -- a ternary alternative
    // (`cond ? await : val`) or a label.  Treat it as that expression and
    // let the loop below find the question mark, exactly as the kReturnThrow
    // case does for `yield`.
    parse_stack_.pop_back();
    PushExpression();
  }
  while (true) {
    DCHECK(!parse_stack_.empty());
    switch (parse_stack_.back()) {
      // If we reach a kQuestionMark, this colon is part of a ternary
      // operator.  Remove the kQuestionMark and replace it with a kOperator.
      case kQuestionMark:
        parse_stack_.pop_back();
        PushOperator();
        return Emit(JsKeywords::kOperator, 1, token_out);
      // If we reach the start of the statement without seeing a kQuestionMark,
      // this was a label.  No need to push any new parse state.
      case kStartOfInput:
      case kBlockHeader:
        return Emit(JsKeywords::kOperator, 1, token_out);
      // If we hit an open brace, check if it's for an object literal or a
      // block.  If it's an object literal, then this colon was for a property
      // name; push a kOperator state so that we know that what follows is an
      // expression (rather than the next property name).  If it's a block,
      // then we're back to start-of-statement (as above) so there's no need to
      // push any new parse state.
      case kOpenBrace:
        // Since the top state is currently kOpenBrace, and the bottom state is
        // always kStartOfInput, we know that the parse stack has at least two
        // entries right now.
        DCHECK_GE(parse_stack_.size(), 2u);
        if (CanPreceedObjectLiteral(parse_stack_[parse_stack_.size() - 2])) {
          // An object-literal property colon: install the value marker
          // (rather than a plain operator, which the expression collapse
          // would eat) so that a value expression never lands directly on
          // the brace and a following property name is the only expression
          // that does -- the method-shorthand discriminator (see
          // ConsumeOpenParen).
          parse_stack_.push_back(kObjectValue);
        }
        return Emit(JsKeywords::kOperator, 1, token_out);
      // Skip past anything that could lie between the colon and the question
      // mark or start-of-statement.  This includes the kOtherKeyword parse
      // state for the sake of the "default" keyword, the kArrow state for
      // the sake of an arrow in a ternary branch (`a ? x => b : c`), and
      // the kReturnThrow state for the sake of a `yield:` label (sloppy
      // mode; `return:`/`throw:` were already invalid JS).
      case kExpression:
      case kOtherKeyword:
      case kArrow:
      case kReturnThrow:
        parse_stack_.pop_back();
        break;
      // Reaching any other parse state is an error.
      case kOperator:
      case kPeriod:
      case kOptionalChain:
      case kOpenBracket:
      case kOpenParen:
      case kBlockKeyword:
      case kJumpKeyword:
      case kModuleDecl:
      case kFromClause:
      case kModuleVarKeyword:
      case kObjectValue:
      case kClassKeyword:
      case kClassBrace:
        return Error(token_out);
      default:
        LOG(DFATAL) << "Unknown parse state: " << parse_stack_.back();
        return Error(token_out);
    }
  }
}

JsKeywords::Type JsTokenizer::ConsumeComma(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ(',', input_[0]);
  if (parse_stack_.back() == kOperator && speculative_operator_) {
    // A comma directly after a speculatively-classified operator word: the
    // word is `await` (or a for-of `of`) with no operand, which means it was
    // really a plain identifier all along (`var await, x`, `f(await, 1)`,
    // `[await, 1]`, `{a: await, b: 1}` -- all legal sloppy-mode code).
    // Treat it as the expression it really is and let the normal comma paths
    // decide, exactly as the kReturnThrow branch below does for `yield`.
    parse_stack_.pop_back();
    PushExpression();
  }
  if (parse_stack_.back() == kReturnThrow) {
    // A comma directly after return/throw/yield: outside generators
    // `yield` is an ordinary identifier (`f(yield, 2)`, `var yield, x`),
    // and inside a generator `yield, 2` is a comma expression over a bare
    // yield -- so treat the keyword as the expression it just produced
    // and let the normal comma paths decide (`return, 2` and `throw, 2`
    // were already invalid JS, tolerated byte-preservingly).
    parse_stack_.pop_back();
    PushExpression();
  }
  // A comma directly over a completed arrow body (an expression body
  // collapsed to [kArrow, kExpression]) ENDS the arrow: the body is an
  // AssignmentExpression, which a comma cannot continue -- `get: () => 1,
  // set(v) {}` is a property plus a setter method, `foo(() => 1, 2)` is
  // two arguments, and `x = () => 1, 2` is a sequence expression
  // (node-verified).  Pop the arrow head(s) and let the normal comma
  // path decide at the level below.
  while (parse_stack_.size() >= 2 && parse_stack_.back() == kExpression &&
         parse_stack_[parse_stack_.size() - 2] == kArrow) {
    parse_stack_.pop_back();  // The body expression.
    parse_stack_.pop_back();  // The arrow head.
    PushExpression();         // Re-collapse onto the level below.
  }
  // A keyword rename target in an import/export clause (`export { a as
  // default, b }`, `export { a as if, b }`) is complete at the following
  // comma: pop the keyword state it pushed, then let the normal clause
  // comma path decide.  (Import-side renames to reserved words are
  // invalid JS -- engines reject `import { a as default }` -- only
  // tolerated byte-preservingly.)
  const ParseState rename_state = parse_stack_.back();
  if ((rename_state == kOtherKeyword || rename_state == kBlockKeyword) &&
      parse_stack_.size() >= 4 &&
      parse_stack_[parse_stack_.size() - 2] == kExpression &&
      parse_stack_[parse_stack_.size() - 3] == kOpenBrace &&
      parse_stack_[parse_stack_.size() - 4] == kModuleDecl) {
    parse_stack_.pop_back();
  }
  const ParseState state = parse_stack_.back();
  if (state == kExpression) {
    // Since the top state is currently kExpression, and the bottom state is
    // always kStartOfInput, we know that the parse stack has at least two
    // entries right now.
    DCHECK_GE(parse_stack_.size(), 2u);
    const ParseState prev = parse_stack_[parse_stack_.size() - 2];
    // One use of commas is as the separator for array/object literals and for
    // identifier lists for e.g. the var keyword.  For any of those, pop the
    // stack back up to the opening delimiter, so that we see the same parse
    // stack state for each item in the list.
    if (prev == kOtherKeyword || prev == kModuleVarKeyword ||
        prev == kObjectValue || prev == kOpenBracket ||
        (prev == kOpenBrace &&
         // Similarly, if the second-from-top state is kOpenBrace (or anything
         // else other than kStartOfInput), we know the parse stack has at
         // least three entries.
         CanPreceedObjectLiteral(parse_stack_[parse_stack_.size() - 3]))) {
      parse_stack_.pop_back();
      // A comma ending an object-literal property value also pops the
      // kObjectValue the property colon installed, returning to the
      // property position for the next entry.
      if (prev == kObjectValue) {
        parse_stack_.pop_back();
      }
      // A declarator comma after an initialized declarator (`var x = 1, y`,
      // exported or not) also pops the declaration keyword installed by the
      // initializer's `=`, so that the next binding sits directly over the
      // variable keyword and ASI after the bare binding fires.  (A comma
      // after `export default <expr>` is invalid JS -- `default` takes an
      // AssignmentExpression -- so carving back there too is harmless.)
      if (prev == kOtherKeyword && parse_stack_.size() >= 2 &&
          (parse_stack_[parse_stack_.size() - 2] == kModuleDecl ||
           parse_stack_[parse_stack_.size() - 2] == kModuleVarKeyword)) {
        parse_stack_.pop_back();
      }
    } else {
      // A comma can also be a binary operator (executing the first operand and
      // returning the second, as it does in C).
      PushOperator();
    }
  } else if (state != kOpenBracket) {
    // The only time commas show up other than right after an expression or
    // identifier is when you have an array literal with missing entries, such
    // as [,2,,3].  So if the top state isn't kExpression, it had better be
    // kOpenBracket.
    return Error(token_out);
  }
  return Emit(JsKeywords::kOperator, 1, token_out);
}

bool JsTokenizer::TryConsumeIdentifierOrKeyword(JsKeywords::Type* type_out,
                                                StringPiece* token_out) {
  DCHECK(!input_.empty());
  // This method gets very hot under load, and regex matching is slow.  We need
  // RE2 here mainly for the unicode support, but most JS files are plain
  // ASCII.  So first try to match against ASCII identifiers; only if we run
  // into a non-ASCII byte will we resort to RE2.
  int index = 0;
  {
    bool use_regex = false;
    const unsigned char first = input_[0];
    if (first >= 0x80) {
      use_regex = true;
    } else if (('a' <= first && first <= 'z') || first == '_' ||
               ('A' <= first && first <= 'Z') || first == '$' ||
               first == '\\') {
      int size = input_.size();
      for (index = 1; index < size; ++index) {
        const unsigned char ch = input_[index];
        if (ch >= 0x80) {
          use_regex = true;
          break;
        } else if (!net_instaweb::IsAsciiAlphaNumeric(ch) && ch != '_' &&
                   ch != '$' && ch != '\\') {
          break;
        }
      }
    } else {
      return false;
    }
    if (use_regex) {
      Re2StringPiece unconsumed = StringPieceToRe2(input_);
      if (!RE2::Consume(&unconsumed, patterns_->identifier_pattern)) {
        return false;
      }
      index = input_.size() - unconsumed.size();
    }
  }
  DCHECK_GT(index, 0);
  // We have a match.  Determine which keyword it is, if any.
  JsKeywords::Flag flag_ignored;
  JsKeywords::Type type =
      JsKeywords::Lookup(input_.substr(0, index), &flag_ignored);
  // A reserved word immediately after a period operator (or its ES2020
  // optional-chaining cousin `?.`) is treated as an identifier.  For example,
  // even though "if" is normally a reserved word, "foo.if" is legal code, and
  // is equivalent to "foo['if']" (and so is "foo?.if").  Similarly, a
  // reserved word is an identifier when used as a property name for an object
  // literal.
  if (parse_stack_.back() == kPeriod || parse_stack_.back() == kOptionalChain ||
      (parse_stack_.back() == kOpenBrace &&
       CanPreceedObjectLiteral(parse_stack_[parse_stack_.size() - 2]))) {
    PushExpression();
    *type_out = Emit(JsKeywords::kIdentifier, index, token_out);
    return true;
  }
  // Set for the speculatively-classified operator words (await/yield/of);
  // see LastTokenWasSpeculativeOperator().
  bool speculative = false;
  switch (type) {
    // If the word isn't a keyword, then it's an identifier.  Also, these other
    // "keywords" are only reserved for future use in strict mode, and
    // otherwise are legal identifiers.  Since we don't detect strict mode
    // errors yet, just always allow them as identifiers.  (`yield` is the
    // exception: it is reserved inside generators, which this tokenizer now
    // models, so it takes its own case below.)
    case JsKeywords::kNotAKeyword:
    case JsKeywords::kImplements:
    case JsKeywords::kInterface:
    case JsKeywords::kPackage:
    case JsKeywords::kPrivate:
    case JsKeywords::kProtected:
    case JsKeywords::kPublic:
    case JsKeywords::kStatic:
      type = JsKeywords::kIdentifier;
      // `of` is a contextual keyword too: an identifier directly after an
      // expression is either the for-of keyword or the start of an
      // ASI-separated statement.  Treat it as a binary operator so that a
      // following slash starts a regex (e.g. `for (m of /re/.exec(s))`).
      // This is fail-safe for the ASI reading: a misread slash yields a
      // verbatim pseudo-regex or a declined minification (see ConsumeRegex),
      // and a following linebreak is preserved by the minifier (see
      // LastTokenWasSpeculativeOperator).  Member-name position is excluded:
      // the kExpression there is a `get`/`set`/`async` modifier, so the `of`
      // is unambiguously the member's NAME (`{ get of(){} }`), and pushing
      // an operator would leave the method's `{` unmatched.
      //
      // `from` and `as` are contextual keywords, not reserved words, so they
      // arrive here as ordinary identifiers.  Inside an import/export
      // declaration they take dedicated parse states:
      //  - `from` at a module declaration point (kModuleDecl directly under
      //    a kExpression: after a default binding, a clause, or a namespace
      //    binding) or directly after the `*` of `export *` begins the
      //    from-clause.  It moves the declaration to the kFromClause state,
      //    which suppresses semicolon insertion until the module specifier
      //    arrives -- and makes insertion after the specifier unconditional.
      //  - `as` after the `*` of `import *`/`export *` (a kOperator over
      //    the marker, with at most one kExpression between, as in
      //    `import a, * as ns`) takes the kPeriod path so the binding that
      //    follows collapses back to a module declaration point.
      // Everywhere else they remain ordinary identifiers.
      if (parse_stack_.back() == kExpression && !AtMemberNameAfterModifier() &&
          input_.substr(0, index) == "of") {
        PushOperator();
        speculative = true;
      } else if (parse_stack_.size() >= 2 &&
                 parse_stack_[parse_stack_.size() - 2] == kModuleDecl &&
                 (parse_stack_.back() == kExpression ||
                  parse_stack_.back() == kOperator) &&
                 input_.substr(0, index) == "from") {
        parse_stack_.pop_back();
        parse_stack_.push_back(kFromClause);
      } else if (parse_stack_.back() == kOperator && parse_stack_.size() >= 2 &&
                 (parse_stack_[parse_stack_.size() - 2] == kModuleDecl ||
                  (parse_stack_.size() >= 3 &&
                   parse_stack_[parse_stack_.size() - 2] == kExpression &&
                   parse_stack_[parse_stack_.size() - 3] == kModuleDecl)) &&
                 input_.substr(0, index) == "as") {
        parse_stack_.push_back(kPeriod);
      } else if (parse_stack_.back() != kBlockKeyword &&
                 parse_stack_.back() != kClassKeyword) {
        // An identifier just after a kBlockKeyword is the name of a function
        // declaration (and just after a kClassKeyword the name of a class);
        // we just ignore it and leave the parse state alone.  Other
        // identifiers are treated as kExpressions.
        PushExpression();
      }
      break;
    // ES2015 `let` is a declaration keyword only at statement position AND
    // when what follows can begin a binding; everywhere else it is still a
    // legal identifier (`var let = 1;`, `let = 5;`, `let++;`, `let / 2;` are
    // all valid non-strict code).  Statement position means: start of input,
    // start of a statement inside a block, or directly after a block header
    // such as `if (x)`.  (A kOpenBrace at the top of the stack here is
    // always a block: had it been an object literal, the property-name
    // branch above would already have consumed this word as an identifier.)
    // The set deliberately excludes kOpenParen and kOperator, so `let` in a
    // for-header stays an identifier; the residual `for (let {a} of xs)`
    // therefore still errors out (byte-preserving) -- pinned in tests, do
    // not "fix" by widening the set without revisiting the sloppy-mode
    // traces.  kModuleDecl (directly after `export`) IS a statement
    // position, but with a narrower binding lookahead (below): only an
    // identifier-start character qualifies, so `export let {a} = b` keeps
    // its pinned error residual (the identifier path leaves the `{` to
    // error out, byte-preserving) while `export let x` becomes a
    // declaration that pushes a kModuleVarKeyword (below).
    //
    // Binding lookahead (mirrors the spec's cover-grammar disambiguation):
    // classify as a declaration only if the next non-whitespace character is
    // an ASCII identifier-start character ('a'-'z', 'A'-'Z', '_', '$', or a
    // '\\' unicode escape) or '{' or '['.  Anything else -- an operator,
    // EOF, a comment, or a non-ASCII byte (which could be unicode
    // whitespace) -- takes the identifier path, which is exactly the old
    // tokenizer's behavior: a lookahead miss degrades to byte-identical
    // pre-ES2015 tokenization, never to an error or a misclassified slash.
    // (Cost: `let /*c*/ x = 1` and `let \u{3c0} = 1` tokenize as
    // identifier-then-identifier, which is still byte-preserving.)
    // A declaration `let` behaves like const/var: a linebreak after it never
    // inserts a semicolon, and a slash after it is an error.  The keyword
    // pushes a kModuleVarKeyword (whether or not it sits over the module
    // marker): the binding then lands at the declaration's bare-binding
    // point, where only `,` or `=` continue (see
    // TryInsertLinebreakSemicolon), and the `=` of an initializer installs
    // the kOtherKeyword itself (see ConsumeOperator).
    case JsKeywords::kLet: {
      const ParseState let_state = parse_stack_.back();
      const bool stmt_position =
          let_state == kStartOfInput || let_state == kOpenBrace ||
          let_state == kBlockHeader || let_state == kModuleDecl;
      bool is_declaration = false;
      if (stmt_position) {
        const int size = input_.size();
        int i = index;
        while (i < size &&
               (input_[i] == ' ' || input_[i] == '\t' || input_[i] == '\f' ||
                input_[i] == '\v' || input_[i] == '\n' || input_[i] == '\r')) {
          ++i;
        }
        if (i < size) {
          const char c = input_[i];
          is_declaration =
              (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_' ||
               c == '$' || c == '\\' ||
               // A `{`/`[` binding pattern is a declaration everywhere
               // except over the module marker, where the pinned
               // `export let {a} = b` residual is preserved instead.
               (let_state != kModuleDecl && (c == '{' || c == '[')));
        }
      }
      if (is_declaration) {
        parse_stack_.push_back(kModuleVarKeyword);
      } else {
        type = JsKeywords::kIdentifier;
        if (parse_stack_.back() != kBlockKeyword) {
          PushExpression();
        }
      }
      break;
    }
    // These keywords are expressions.  A slash after one of these is division
    // (rather than a regex literal).  ES2015 `super` is a primary-expression
    // head (`super(...)`, `super.x`, `super[i]`), so it belongs here too.
    case JsKeywords::kFalse:
    case JsKeywords::kNull:
    case JsKeywords::kSuper:
    case JsKeywords::kThis:
    case JsKeywords::kTrue:
      PushExpression();
      break;
    // These keywords must be followed by something in parentheses.  A slash
    // immediately after one of these is invalid; a slash after the parentheses
    // is the start of a regex literal (rather than division).
    case JsKeywords::kCatch:
    case JsKeywords::kFor:
    case JsKeywords::kFunction:
    case JsKeywords::kIf:
    case JsKeywords::kSwitch:
    case JsKeywords::kWhile:
    case JsKeywords::kWith:
      parse_stack_.push_back(kBlockKeyword);
      break;
    // These keywords mark the start of a block.  A slash after one of these is
    // the start of a regex literal (rather than division); an open brace after
    // one of these is the start of a block (rather than an object literal).
    case JsKeywords::kDo:
    case JsKeywords::kElse:
    case JsKeywords::kFinally:
    case JsKeywords::kTry:
      PushBlockHeader();
      break;
    // These keywords act like operators (sort of).  A slash after one of these
    // marks the start of a regex literal (rather than division); an open brace
    // after one of these is the start of an object literal (rather than a
    // block).
    case JsKeywords::kCase:
    case JsKeywords::kDelete:
    case JsKeywords::kIn:
    case JsKeywords::kInstanceof:
    case JsKeywords::kNew:
    case JsKeywords::kTypeof:
    case JsKeywords::kVoid:
      PushOperator();
      break;
    // These two keywords are like prefix operators in their treatment of
    // slashes, but a linebreak after them always induces semicolon insertion.
    case JsKeywords::kReturn:
    case JsKeywords::kThrow:
      parse_stack_.push_back(kReturnThrow);
      break;
    // `yield` is a keyword inside generators (which this tokenizer now
    // models) and a legal identifier everywhere else.  Treat it like
    // `return`/`throw`: a slash after it starts a regex literal, and a
    // linebreak after it always inserts a semicolon.  Inside a generator
    // that is exactly right -- `yield /re/g` yields the regex (reading the
    // slash as division would re-emit a bare `g` identifier where engines
    // then report a ReferenceError), and a LineTerminator before a
    // delegated `*` is a SyntaxError in engines, so the newline must not
    // be dropped.  Outside generators the extra insertions are byte-safe:
    // a kept newline re-parses identically, since engines only insert
    // when the next token cannot continue anyway.  After a block keyword
    // it is a function name (`function yield() {}`), and in member-name
    // position it is a member NAME (`{ get yield(){} }`).  Because the
    // identifier reading stays possible, the classification is speculative
    // (see LastTokenWasSpeculativeOperator).
    case JsKeywords::kYield:
      if (parse_stack_.back() == kBlockKeyword ||
          parse_stack_.back() == kClassKeyword) {
        // `class yield {}` is a SyntaxError in every mode (class bodies
        // are strict), but taking the name path keeps the tokenization
        // byte-preserving instead of committing the body's braces to a
        // divergent reading.
        type = JsKeywords::kIdentifier;
      } else if (AtMemberNameAfterModifier()) {
        type = JsKeywords::kIdentifier;
        PushExpression();
      } else {
        parse_stack_.push_back(kReturnThrow);
        speculative = true;
      }
      break;
    // `await` acts like a prefix operator: a slash after it starts a regex
    // literal (not division), an open brace after it starts an object
    // literal, and a linebreak after it never induces semicolon insertion
    // (await is not a restricted production).  It keeps the plain kOperator
    // state -- not kReturnThrow, which `yield` can afford because a yield
    // expression is a whole statement's worth of parse state, whereas an
    // await expression must collapse into its surrounding expression
    // (`{ a: await g(), b: 1 }`).  The `,`/`:` that may follow the
    // IDENTIFIER reading is instead tolerated at the separator itself; see
    // the speculative-operator carve-outs in ConsumeComma and ConsumeColon.
    // Directly after a block keyword `await` is the `for await` modifier
    // (`for await (const x of s)`) or the name of a function declaration
    // (`function await() {}`, legal in sloppy mode), and in member-name
    // position it is a member NAME (`{ get await(){} }`, `{ *await(){} }`),
    // so leave the operator reading alone in both.  Outside an async
    // function `await` is a plain identifier, which is why the
    // classification is speculative (see LastTokenWasSpeculativeOperator).
    case JsKeywords::kAwait:
      if (parse_stack_.back() == kBlockKeyword ||
          parse_stack_.back() == kClassKeyword) {
        // Leave the parse state alone; `await` names the function or the
        // class (`class await {}`, legal in sloppy mode -- the class-body
        // `{` must take the kClassBrace path, never the speculative
        // object-literal commit) or modifies the `for`.
      } else if (AtMemberNameAfterModifier()) {
        type = JsKeywords::kIdentifier;
        PushExpression();
      } else {
        PushOperator();
        speculative = true;
      }
      break;
    // These keywords can't have a division operator or a regex literal after
    // them, so a slash after one of these is an error (not counting comments,
    // of course).  Moreover, a linebreak after them always induces semicolon
    // insertion.
    case JsKeywords::kBreak:
    case JsKeywords::kContinue:
    case JsKeywords::kDebugger:
      parse_stack_.push_back(kJumpKeyword);
      break;
    // These keywords also can't have a division operator or a regex literal
    // after them.  However, a linebreak after them never induces semicolon
    // insertion.  They push a kModuleVarKeyword (exactly like a declaration
    // `let` above), so the binding lands at the declaration's bare-binding
    // point, where only `,` or `=` continue -- the same restricted point the
    // module-declaration fix established for `export var/const x`.
    case JsKeywords::kConst:
    case JsKeywords::kVar:
      parse_stack_.push_back(kModuleVarKeyword);
      break;
    case JsKeywords::kDefault:
      parse_stack_.push_back(kOtherKeyword);
      break;
    // ES2015 `import` and `export` begin module declarations.  At statement
    // position we push a kModuleDecl marker that anchors the declaration:
    // braces after it are clause-shaped (object-literal-shaped), a linebreak
    // directly after the keyword never inserts a semicolon (`import\n{a}`
    // continues the statement), and ASI fires at the declaration's
    // grammatical end regardless of the next token (see
    // TryInsertLinebreakSemicolon and kModuleContinuationRegex).  `import`
    // followed by `(` or `.` is the dynamic import() call or import.meta:
    // ConsumeOpenParen/ConsumePeriod convert the marker back to a plain
    // kOperator, so those tokenize exactly like ordinary expressions.  At
    // expression position (e.g. `x = import('y')`) both keywords keep the
    // plain-operator behavior.  Known residual, accepted deliberately: in
    // `export default function(){}` the function body's closing brace
    // leaves the `default` kOtherKeyword on top of the stack (see
    // ConsumeCloseBrace), so a regex literal directly after it -- genuine
    // but vanishingly rare JS, e.g. `export default function(){}/re/...` --
    // is reported as an error, preserving the input byte-for-byte rather
    // than mis-tokenizing it.  That error-preservation holds only for the
    // plain form: in `export default async function(){}` the `async`
    // identifier interposes a kExpression, so the closing brace leaves an
    // Expr on top and a following slash is classified as division.  That is
    // the pre-existing async-function misclassification family
    // (`async function f(){}/re/` behaves identically), newly reachable now
    // that `export` tokenizes; pinned in tests as-is.
    case JsKeywords::kImport:
    case JsKeywords::kExport:
      if (parse_stack_.back() == kStartOfInput ||
          parse_stack_.back() == kOpenBrace ||
          parse_stack_.back() == kBlockHeader) {
        parse_stack_.push_back(kModuleDecl);
      } else {
        PushOperator();
      }
      break;
    // `class` opens a class header: the keyword (and the heritage span)
    // takes a kClassKeyword state; the `{` of the body completes it (see
    // ConsumeOpenBrace).  `extends` is the heritage operator there, and
    // reserved everywhere else (as is `enum`, in ALL modes), so those
    // still error byte-preservingly.
    case JsKeywords::kClass:
      parse_stack_.push_back(kClassKeyword);
      break;
    case JsKeywords::kExtends:
      if (parse_stack_.back() == kClassKeyword) {
        PushOperator();
      } else {
        *type_out = Error(token_out);
        return true;
      }
      break;
    case JsKeywords::kEnum:
      *type_out = Error(token_out);
      return true;
    default:
      LOG(DFATAL) << "Unknown keyword type: " << type;
      *type_out = Error(token_out);
      return true;
  }
  *type_out = Emit(type, index, token_out);
  // Emit() cleared the flag; re-open the window for the speculative words.
  speculative_operator_ = speculative;
  return true;
}

JsKeywords::Type JsTokenizer::ConsumeNumber(StringPiece* token_out) {
  DCHECK(!input_.empty());
  Re2StringPiece unconsumed = StringPieceToRe2(input_);
  if (!RE2::Consume(&unconsumed, patterns_->numeric_literal_pattern)) {
    // We only call ConsumeNumber when we're sure we're looking at a numeric
    // literal, so this ought not happen even for pathalogical input.
    LOG(DFATAL) << "Failed to match number pattern: " << input_.substr(0, 50);
    return Error(token_out);
  }
  PushExpression();
  return Emit(JsKeywords::kNumber, input_.size() - unconsumed.size(),
              token_out);
}

JsKeywords::Type JsTokenizer::ConsumeOperator(StringPiece* token_out) {
  DCHECK(!input_.empty());
  Re2StringPiece unconsumed = StringPieceToRe2(input_);
  if (!RE2::Consume(&unconsumed, patterns_->operator_pattern)) {
    // Unrecognized character:
    return Error(token_out);
  }
  // Sampled before Emit() below clears it; see the update-operator carry at
  // the end of this method.
  const bool was_speculative = speculative_operator_;
  const JsKeywords::Type type =
      Emit(JsKeywords::kOperator, input_.size() - unconsumed.size(), token_out);
  const StringPiece token = *token_out;
  // Is this a postfix operator?  We treat those differently than prefix or
  // unary operators.
  DCHECK(!parse_stack_.empty());
  if ((token == "++" || token == "--") && parse_stack_.back() == kExpression) {
    // Postfix operator; leave the parse state as kExpression ("an
    // expression followed by a postfix operator is still just an
    // expression"), but remember that the expression is now a completed
    // UpdateExpression, whose continuation set is strictly narrower than
    // a general expression's: no call or member access can attach to it.
    // TryInsertLinebreakSemicolon consults this flag at the next
    // linebreak.  (Emit() already ran for this token above, so the flag
    // armed here survives until the NEXT significant token clears it.)
    postfix_update_pending_ = true;
  } else if (token == "=" && !input_.empty() && input_[0] == '>') {
    // An arrow head `=>` (per ECMA-262 a single punctuator, though emitted
    // here as separate `=` and `>` tokens to preserve the old byte stream):
    // push a kArrow state so that a `{...}` body is recognized as a block
    // when it closes (see ConsumeCloseBrace).  The state sits under the
    // body like an operator, so expression-body continuations keep the
    // ordinary rules.  This check comes before the declaration-initializer
    // case below: `export let f => ...` lexes as an arrow (invalid JS,
    // tolerated byte-preservingly).
    parse_stack_.push_back(kArrow);
  } else if (token[0] == '>' &&
             parse_stack_.back() == kArrow) {  // NOLINT(bugprone-branch-clone)
    // The `>` of the arrow head: leave the kArrow state on the stack; the
    // generator-`*` branch below intentionally repeats this empty body.
    // (A kArrow directly on top is only ever seen here, right after the
    // `=`; anything else this combines with was already invalid JS.)
  } else if (token[0] == '*' && parse_stack_.back() == kBlockKeyword) {
    // The `*` of `function*` (and `async function*`): a generator marker,
    // not a binary operator -- leave the block keyword on the stack so the
    // name and parameter list complete into a block header exactly like a
    // plain function's.  (`if * x` and the like were already invalid JS;
    // leaving the keyword there stays byte-preserving.)
  } else if (token == "*" &&
             (IsMemberNameBrace(parse_stack_.size() - 1) ||
              AtMemberNameAfterModifier()) &&
             NextCharStartsIdentifier()) {
    // The `*` of a generator method shorthand with a plain name (`{ *m(){} }`,
    // `{ async *m(){} }`, `class X { *m(){} }`): also a generator marker, so
    // push a block keyword just like `function*` above.  The name then takes
    // the function-name path -- which is what makes `{ *await(){} }` and
    // `{ *yield(){} }` name their methods rather than push an operator -- and
    // the parameter list completes into a block header.
    //
    // The name's function-name path pushes no expression, so without more the
    // closing brace of the body would land on the bare member-name brace --
    // where a following comma (the member separator) is an error, declining
    // minification of `{ *m(){}, b: 2 }`.  A plain method name (or an `async`
    // modifier) leaves a kExpression under the block header, and the close
    // then lands on that.  Install the same expression here, under the block
    // keyword, so the completed method closes back to exactly the state a
    // plain method leaves and the comma takes the normal member-separator
    // path.  (When an `async` modifier already left that expression,
    // PushExpression() simply merges with it.)
    //
    // The identifier lookahead keeps every other member-name form on the
    // plain-operator path below, which already handles them: a computed name
    // (`{ *[Symbol.iterator](){} }`) would error on the `[` after a block
    // keyword, and string/numeric names have no reason to move.  (A binary
    // `*` directly on a member-name brace was already invalid JS.)
    PushExpression();
    parse_stack_.push_back(kBlockKeyword);
  } else if (token == "=" &&
             ((parse_stack_.size() >= 2 && parse_stack_.back() == kExpression &&
               (parse_stack_[parse_stack_.size() - 2] == kModuleDecl ||
                parse_stack_[parse_stack_.size() - 2] == kModuleVarKeyword ||
                parse_stack_[parse_stack_.size() - 2] == kClassBrace)) ||
              (parse_stack_.size() >= 3 && parse_stack_.back() == kExpression &&
               parse_stack_[parse_stack_.size() - 2] == kOpenBrace &&
               CanPreceedObjectLiteral(
                   parse_stack_[parse_stack_.size() - 3])))) {
    // The `=` of an initializer: a variable initializer (`let/const/var x
    // = ...`, exported or not; also of an invalid `import ... = ...`,
    // tolerated), a class field initializer (`class X { a = ... }`), or a
    // destructuring default (`const { canvas = f() } = x`, or a parameter
    // pattern).  In all three the right-hand side is an ordinary
    // expression (an AssignmentExpression) with ordinary ASI continuation
    // rules, so install a kOtherKeyword over the declaration marker, class
    // body brace, or pattern brace before pushing the operator.  The
    // initializer then collapses to [..., kOtherKeyword, kExpression],
    // which is NOT a bare-binding point for TryInsertLinebreakSemicolon.
    // For class fields and destructuring patterns the marker also keeps
    // the initializer from collapsing back onto the brace
    // indistinguishably from an element name, which would misfire the
    // method-shorthand gate on call/member continuations
    // (`class X { a = 1\n(2) }` is a call, not a method).  (An `=`
    // shorthand in a real object literal was already invalid JS, tolerated
    // byte-preservingly.)
    parse_stack_.pop_back();
    parse_stack_.push_back(kOtherKeyword);
    PushOperator();
  } else {
    // Prefix or binary operator; push it onto the stack.
    PushOperator();
  }
  // An update operator consumed while the speculative window was open
  // (`await++`, `yield--`): under the identifier reading the word and the
  // `++`/`--` form a completed postfix UpdateExpression, so a linebreak
  // after the pair may be load-bearing for semicolon insertion; under the
  // operator-word reading the `++`/`--` is a prefix operator, and a
  // linebreak before its operand is legal.  Preserving the linebreak is
  // therefore semantics-neutral in both readings, so keep the window open
  // across the update operator -- the minifier then preserves the
  // linebreak, and a slash after the pair keeps the fail-safe regex guard.
  // (Emit() above cleared the flag.  The postfix branch cannot race this:
  // it requires a kExpression top, which means the previous token already
  // closed the window.)
  if ((token == "++" || token == "--") && was_speculative) {
    speculative_operator_ = true;
  }
  return type;
}

JsKeywords::Type JsTokenizer::ConsumePeriod(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('.', input_[0]);
  if (input_.size() >= 2) {
    const int next = static_cast<unsigned char>(input_[1]);
    if (next >= '0' && next <= '9') {
      return ConsumeNumber(token_out);
    }
  }
  // ES2015 spread `...` (and rest in destructuring and parameter lists):
  // emit it as a single operator token; like other prefix operators, an
  // expression (and a regex literal) may follow.
  if (input_.size() >= 3 && input_[1] == '.' && input_[2] == '.') {
    PushOperator();
    return Emit(JsKeywords::kOperator, 3, token_out);
  }
  // `import.meta` (or a member access on a dynamic import) is not a
  // declaration: drop the module-declaration marker, exactly as
  // ConsumeOpenParen does for `import(...)`.
  if (parse_stack_.back() == kModuleDecl) {
    parse_stack_.pop_back();
    PushOperator();
  }
  parse_stack_.push_back(kPeriod);
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeQuestionMark(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('?', input_[0]);
  DCHECK(!parse_stack_.empty());
  // A `?` can grow the stack without a pop until its `:` arrives
  // ("1?1?1?..."), so it needs the depth cap too.
  if (parse_stack_.size() >= kMaxParseStackDepth) {
    return Error(token_out);
  }
  if (parse_stack_.back() != kExpression) {
    // Outside generators `yield` is an ordinary identifier (`x = yield ?
    // 1 : 2`), so treat the keyword as the expression it just produced.
    // (`return ?` and `throw ?` were already invalid JS, as is
    // `yield ? 1 : 2` inside a generator -- all byte-preserving.)
    if (parse_stack_.back() == kReturnThrow) {
      parse_stack_.pop_back();
      PushExpression();
    } else {
      return Error(token_out);
    }
  }
  if (input_.size() >= 2 && input_[1] == '?') {
    // ES2020 nullish coalescing `??`, or ES2021 logical assignment `??=`.
    // Both are binary operators: a slash after them starts a regex literal,
    // and an open brace after them starts an object literal.
    const int length = (input_.size() >= 3 && input_[2] == '=') ? 3 : 2;
    PushOperator();
    return Emit(JsKeywords::kOperator, length, token_out);
  }
  if (input_.size() >= 2 && input_[1] == '.' &&
      !(input_.size() >= 3 && input_[2] >= '0' && input_[2] <= '9')) {
    // ES2020 optional chaining `?.`.  Per ECMA-262, OptionalChainingPunctuator
    // is `?.` [lookahead not in DecimalDigit], so `a?.5:b` is the ternary
    // `a ? .5 : b` and falls through to the kQuestionMark path below.
    parse_stack_.push_back(kOptionalChain);
    return Emit(JsKeywords::kOperator, 2, token_out);
  }
  parse_stack_.push_back(kQuestionMark);
  return Emit(JsKeywords::kOperator, 1, token_out);
}

namespace {

// Walks a regex literal starting at its opening slash and returns the index
// of its closing slash, or npos if it is unterminated.  A slash inside a
// character class is implicitly escaped and does not close the literal;
// classes do not nest, so a single flag tracks them.
size_t FindRegexClosingSlash(StringPiece input) {
  bool in_class = false;
  bool escaped = false;
  for (size_t i = 1; i < input.size(); ++i) {
    const char ch = input[i];
    if (escaped) {
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (in_class) {
      in_class = (ch != ']');
    } else if (ch == '[') {
      in_class = true;
    } else if (ch == '/') {
      return i;
    }
  }
  return StringPiece::npos;
}

}  // namespace

bool SpeculativeRegexCanReassembleComment(StringPiece input) {
  DCHECK(!input.empty());
  DCHECK_EQ('/', input[0]);
  const size_t close = FindRegexClosingSlash(input);
  if (close == StringPiece::npos) {
    // Unterminated.  The callers only get here after a successful scan, but
    // decline rather than guess.
    return true;
  }
  // The right boundary: with flags, input[close + 1] is a flag letter and
  // nothing can weld onto it; without flags it is the next input byte, and a
  // "/" or "*" there welds onto the closing slash as soon as the whitespace
  // between them is deleted.  (This is the guard's whole purpose; the input's
  // own comments are not the hazard -- they are comments under either reading
  // and at the same byte.)
  if (close + 1 < input.size() &&
      (input[close + 1] == '/' || input[close + 1] == '*')) {
    return true;
  }
  // Comment delimiters INSIDE the literal.  Every "//" or "/*" among the
  // bytes about to be emitted verbatim opens a comment under the division
  // reading, whatever it means under the regex reading, so this test ignores
  // escaping.
  bool in_class = false;
  bool escaped = false;
  for (size_t i = 1; i < close; ++i) {
    const char ch = input[i];
    const bool delimiter =
        ch == '/' && (input[i + 1] == '/' || input[i + 1] == '*');
    if (delimiter) {
      if (!in_class) {
        // Outside a character class an unescaped slash would already have
        // closed the literal, so a delimiter here was formed by an escape
        // (`/a\//`, `/a\/*b/`).  Decline: it is vanishingly rare, and the
        // division reading of such input is not valid JavaScript anyway.
        return true;
      }
      if (input[i + 1] == '*') {
        // Under the division reading a "/*" inside what the regex reading
        // calls a character class opens a BLOCK comment.  That is harmless
        // only if the comment cannot swallow anything the minifier rewrote:
        // either it closes inside the verbatim literal, or it never closes
        // at all -- in which case the division reading is not valid
        // JavaScript.
        const size_t end = input.find("*/", i + 2);
        if (end != StringPiece::npos && end + 2 > close) {
          return true;
        }
      }
      // A "//" inside a character class is harmless: under the division
      // reading it opens a LINE comment, and a regex literal may not contain
      // a line terminator, so the class's own `]` and the closing slash are
      // both swallowed by it -- leaving the `[` unclosed for the rest of the
      // line, which no valid division reading recovers from.
    }
    if (escaped) {
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (in_class) {
      in_class = (ch != ']');
    } else if (ch == '[') {
      in_class = true;
    }
  }
  return false;
}

JsKeywords::Type JsTokenizer::ConsumeRegex(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('/', input_[0]);
  Re2StringPiece unconsumed = StringPieceToRe2(input_);
  if (!RE2::Consume(&unconsumed, patterns_->regex_literal_pattern)) {
    // EOF or a linebreak in the regex will cause an error.
    return Error(token_out);
  }
  const size_t num_chars = input_.size() - unconsumed.size();
  // Invariant: a regex consumed directly after a speculatively-classified
  // operator word (await/yield/for-of "of" -- each of which may really be a
  // plain identifier, making this slash division) must not let minification
  // reassemble a comment delimiter that was not in the input.  Declining
  // (error) is fail-safe: minification fails and the caller serves the
  // original input unmodified.
  if (speculative_operator_ && SpeculativeRegexCanReassembleComment(input_)) {
    return Error(token_out);
  }
  PushExpression();
  return Emit(JsKeywords::kRegex, num_chars, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeSemicolon(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ(';', input_[0]);
  // Semicolons can appear either at the end of a statement, or within a
  // for-loop header.  So pop the parse state back to the previous open brace
  // (or start of input) for end-of-statement, or the previous open paren (in
  // which case we'd better be within a block header).
  while (true) {
    DCHECK(!parse_stack_.empty());
    const ParseState state = parse_stack_.back();
    if (state == kOpenBracket || state == kTemplateInterp) {
      return Error(token_out);
    } else if (state == kOpenParen) {
      // Semicolon within parens is only okay if it's a for-loop header, so the
      // parse state below the kOpenParen had better be kBlockKeyword (for the
      // "for" keyword) or else this is a parse error.  (Since the top state is
      // currently kOpenParen, and the bottom state is always kStartOfInput, we
      // know that the parse stack has at least two entries right now).
      DCHECK_GE(parse_stack_.size(), 2u);
      if (parse_stack_[parse_stack_.size() - 2] != kBlockKeyword) {
        return Error(token_out);
      }
      break;
    } else if (state == kStartOfInput || state == kOpenBrace ||
               state == kClassBrace) {
      // A `;` inside a class body is a legal no-op (and a field separator).
      break;
    }
    parse_stack_.pop_back();
  }
  // Emit a token for the semicolon.
  return Emit(JsKeywords::kOperator, 1, token_out);
}

JsKeywords::Type JsTokenizer::ConsumeSlash(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK_EQ('/', input_[0]);
  // If the slash is immediately followed by a slash or star, it's a comment,
  // no matter what the current parse state is.
  if (input_.size() >= 2) {
    const int next = static_cast<unsigned char>(input_[1]);
    if (next == '/') {
      return ConsumeLineComment(token_out);
    } else if (next == '*') {
      const JsKeywords::Type type = ConsumeBlockComment(token_out);
      // A block comment that contains a line terminator counts as a line
      // terminator for automatic semicolon insertion (ECMA-262), so run the
      // same insertion logic a real linebreak would get: "return/*\n*/x"
      // must not be joined into "return x", and "x/*\n*/++y" must not be
      // joined into the syntax error "x++y".  (Comments pulled into
      // TryInsertLinebreakSemicolon's own lookahead queue do not pass
      // through here, so this cannot re-enter.)  Conditional-compilation
      // comments are left as plain comments: the minifier retains them
      // verbatim, and the retained text itself carries the linebreak into
      // the output.
      if (type == JsKeywords::kComment &&
          CommentHasLineTerminator(*token_out) &&
          !IsConditionalCompilationComment(*token_out)) {
        start_of_line_ = true;
        if (TryInsertLinebreakSemicolon()) {
          return JsKeywords::kSemiInsert;
        }
      }
      return type;
    }
  }
  // Otherwise, we have to consult the current parse state to decide if this
  // slash is a division operator or the start of a regex literal.
  DCHECK(!parse_stack_.empty());
  switch (parse_stack_.back()) {
    case kExpression:
      return ConsumeOperator(token_out);
    case kStartOfInput:
    case kOperator:
    case kQuestionMark:
    case kOpenBrace:
    case kOpenBracket:
    case kOpenParen:
    case kTemplateInterp:
    case kBlockHeader:
    case kReturnThrow:
    case kArrow:  // `x => /re/`: a regex literal starts an expression body.
    case kObjectValue:  // A regex literal can start a property value.
      return ConsumeRegex(token_out);
    case kPeriod:
    case kOptionalChain:
    case kBlockKeyword:
    case kJumpKeyword:
    case kOtherKeyword:
    case kModuleDecl:
    case kFromClause:
    case kModuleVarKeyword:
    case kClassKeyword:
    case kClassBrace:
      return Error(token_out);
    default:
      LOG(DFATAL) << "Unknown parse state: " << parse_stack_.back();
      return Error(token_out);
  }
}

JsKeywords::Type JsTokenizer::ConsumeString(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK(input_[0] == '"' || input_[0] == '\'');
  Re2StringPiece unconsumed = StringPieceToRe2(input_);
  if (!RE2::Consume(&unconsumed, patterns_->string_literal_pattern) ||
      input_[input_.size() - unconsumed.size() - 1] != input_[0]) {
    // EOF or an unescaped linebreak in the string will cause an error.
    return Error(token_out);
  }
  // A string literal directly over the module marker is the module specifier
  // of a bare import (`import 'x'`): move the declaration to the from-clause
  // shape so that ASI always fires after the specifier, exactly as it does
  // after the specifier of a from-clause.
  if (parse_stack_.back() == kModuleDecl) {
    parse_stack_.push_back(kFromClause);
  }
  PushExpression();
  return Emit(JsKeywords::kStringLiteral, input_.size() - unconsumed.size(),
              token_out);
}

bool JsTokenizer::NearestOpenDelimiterIsTemplateInterp() const {
  // Walk from the top of the stack, skipping expression/operator/keyword
  // states, until we reach an open delimiter (or the bottom of the stack).
  for (std::vector<ParseState>::const_reverse_iterator
           iter = parse_stack_.rbegin(),
           end = parse_stack_.rend();
       iter != end; ++iter) {
    switch (*iter) {
      case kTemplateInterp:
        return true;
      case kStartOfInput:
      case kOpenBrace:
      case kOpenBracket:
      case kOpenParen:
      case kBlockKeyword:
      case kClassBrace:
        // kClassBrace IS a delimiter here: inside a template
        // interpolation a class body `}` closes the class (and a template
        // after it is a tagged template on the class expression,
        // node-verified), not template text.  (Without this stop the walk
        // continued past the class body down to the interpolation and
        // consumed the class's `}` as template text.)
        return false;
      default:
        // A non-delimiter state (expression, operator, block header, or a
        // keyword state): keep looking further down the stack.
        break;
    }
  }
  return false;
}

bool JsTokenizer::NearestOpenDelimiterIsClassBrace() const {
  // Same walk as the template-interpolation check, for the class body
  // brace.  kClassBrace itself is a delimiter here (a nested class body
  // belongs to the innermost class).
  for (std::vector<ParseState>::const_reverse_iterator
           iter = parse_stack_.rbegin(),
           end = parse_stack_.rend();
       iter != end; ++iter) {
    switch (*iter) {
      case kClassBrace:
        return true;
      case kStartOfInput:
      case kOpenBrace:
      case kOpenBracket:
      case kOpenParen:
      case kBlockKeyword:
      case kTemplateInterp:
        return false;
      default:
        // A non-delimiter state (expression, operator, block header, or a
        // keyword state): keep looking further down the stack.
        break;
    }
  }
  return false;
}

JsKeywords::Type JsTokenizer::ConsumeTemplateChunk(StringPiece* token_out) {
  DCHECK(!input_.empty());
  DCHECK(input_[0] == '`' || input_[0] == '}');
  // input_[0] is the chunk's opening delimiter (a backtick to begin a
  // template, or a '}' to resume one after an interpolation).  Scan forward
  // to the chunk's terminator: an unescaped backtick ends the template (this
  // chunk is a no-substitution template or a TemplateTail), while an
  // unescaped '${' starts an interpolation (this chunk is a TemplateHead or
  // TemplateMiddle).  Template literals may span multiple lines, so raw
  // linebreaks are permitted; a backslash escapes the following byte
  // (covering \`, \$, and \\).
  const size_t size = input_.size();
  for (size_t i = 1; i < size; ++i) {
    const char c = input_[i];
    if (c == '\\') {
      // Escape sequence: skip the next byte (if any).  If the backslash is
      // the final byte, the loop terminates and we report an unterminated
      // template below.
      ++i;
      continue;
    }
    if (c == '`') {
      // End of the template literal.  A template is a primary expression, so
      // a subsequent slash is division.
      PushExpression();
      return Emit(JsKeywords::kTemplateLiteral, i + 1, token_out);
    }
    if (c == '$' && i + 1 < size && input_[i + 1] == '{') {
      // Start of a ${...} interpolation.  The '${' is part of this chunk;
      // the interpolation body that follows is tokenized as ordinary JS
      // until the matching '}' resumes the template.
      if (parse_stack_.size() >= kMaxParseStackDepth) {
        return Error(token_out);
      }
      parse_stack_.push_back(kTemplateInterp);
      return Emit(JsKeywords::kTemplateLiteral, i + 2, token_out);
    }
  }
  // Reached end of input without a closing backtick or an interpolation:
  // the template is unterminated.  Bail conservatively.
  return Error(token_out);
}

bool JsTokenizer::TryConsumeWhitespace(bool allow_semicolon_insertion,
                                       JsKeywords::Type* type_out,
                                       StringPiece* token_out) {
  DCHECK(!input_.empty());
  // This method gets very hot under load, and regex matching is slow.  We need
  // RE2 here mainly for the unicode support, but most JS files are plain
  // ASCII.  So first try to match against ASCII whitespace; only if we run
  // into a non-ASCII byte will we resort to RE2.
  bool has_linebreak = false;
  bool use_regex = false;
  int token_size = 0, size = input_.size();
  for (; token_size < size; ++token_size) {
    const unsigned char ch = input_[token_size];
    if (ch >= 0x80) {
      use_regex = true;
      break;
    } else if (ch == '\n' || ch == '\r') {
      has_linebreak = true;
    } else if (ch != ' ' && ch != '\t' && ch != '\f' && ch != '\v') {
      break;
    }
  }
  if (use_regex) {
    Re2StringPiece unconsumed = StringPieceToRe2(input_);
    Re2StringPiece linebreak;
    if (!RE2::Consume(&unconsumed, patterns_->whitespace_pattern, &linebreak)) {
      return false;
    }
    has_linebreak = !linebreak.empty();
    token_size = input_.size() - unconsumed.size();
    DCHECK_GT(token_size, 0);
  }
  if (token_size == 0) {
    return false;
  }
  // Yep, this was whitespace.  Emit a token now, since we may need to do some
  // lookahead in a moment.  We may change *type_out in a moment, but
  // kWhitespace is good enough to get Emit() to do the right thing for now.
  *type_out = Emit(JsKeywords::kWhitespace, token_size, token_out);
  // Now we have to decide what kind of whitespace this was.  If it contained
  // no linebreaks, it's just regular whitespace; otherwise, we have to decide
  // whether or not this linebreak will cause semicolon insertion, and set
  // *type_out accordingly.
  if (has_linebreak) {
    start_of_line_ = true;
    if (allow_semicolon_insertion && TryInsertLinebreakSemicolon()) {
      *type_out = JsKeywords::kSemiInsert;
    } else {
      *type_out = JsKeywords::kLineSeparator;
    }
  }
  return true;
}

JsKeywords::Type JsTokenizer::Error(StringPiece* token_out) {
  error_ = true;
  *token_out = input_;
  input_ = StringPiece();
  return JsKeywords::kError;
}

JsKeywords::Type JsTokenizer::Emit(JsKeywords::Type type, int num_chars,
                                   StringPiece* token_out) {
  DCHECK_GT(num_chars, 0);
  DCHECK_LE(static_cast<size_t>(num_chars), input_.size());
  const StringPiece token = input_.substr(0, num_chars);
  if (type != JsKeywords::kComment && type != JsKeywords::kWhitespace &&
      type != JsKeywords::kLineSeparator && type != JsKeywords::kSemiInsert) {
    start_of_line_ = false;
    // Any real token continues (or ends) the construct the arrow body was
    // part of, so a pending terminal-arrow ASI no longer applies.
    // (ConsumeCloseBrace arms the flag only after emitting its own `}`.)
    arrow_body_asi_pending_ = false;
    // Likewise for a pending postfix-update ASI: any real token either
    // legally continues the UpdateExpression or ends its statement.
    // (ConsumeOperator arms the flag only after emitting its own `++`/`--`.)
    postfix_update_pending_ = false;
    // Any significant token ends the speculative-operator window; the cases
    // in TryConsumeIdentifierOrKeyword that open the window re-set the flag
    // after calling Emit().
    speculative_operator_ = false;
    speculative_linebreak_ = false;
    // Check if it looks like we're tokenizing a JSON object rather than JS
    // code.  If the first three tokens in the input are open brace, string
    // literal, colon, then this is a JSON object (since that would be illegal
    // syntax at the start of JS code), and we should tweak the parse stack so
    // that we treat the outer braces as an object literal rather than as a
    // code block.  If the first three tokens in the input are anything else,
    // then we can assume this is JS code.
    switch (json_step_) {
      case kJsonStart:
        if (type == JsKeywords::kOperator && token == "{") {
          json_step_ = kJsonOpenBrace;
        } else {
          json_step_ = kIsNotJsonObject;
        }
        break;
      case kJsonOpenBrace:
        if (type == JsKeywords::kStringLiteral) {
          json_step_ = kJsonOpenBraceStringLiteral;
        } else {
          json_step_ = kIsNotJsonObject;
        }
        break;
      case kJsonOpenBraceStringLiteral:
        if (type == JsKeywords::kOperator && token == ":") {
          json_step_ = kIsJsonObject;
          // The first three tokens were open brace, string literal, colon.
          // That will make the parse stack look like "Start {".  We will add
          // an Oper state in between Start and { to make the braces look like
          // an object literal, and then add an Oper state at the end, since
          // that's what we do for colons in an object literal.  The resulting
          // parse stack is "Start Oper { Oper", and we can just continue as
          // normal from there.
          DCHECK_EQ(2u, parse_stack_.size());
          DCHECK_EQ(kStartOfInput, parse_stack_[0]);
          DCHECK_EQ(kOpenBrace, parse_stack_[1]);
          parse_stack_.pop_back();
          parse_stack_.push_back(kOperator);
          parse_stack_.push_back(kOpenBrace);
          parse_stack_.push_back(kOperator);
        } else {
          json_step_ = kIsNotJsonObject;
        }
        break;
      default:
        break;
    }
  } else if (speculative_operator_) {
    // Whitespace or a comment inside an open speculative window: record
    // whether it carries a line terminator (LF, CR, or the UTF-8 LS/PS
    // sequences) -- a terminator inside a block comment counts for ASI
    // exactly like bare whitespace, so the scan looks at token content,
    // not token type.  ConsumeOpenBrace narrows its decline to this case.
    if (token.find('\n') != StringPiece::npos ||
        token.find('\r') != StringPiece::npos ||
        token.find("\xE2\x80\xA8") != StringPiece::npos ||
        token.find("\xE2\x80\xA9") != StringPiece::npos) {
      speculative_linebreak_ = true;
    }
  }
  *token_out = token;
  input_ = input_.substr(num_chars);
  return type;
}

void JsTokenizer::PushBlockHeader() {
  // Push a kBlockHeader state onto the stack, but if there's already a
  // kBlockHeader on the stack (e.g. as in "else if (...)"), merge the two
  // together by simply leaving the stack alone.
  DCHECK(!parse_stack_.empty());
  if (parse_stack_.back() != kBlockHeader) {
    parse_stack_.push_back(kBlockHeader);
  }
}

void JsTokenizer::PushExpression() {
  // Push a kExpression state onto the stack, merging it with any kExpression or
  // kOperator states on top (e.g. so "a + b" -> "Expr Oper Expr" becomes "Expr"
  // and "foo(1)" -> "Expr ( Expr )" becomes "Expr Expr" becomes "Expr").
  DCHECK(!parse_stack_.empty());
  while (parse_stack_.back() == kExpression ||
         parse_stack_.back() == kOperator || parse_stack_.back() == kPeriod ||
         parse_stack_.back() == kOptionalChain) {
    parse_stack_.pop_back();
    DCHECK(!parse_stack_.empty());
  }
  parse_stack_.push_back(kExpression);
}

void JsTokenizer::PushOperator() {
  // Push a kOperator state onto the stack, but if there's already a kOperator
  // on the stack (e.g. as in "x && !y"), merge the two together by simply
  // leaving the stack alone.
  DCHECK(!parse_stack_.empty());
  if (parse_stack_.back() != kOperator) {
    parse_stack_.push_back(kOperator);
  }
}

bool JsTokenizer::TryInsertLinebreakSemicolon() {
  // Determining whether semicolon insertion happens requires checking the next
  // non-whitespace/comment token, so skip past any comments and whitespace and
  // store them in the lookahead queue.  Note that whether or not the linebreak
  // we're considering in this method inserts a semicolon, the subsequent
  // whitespace we're about to skip past certainly won't.
  DCHECK(lookahead_queue_.empty());
  {
    JsKeywords::Type type;
    StringPiece token;
    while (!input_.empty() && (TryConsumeComment(&type, &token) ||
                               TryConsumeWhitespace(false, &type, &token))) {
      lookahead_queue_.emplace_back(type, token);
    }
  }
  // Even if semicolon insertion would technically happen for the linebreak
  // here, we will pretend that it won't if we're about to hit a real
  // semicolon, or if the semicolon would be inserted anyway without the
  // linebreak.
  if (input_.empty() || input_[0] == ';' || input_[0] == '}') {
    return false;
  }
  // Whether semicolon insertion can happen depends on the current parse state.
  DCHECK(!parse_stack_.empty());
  switch (parse_stack_.back()) {
    case kStartOfInput:
    case kOpenBrace:
    case kOpenBracket:
    case kOpenParen:
    case kTemplateInterp:
    case kBlockKeyword:
    case kBlockHeader:
    case kClassBrace:
      // Semicolon insertion never happens in places where it would create an
      // empty statement (or an empty class element).
      return false;
    case kExpression:
      // A statement can't end with an unclosed paren or bracket; in
      // particular, semicolons for a for-loop header are never inserted.
      for (std::vector<ParseState>::const_reverse_iterator
               iter = parse_stack_.rbegin(),
               end = parse_stack_.rend();
           iter != end; ++iter) {
        const ParseState state = *iter;
        if (state == kOpenParen || state == kOpenBracket ||
            state == kTemplateInterp) {
          return false;
        }
        if (state == kOpenBrace || state == kBlockHeader) {
          break;
        }
      }
      // After a completed block-bodied arrow (the flag armed at its
      // closing brace) nothing can continue the ArrowFunction -- not even
      // the operators and parens in the generic continuation set -- so ASI
      // always fires.  (An expression body is still open instead, with
      // ordinary continuation rules, and never arms the flag.)
      if (arrow_body_asi_pending_) {
        arrow_body_asi_pending_ = false;
        break;
      }
      // After the module specifier of an import/export declaration (or
      // after a completed `export [async] function` body) nothing can
      // continue the declaration, so ASI always fires -- the generic
      // continuation set, and even the `from` continuation, do not apply
      // here (`import 'x'\nfrom = 5;` is two statements).
      if (parse_stack_.size() >= 2 &&
          parse_stack_[parse_stack_.size() - 2] == kFromClause) {
        break;
      }
      // At the bare-binding point of a variable declaration
      // (kModuleVarKeyword directly below the expression), only `,` (the
      // next declarator) and `=` (the initializer) can continue.
      if (parse_stack_.size() >= 2 &&
          parse_stack_[parse_stack_.size() - 2] == kModuleVarKeyword) {
        Re2StringPiece unconsumed = StringPieceToRe2(input_);
        if (RE2::Consume(&unconsumed,
                         patterns_->module_var_continuation_pattern)) {
          return false;
        }
        break;
      }
      // At any other module declaration point (kModuleDecl directly below
      // the expression: after an import/export binding, clause, or
      // namespace binding) the generic continuation set is too broad --
      // `(`, `/`, and the like cannot continue an import/export
      // declaration.  Only the declaration continuations `from` and `,`
      // suppress insertion here.
      if (parse_stack_.size() >= 2 &&
          parse_stack_[parse_stack_.size() - 2] == kModuleDecl) {
        Re2StringPiece unconsumed = StringPieceToRe2(input_);
        if (RE2::Consume(&unconsumed, patterns_->module_continuation_pattern)) {
          return false;
        }
        break;
      }
      // In a class heritage span (`class X extends Y`), the `{` of the
      // class body is not a statement block and never inserts -- engines
      // treat a linebreak between the heritage expression and the body as
      // insignificant (node-verified for empty and method bodies).
      if (parse_stack_.size() >= 2 &&
          parse_stack_[parse_stack_.size() - 2] == kClassKeyword) {
        return false;
      }
      // At a class-element position (the expression sits directly on the
      // class body brace) the completed expression is an element: a field
      // name, a computed name, or a just-closed method.  Only `=` (a field
      // initializer) or `(` (a method parameter list) can continue an
      // element there, and neither reading is linebreak-sensitive.  The
      // generic continuation set below is too broad for this position --
      // `*` most notably begins a FOLLOWING generator method, and the
      // linebreak after a bare field is ASI-load-bearing:
      // `class C { x \n *gen(){} }` corrupts to `x*gen(){}` (parsed as a
      // multiplication in field-initializer position) if it is dropped.
      if (parse_stack_.size() >= 2 &&
          parse_stack_[parse_stack_.size() - 2] == kClassBrace) {
        // NOTE: the '=' test also admits '==' / '===' / '=>' here, but no
        // class element can begin with any of those, so both readings of
        // such input are SyntaxErrors and the choice cannot matter.
        if (input_[0] == '=' || input_[0] == '(') {
          return false;
        }
        break;
      }
      // After a postfix `++`/`--` (flag armed in ConsumeOperator) the
      // completed UpdateExpression cannot take a call or member access, so
      // a following `(`, or a `.` starting a numeric literal, begins a new
      // statement and the linebreak inserts a semicolon -- even though the
      // generic continuation set below contains both `(` and `.`.  (Every
      // other member of that set is a binary/ternary operator, which
      // legally continues an UpdateExpression.)
      if (postfix_update_pending_ &&
          (input_[0] == '(' || (input_[0] == '.' && input_.size() >= 2 &&
                                input_[1] >= '0' && input_[1] <= '9'))) {
        postfix_update_pending_ = false;
        break;
      }
      // Semicolon insertion will not happen after an expression if the next
      // token could continue the statement.
      {
        Re2StringPiece unconsumed = StringPieceToRe2(input_);
        if (RE2::Consume(&unconsumed, patterns_->line_continuation_pattern)) {
          return false;
        }
      }
      break;
    // Binary and prefix operators (including the arrow head `=>`, whose
    // body is still to come, and the property colon awaiting its value)
    // should not have semicolon insertion happen after them.
    case kOperator:
    case kPeriod:
    case kOptionalChain:
    case kQuestionMark:
    case kArrow:
    case kObjectValue:
      return false;
    // Line continuations are never permitted after return, throw, break,
    // continue, or debugger keywords, so a semicolon is always inserted for
    // those.
    case kReturnThrow:
    case kJumpKeyword:
      break;
    // A statement cannot end after `default` or an initializer's `=`, so we
    // never insert a semicolon after those.  Nor can it end directly after
    // the `import`/`export` that opened a module declaration, after the
    // `from` awaiting its module specifier, or after the let/const/var that
    // opened a variable declaration.
    case kOtherKeyword:
    case kModuleDecl:
    case kFromClause:
    case kModuleVarKeyword:
    case kClassKeyword:
      return false;
    default:
      LOG(DFATAL) << "Unknown parse state: " << parse_stack_.back();
      break;
  }
  // We've decided at this point that semicolon insertion will happen, so
  // update the parse stack to end the current statement.
  while (true) {
    DCHECK(!parse_stack_.empty());
    const ParseState state = parse_stack_.back();
    if (state == kStartOfInput || state == kOpenBrace || state == kClassBrace) {
      // Inside a class body an inserted semicolon ends the current element
      // (a field declaration), not the class.
      break;
    }
    parse_stack_.pop_back();
  }
  return true;
}

bool JsTokenizer::CanPreceedObjectLiteral(ParseState state) {
  // kOtherKeyword is included for `export default {...}`: the braces start
  // an object expression.  ConsumeCloseBrace carves out the one case where
  // a block legitimately sits on top of a kOtherKeyword
  // (`export default function(){}`).  kModuleDecl is included for the
  // import/export clauses (`import {a} from 'x'`, `export {a}`), whose
  // braces are object-literal-shaped; kModuleVarKeyword is included for
  // destructuring binding patterns in variable declarations
  // (`const {a} = x`, `export var {a} = b`), which for our purposes are
  // likewise object-literal-shaped (property names, `:` renames, and the
  // closing brace producing an expression so that `= x` parses).  kArrow
  // is deliberately absent: an arrow's `{...}` is a BLOCK body, not an
  // object literal, so statement keywords inside it keep their keyword
  // paths (the one collapse that needs the arrow, a function-expression
  // body `x => function(){}`, is handled explicitly in ConsumeCloseBrace).
  // kOptionalChain is deliberately absent too: `a?.{` is never valid.
  return (state == kOperator || state == kQuestionMark ||
          state == kOpenBracket || state == kOpenParen ||
          state == kReturnThrow || state == kTemplateInterp ||
          state == kOtherKeyword || state == kModuleDecl ||
          state == kModuleVarKeyword || state == kObjectValue);
}

bool JsTokenizer::IsMemberNameBrace(size_t index) const {
  DCHECK_LT(index, parse_stack_.size());
  const ParseState state = parse_stack_[index];
  if (state == kClassBrace) {
    return true;
  }
  return state == kOpenBrace && index >= 1 &&
         CanPreceedObjectLiteral(parse_stack_[index - 1]);
}

bool JsTokenizer::AtMemberNameAfterModifier() const {
  const size_t size = parse_stack_.size();
  return size >= 2 && parse_stack_[size - 1] == kExpression &&
         IsMemberNameBrace(size - 2);
}

bool JsTokenizer::NextCharStartsIdentifier() const {
  const int size = input_.size();
  int i = 0;
  while (i < size &&
         (input_[i] == ' ' || input_[i] == '\t' || input_[i] == '\f' ||
          input_[i] == '\v' || input_[i] == '\n' || input_[i] == '\r')) {
    ++i;
  }
  if (i >= size) {
    return false;
  }
  const char c = input_[i];
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_' ||
         c == '$' || c == '\\';
}

JsTokenizerPatterns::JsTokenizerPatterns()
    : identifier_pattern(kIdentifierRegex),
      line_comment_pattern(kLineCommentRegex),
      numeric_literal_pattern(kNumericLiteralPosixRegex, re2::posix_syntax),
      operator_pattern(kOperatorRegex),
      regex_literal_pattern(kRegexLiteralRegex),
      string_literal_pattern(kStringLiteralRegex),
      whitespace_pattern(kWhitespaceRegex),
      line_continuation_pattern(kLineContinuationRegex),
      module_continuation_pattern(kModuleContinuationRegex),
      module_var_continuation_pattern(kModuleVarContinuationRegex) {
  DCHECK(identifier_pattern.ok());
  DCHECK(numeric_literal_pattern.ok());
  DCHECK(operator_pattern.ok());
  DCHECK(regex_literal_pattern.ok());
  DCHECK(string_literal_pattern.ok());
  DCHECK(whitespace_pattern.ok());
  DCHECK(line_continuation_pattern.ok());
  DCHECK(module_continuation_pattern.ok());
  DCHECK(module_var_continuation_pattern.ok());
}

JsTokenizerPatterns::~JsTokenizerPatterns() {}

}  // namespace js

}  // namespace pagespeed
