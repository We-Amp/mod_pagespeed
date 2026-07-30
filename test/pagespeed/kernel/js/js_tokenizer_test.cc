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

#include "pagespeed/kernel/js/js_tokenizer.h"

#include <memory>
#include <utility>

#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/js/js_keywords.h"
#include "test/pagespeed/kernel/base/gtest.h"

using pagespeed::JsKeywords;
using pagespeed::js::JsTokenizer;
using pagespeed::js::JsTokenizerPatterns;

namespace {

const char kTestRootDir[] = "/test/pagespeed/kernel/js/testdata/third_party/";

class JsTokenizerTest : public testing::Test {
 protected:
  void BeginTokenizing(StringPiece input) {
    tokenizer_ = std::make_unique<JsTokenizer>(&patterns_, input);
  }

  void ExpectParseStack(StringPiece expected_parse_stack) {
    EXPECT_STREQ(expected_parse_stack, tokenizer_->ParseStackForTest());
  }

  void ExpectSpeculativeOperatorWindow(bool open) {
    EXPECT_EQ(open, tokenizer_->LastTokenWasSpeculativeOperator());
  }

  void ExpectToken(JsKeywords::Type expected_type, StringPiece expected_token) {
    StringPiece actual_token;
    const JsKeywords::Type actual_type = tokenizer_->NextToken(&actual_token);
    EXPECT_EQ(std::make_pair(expected_type, expected_token),
              std::make_pair(actual_type, actual_token));
    EXPECT_FALSE(tokenizer_->has_error());
  }

  void ExpectToken(JsKeywords::Type expected_type, StringPiece expected_token,
                   StringPiece expected_parse_stack) {
    ExpectToken(expected_type, expected_token);
    ExpectParseStack(expected_parse_stack);
  }

  void ExpectEndOfInput() {
    ExpectToken(JsKeywords::kEndOfInput, "");
    ExpectParseStack("");
  }

  void ExpectError(StringPiece expected_token) {
    StringPiece actual_token;
    const JsKeywords::Type actual_type = tokenizer_->NextToken(&actual_token);
    EXPECT_EQ(std::make_pair(JsKeywords::kError, expected_token),
              std::make_pair(actual_type, actual_token));
    EXPECT_TRUE(tokenizer_->has_error());
  }

  void ExpectTokenizeFileSuccessfully(StringPiece filename) {
    // Read in the JavaScript file.
    GoogleString original;
    {
      net_instaweb::StdioFileSystem file_system;
      const GoogleString filepath =
          StrCat(net_instaweb::GTestSrcDir(), kTestRootDir, filename);
      net_instaweb::GoogleMessageHandler message_handler;
      ASSERT_TRUE(
          file_system.ReadFile(filepath.c_str(), &original, &message_handler));
    }
    // Tokenize the JavaScript, appending each token onto the output string.
    // There should be no tokenizer errors.
    GoogleString output;
    {
      output.reserve(original.size());
      JsTokenizer tokenizer(&patterns_, original);
      StringPiece token;
      while (tokenizer.NextToken(&token) != JsKeywords::kEndOfInput) {
        ASSERT_FALSE(tokenizer.has_error())
            << "Error at: " << token.substr(0, 150)
            << "\nWith stack: " << tokenizer.ParseStackForTest();
        token.AppendToString(&output);
      }
    }
    // The concatenation of all tokens should exactly reproduce the input.
    EXPECT_STREQ(original, output);
  }

 private:
  JsTokenizerPatterns patterns_;
  std::unique_ptr<JsTokenizer> tokenizer_;
};

TEST_F(JsTokenizerTest, EmptyInput) {
  BeginTokenizing("");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Blocks) {
  BeginTokenizing(
      "if (foo){\n"
      "}else if(bar){\n"
      "}else baz;");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kIf, "if", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "foo", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kElse, "else", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIf, "if", "Start BkHdr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "bar", "Start BkHdr BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kElse, "else", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "baz", "Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Functions) {
  BeginTokenizing(
      "function foo(){return 1}\n"
      "bar=function(){return 2};\n"
      "(function(){window=5})();");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kFunction, "function");
  ExpectParseStack("Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectParseStack("Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start BkHdr {");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectParseStack("Start BkHdr { RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1");
  ExpectParseStack("Start BkHdr { RetTh Expr");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "bar");
  ExpectParseStack("Start Expr");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kFunction, "function");
  ExpectParseStack("Start Expr Oper BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start Expr Oper BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start Expr Oper BkHdr {");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectParseStack("Start Expr Oper BkHdr { RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2");
  ExpectParseStack("Start Expr Oper BkHdr { RetTh Expr");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start Expr");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start (");
  ExpectToken(JsKeywords::kFunction, "function");
  ExpectParseStack("Start ( BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start ( BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start ( BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start ( BkHdr {");
  ExpectToken(JsKeywords::kIdentifier, "window");
  ExpectParseStack("Start ( BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectParseStack("Start ( BkHdr { Expr Oper");
  ExpectToken(JsKeywords::kNumber, "5");
  ExpectParseStack("Start ( BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start Expr");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start Expr (");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start Expr");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectParseStack("Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ForLoop) {
  BeginTokenizing(
      "loop:for(var x=0;x<5;++x){\n"
      "  break loop;\n"
      "}");
  ExpectToken(JsKeywords::kIdentifier, "loop");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kFor, "for");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kVar, "var");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kNumber, "0");
  ExpectParseStack("Start BkKwd ( MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectParseStack("Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "<");
  ExpectToken(JsKeywords::kNumber, "5");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectParseStack("Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n  ");

  ExpectToken(JsKeywords::kBreak, "break");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "loop");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kOperator, "}");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Keywords) {
  // A piece of code that contains every keyword at least once.
  BeginTokenizing(
      "(function(){\n"
      " var\n"
      "   x=typeof null\n"
      " const y=void false\n"
      " delete x\n"
      " if(this instanceof String){\n"
      "  debugger\n"
      "  for(z in this)\n"
      "   continue\n"
      "  do break;while(true)\n"
      "  switch(y){\n"
      "   case 0:\n"
      "   default:\n"
      "    try{\n"
      "     with(this){\n"
      "      throw new Object()\n"
      "     }\n"
      "    }catch(e){\n"
      "     return\n"
      "    }finally{}\n"
      "  }\n"
      " }else return\n"
      "})");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kFunction, "function");
  ExpectParseStack("Start ( BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kLineSeparator, "\n ");
  ExpectParseStack("Start ( BkHdr {");

  ExpectToken(JsKeywords::kVar, "var");
  ExpectParseStack("Start ( BkHdr { MVar");
  ExpectToken(JsKeywords::kLineSeparator, "\n   ");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kTypeof, "typeof");
  ExpectParseStack("Start ( BkHdr { MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNull, "null");
  ExpectParseStack("Start ( BkHdr { MVar Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n ");
  ExpectParseStack("Start ( BkHdr {");

  ExpectToken(JsKeywords::kConst, "const");
  ExpectParseStack("Start ( BkHdr { MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kVoid, "void");
  ExpectParseStack("Start ( BkHdr { MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFalse, "false");
  ExpectParseStack("Start ( BkHdr { MVar Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n ");
  ExpectParseStack("Start ( BkHdr {");

  ExpectToken(JsKeywords::kDelete, "delete");
  ExpectParseStack("Start ( BkHdr { Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kSemiInsert, "\n ");
  ExpectParseStack("Start ( BkHdr {");

  ExpectToken(JsKeywords::kIf, "if");
  ExpectParseStack("Start ( BkHdr { BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kThis, "this");
  ExpectParseStack("Start ( BkHdr { BkKwd ( Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kInstanceof, "instanceof");
  ExpectParseStack("Start ( BkHdr { BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "String");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kLineSeparator, "\n  ");
  ExpectParseStack("Start ( BkHdr { BkHdr {");

  ExpectToken(JsKeywords::kDebugger, "debugger");
  ExpectParseStack("Start ( BkHdr { BkHdr { Jump");
  ExpectToken(JsKeywords::kSemiInsert, "\n  ");
  ExpectParseStack("Start ( BkHdr { BkHdr {");

  ExpectToken(JsKeywords::kFor, "for");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kIdentifier, "z");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIn, "in");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kThis, "this");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kLineSeparator, "\n   ");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr");

  ExpectToken(JsKeywords::kContinue, "continue");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr Jump");
  ExpectToken(JsKeywords::kSemiInsert, "\n  ");
  ExpectParseStack("Start ( BkHdr { BkHdr {");

  ExpectToken(JsKeywords::kDo, "do");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kBreak, "break");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr Jump");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectParseStack("Start ( BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kWhile, "while");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kTrue, "true");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr");
  ExpectToken(JsKeywords::kLineSeparator, "\n  ");

  ExpectToken(JsKeywords::kSwitch, "switch");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n   ");

  ExpectToken(JsKeywords::kCase, "case");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "0");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n   ");

  ExpectToken(JsKeywords::kDefault, "default");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { Other");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n    ");

  ExpectToken(JsKeywords::kTry, "try");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kLineSeparator, "\n     ");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr {");

  ExpectToken(JsKeywords::kWith, "with");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kThis, "this");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kLineSeparator, "\n      ");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkHdr {");

  ExpectToken(JsKeywords::kThrow, "throw");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkHdr { RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNew, "new");
  ExpectParseStack(
      "Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkHdr { RetTh Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "Object");
  ExpectParseStack(
      "Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkHdr { RetTh Expr");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kLineSeparator, "\n     ");
  ExpectParseStack(
      "Start ( BkHdr { BkHdr { BkHdr { BkHdr { BkHdr { RetTh Expr");

  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kLineSeparator, "\n    ");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kCatch, "catch");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kIdentifier, "e");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kLineSeparator, "\n     ");

  ExpectToken(JsKeywords::kReturn, "return");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr { RetTh");
  ExpectToken(JsKeywords::kLineSeparator, "\n    ");

  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kFinally, "finally");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kLineSeparator, "\n  ");
  ExpectParseStack("Start ( BkHdr { BkHdr { BkHdr {");

  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kLineSeparator, "\n ");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start ( BkHdr {");
  ExpectToken(JsKeywords::kElse, "else");
  ExpectParseStack("Start ( BkHdr { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectParseStack("Start ( BkHdr { BkHdr RetTh");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kOperator, "}");
  ExpectParseStack("Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start Expr");
  ExpectEndOfInput();
  ExpectParseStack("");
}

TEST_F(JsTokenizerTest, StrictModeReservedWords) {
  // These names are reserved words in strict mode, but otherwise they're legal
  // identifiers.
  // TODO(mdsteele): At some point we may want to implement strict mode error
  //   checking, at which point we should add a test with "use strict".
  BeginTokenizing(
      "var implements,interface,let,package\n"
      "   ,private,protected,public,static,yield;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "implements", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar");
  ExpectToken(JsKeywords::kIdentifier, "interface");
  ExpectToken(JsKeywords::kOperator, ",");
  ExpectToken(JsKeywords::kIdentifier, "let");
  ExpectToken(JsKeywords::kOperator, ",");
  ExpectToken(JsKeywords::kIdentifier, "package");
  ExpectToken(JsKeywords::kLineSeparator, "\n   ");

  ExpectToken(JsKeywords::kOperator, ",");
  ExpectToken(JsKeywords::kIdentifier, "private");
  ExpectToken(JsKeywords::kOperator, ",");
  ExpectToken(JsKeywords::kIdentifier, "protected");
  ExpectToken(JsKeywords::kOperator, ",");
  ExpectToken(JsKeywords::kIdentifier, "public");
  ExpectToken(JsKeywords::kOperator, ",");
  ExpectToken(JsKeywords::kIdentifier, "static");
  ExpectToken(JsKeywords::kOperator, ",");
  // `yield` is the exception: it is reserved inside generators (which the
  // tokenizer models), so it takes the kReturnThrow state like
  // `return`/`throw` -- byte-safe for its sloppy identifier uses.
  ExpectToken(JsKeywords::kYield, "yield", "Start MVar RetTh");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, AwaitRegex) {
  // "await" acts like a prefix operator; a slash after it starts a regex
  // literal, not division.
  BeginTokenizing("return await / x /;");
  ExpectToken(JsKeywords::kReturn, "return", "Start RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start RetTh Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ x /", "Start RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, AwaitNoSemicolonInsertion) {
  // "await" is not a restricted production; a linebreak after it does not
  // induce semicolon insertion.
  BeginTokenizing("await\nx");
  ExpectToken(JsKeywords::kAwait, "await", "Start Oper");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, YieldRegex) {
  // "yield" behaves like return/throw; a slash after it starts a regex
  // literal, not division.
  BeginTokenizing("yield / x /;");
  ExpectToken(JsKeywords::kYield, "yield", "Start RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ x /", "Start RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, YieldSemicolonInsertion) {
  // "yield" is a restricted production, like return/throw: a linebreak after
  // it induces semicolon insertion.
  BeginTokenizing("yield\nx");
  ExpectToken(JsKeywords::kYield, "yield", "Start RetTh");
  ExpectToken(JsKeywords::kSemiInsert, "\n", "Start");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ForOfRegex) {
  // The contextual "of" keyword is still emitted as kIdentifier (so the
  // minifier preserves the space in "a of"), but it pushes an operator state
  // so that a following slash begins a regex literal.
  BeginTokenizing("for(a of / z /)b;");
  ExpectToken(JsKeywords::kFor, "for", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "of", "Start BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ z /", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, AwaitAfterPeriod) {
  // After a period, "await" is an ordinary property name, and a slash after
  // the member expression is division.
  BeginTokenizing("x.await/2;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Expr .");
  ExpectToken(JsKeywords::kIdentifier, "await", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, VarAwait) {
  // Outside an async function, "await" is a legal variable name.  We still
  // emit kAwait and push an operator state -- an intentional, fail-safe
  // degradation (see JsMinifyTest.AwaitAsVariableDegradation) -- which is
  // harmless here.
  BeginTokenizing("var await=1;");
  // "var" pushes the declaration state kModuleVarKeyword ("MVar").
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start MVar Oper");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Oper");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpeculativeRegexCommentDelimiterGuard) {
  // A regex consumed directly after "await"/"yield" (which may really be
  // plain identifiers, making the slash division) must neither contain a
  // comment delimiter nor abut one across its end: reassembly after
  // whitespace/comment removal could otherwise create "//" or "/*".  The
  // tokenizer errors, which makes minification fail closed.
  BeginTokenizing("x = await / 2 // c\n+ y;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("/ 2 // c\n+ y;");
}

TEST_F(JsTokenizerTest, SpeculativeRegexGuardIgnoresCharacterClasses) {
  // The comment-delimiter guard is about REASSEMBLY at the literal's right
  // boundary, so a `//` inside a character class -- which cannot open a
  // comment that escapes the verbatim literal -- does not trip it.
  BeginTokenizing("return await/[//]/g;");
  ExpectToken(JsKeywords::kReturn, "return", "Start RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start RetTh Oper");
  ExpectToken(JsKeywords::kRegex, "/[//]/g", "Start RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpeculativeRegexGuardIgnoresFlags) {
  // Nor does a comment after a FLAGGED literal: the last byte emitted is a
  // flag letter, and nothing welds onto that.
  BeginTokenizing("return await/a/g/*c*/;");
  ExpectToken(JsKeywords::kReturn, "return", "Start RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start RetTh Oper");
  ExpectToken(JsKeywords::kRegex, "/a/g", "Start RetTh Expr");
  ExpectToken(JsKeywords::kComment, "/*c*/", "Start RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpeculativeOperatorBlockGuard) {
  // A "{" directly after "await" (which may really be a plain identifier)
  // must not be committed to the object-literal reading: in the identifier
  // reading (an ASI-separated statement) the braces are a BLOCK, after which
  // a slash is a regex rather than division.  The tokenizer errors, which
  // makes minification fail closed.
  BeginTokenizing("var await = 1;\nawait\n{ } / x /.test(y);");
  // "var" pushes the declaration state kModuleVarKeyword ("MVar"); the block
  // guard after the ASI-separated "await" is unaffected by that state.
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start MVar Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kAwait, "await", "Start Oper");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectError("{ } / x /.test(y);");
}

TEST_F(JsTokenizerTest, MemberNameAfterModifierIsNotAnOperator) {
  // A `get`/`set`/`async` modifier leaves a kExpression on the object
  // literal's brace, and the word after it is the member's NAME, not an
  // operator: classifying `of` (or `await`/`yield`) as one there would leave
  // the method's block unmatched and error out a few tokens later.
  BeginTokenizing("x={get of(){}};");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "of", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassMemberNameAfterModifierIsNotAnOperator) {
  // The same rule inside a class body, where the modifier sits on the class
  // brace instead.
  BeginTokenizing("class C{get await(){}}");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "C", "Start Cls");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  // `await` in member-name position is emitted as a plain identifier.
  ExpectToken(JsKeywords::kIdentifier, "await", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, GeneratorMethodStarIsABlockKeyword) {
  // The `*` of a generator method shorthand with a plain name is a generator
  // marker, exactly like the `*` of `function*`: it pushes a block keyword,
  // so the name that follows takes the function-name path (which is what
  // keeps `*await` naming the method) and the parameter list completes into a
  // block header.  The marker also installs the expression a plain method
  // name would leave, so the body closes back to the same state and a
  // following comma is the member separator.
  BeginTokenizing("x={*await(){}};");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper { Expr BkKwd");
  ExpectToken(JsKeywords::kAwait, "await", "Start Expr Oper { Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, GeneratorMethodStarWithComputedNameStaysAnOperator) {
  // ...but only with a plain name.  A computed name would error on the `[`
  // after a block keyword, so the identifier lookahead keeps it on the
  // ordinary operator path.
  BeginTokenizing("x={*[k](){}};");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper { Oper");
  ExpectToken(JsKeywords::kOperator, "[", "Start Expr Oper { Oper [");
  ExpectToken(JsKeywords::kIdentifier, "k", "Start Expr Oper { Oper [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpeculativeOperatorFollowedBySeparator) {
  // A `,` or `:` directly after `await` means the word had no operand, so it
  // was really a plain identifier (all legal sloppy-mode code).  The
  // separator carve-outs in ConsumeComma/ConsumeColon collapse the
  // speculative operator into the expression it really is, exactly as the
  // kReturnThrow path does for `yield`.
  BeginTokenizing("var await,x;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kAwait, "await", "Start MVar Oper");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpeculativeOperatorFollowedByTernaryColon) {
  BeginTokenizing("x=c?await:v;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "c", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?", "Start Expr ?");
  ExpectToken(JsKeywords::kAwait, "await", "Start Expr ? Oper");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "v", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpeculativeUpdateOperatorKeepsWindow) {
  // A `++`/`--` consumed while the speculative window is open keeps it open
  // (`await++` may be a completed postfix UpdateExpression whose following
  // linebreak inserts a semicolon).  The linebreak arrives as a
  // kLineSeparator -- the parse stack alone would let the next token
  // continue -- and the minifier preserves it via
  // LastTokenWasSpeculativeOperator().
  BeginTokenizing("await++\nf(x);");
  ExpectToken(JsKeywords::kAwait, "await", "Start Oper");
  ExpectToken(JsKeywords::kOperator, "++", "Start Oper");
  // The carry itself: the window is still open after the update operator...
  ExpectSpeculativeOperatorWindow(true);
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  // ...and survives the whitespace, which is what the minifier consults.
  ExpectSpeculativeOperatorWindow(true);
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Expr");
  // The next significant token closes it.
  ExpectSpeculativeOperatorWindow(false);
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ReservedWordsAsFieldNames1) {
  // Reserved words may be used as identifiers in certain contexts, such as
  // field access.
  // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Lexical_grammar#Reserved_word_usage
  BeginTokenizing("document.true=true;");
  ExpectToken(JsKeywords::kIdentifier, "document");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectToken(JsKeywords::kIdentifier, "true");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kTrue, "true");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ReservedWordsAsFieldNames2) {
  // Reserved words may be used as identifiers in certain contexts, such as
  // object literal property names.
  // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Lexical_grammar#Reserved_word_usage
  BeginTokenizing("x={if:false,class:3};");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kIdentifier, "if", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr Oper { OVal");
  ExpectToken(JsKeywords::kFalse, "false", "Start Expr Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper {");
  ExpectToken(JsKeywords::kIdentifier, "class", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr Oper { OVal");
  ExpectToken(JsKeywords::kNumber, "3", "Start Expr Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ReservedWordsAsLabelNames) {
  // Reserved words may NOT be used as label identifiers.  We should
  // distinguish this case from the object literal case above.
  BeginTokenizing("{if:false,class:3};");
  ExpectToken(JsKeywords::kOperator, "{", "Start {");
  ExpectToken(JsKeywords::kIf, "if", "Start { BkKwd");
  ExpectError(":false,class:3};");
}

TEST_F(JsTokenizerTest, Comments) {
  BeginTokenizing(
      "x=1; // hello\n"
      "y=/* world */2;\n"
      "z<!--sgml\n"    // <!-- is a line comment, but
      "foo-->bar;\n"   // --> is a line comment only at
      " --> sgml\n");  // the start of the line.
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kNumber, "1");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kComment, "// hello");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kComment, "/* world */");
  ExpectToken(JsKeywords::kNumber, "2");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "z");
  ExpectToken(JsKeywords::kComment, "<!--sgml");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, "--");
  ExpectToken(JsKeywords::kOperator, ">");
  ExpectToken(JsKeywords::kIdentifier, "bar");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n ");

  ExpectToken(JsKeywords::kComment, "--> sgml");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LineComments) {
  BeginTokenizing(
      "1//foo\r"
      "+2//bar\xE2\x80\xA8"  // U+2028 LINE SEPARATOR"
      "+3//baz\xE2\x80\xA9"  // U+2029 PARAGRAPH SEPARATOR
      "4//quux\xE2\x81\x9F"  // U+205F MEDIUM MATHEMATICAL SPACE
      "+5//hello, world!\n"
      "6");
  ExpectToken(JsKeywords::kNumber, "1");
  ExpectToken(JsKeywords::kComment, "//foo");
  ExpectToken(JsKeywords::kLineSeparator, "\r");

  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kNumber, "2");
  ExpectToken(JsKeywords::kComment, "//bar");
  ExpectToken(JsKeywords::kLineSeparator, "\xE2\x80\xA8");

  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kNumber, "3");
  ExpectToken(JsKeywords::kComment, "//baz");
  ExpectToken(JsKeywords::kSemiInsert, "\xE2\x80\xA9");

  ExpectToken(JsKeywords::kNumber, "4");
  ExpectToken(JsKeywords::kComment, "//quux\xE2\x81\x9F+5//hello, world!");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kNumber, "6");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SgmlComments) {
  BeginTokenizing(
      "4+<!--foo\n"
      "3+\n"
      "/*foo*/ --> foo\n"
      "x --> foo\n");
  ExpectToken(JsKeywords::kNumber, "4");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kComment, "<!--foo");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kNumber, "3");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kComment, "/*foo*/");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kComment, "--> foo");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "--");
  ExpectToken(JsKeywords::kOperator, ">");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Whitespace) {
  BeginTokenizing(
      "\xEF\xBB\xBF"  // U+FEFF BYTE ORDER MARK
      "a\f=\t1\v+ 2\r"
      "5\xC2\xA0"        // U+00A0 NO-BREAK SPACE
      "\xE2\x81\x9F"     // U+205F MEDIUM MATHEMATICAL SPACE
      "+\xE1\x9A\x80"    // U+1680 OGHAM SPACE MARK
      "7\xE2\x80\xA9"    // U+2029 PARAGRAPH SEPARATOR
      ";\xE2\x80\xA8");  // U+2028 LINE SEPARATOR
  ExpectToken(JsKeywords::kWhitespace, "\xEF\xBB\xBF");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kWhitespace, "\f");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kWhitespace, "\t");
  ExpectToken(JsKeywords::kNumber, "1");
  ExpectToken(JsKeywords::kWhitespace, "\v");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2");
  ExpectToken(JsKeywords::kSemiInsert, "\r");

  ExpectToken(JsKeywords::kNumber, "5");
  ExpectToken(JsKeywords::kWhitespace, "\xC2\xA0\xE2\x81\x9F");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kWhitespace, "\xE1\x9A\x80");
  ExpectToken(JsKeywords::kNumber, "7");
  ExpectToken(JsKeywords::kLineSeparator, "\xE2\x80\xA9");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\xE2\x80\xA8");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, NumericLiterals) {
  BeginTokenizing(
      "var  foo =\n  .85 - 0191.e+3+0171.toString();\n"
      "1..property");
  ExpectToken(JsKeywords::kVar, "var");
  ExpectToken(JsKeywords::kWhitespace, "  ");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kLineSeparator, "\n  ");
  ExpectToken(JsKeywords::kNumber, ".85");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "-");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "0191.e+3");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kNumber, "0171");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectToken(JsKeywords::kIdentifier, "toString");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kNumber, "1.");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectToken(JsKeywords::kIdentifier, "property");
  ExpectEndOfInput();
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, RegexLiterals) {
  BeginTokenizing(
      "foo=/quux/;\n"
      "bar=/quux/ig;\n"
      "baz=a/quux/ig;");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kRegex, "/quux/");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "bar");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kRegex, "/quux/ig");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "baz");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "quux");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "ig");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, RegexVsSlash) {
  BeginTokenizing(
      "if(a*(b+c)/d<e)/d<e/.exec('\\'');\n"
      "else/x/.exec(\"\");");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kIf, "if");
  ExpectParseStack("Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectParseStack("Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, "*");
  ExpectParseStack("Start BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start BkKwd ( Expr Oper (");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectParseStack("Start BkKwd ( Expr Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectParseStack("Start BkKwd ( Expr Oper ( Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "c");
  ExpectParseStack("Start BkKwd ( Expr Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectParseStack("Start BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "d");
  ExpectParseStack("Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, "<");
  ExpectParseStack("Start BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "e");
  ExpectParseStack("Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start BkHdr");
  ExpectToken(JsKeywords::kRegex, "/d<e/");
  ExpectParseStack("Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectParseStack("Start BkHdr Expr .");
  ExpectToken(JsKeywords::kIdentifier, "exec");
  ExpectParseStack("Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start BkHdr Expr (");
  ExpectToken(JsKeywords::kStringLiteral, "'\\''");
  ExpectParseStack("Start BkHdr Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kElse, "else");
  ExpectParseStack("Start BkHdr");
  ExpectToken(JsKeywords::kRegex, "/x/");
  ExpectParseStack("Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectParseStack("Start BkHdr Expr .");
  ExpectToken(JsKeywords::kIdentifier, "exec");
  ExpectParseStack("Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectParseStack("Start BkHdr Expr (");
  ExpectToken(JsKeywords::kStringLiteral, "\"\"");
  ExpectParseStack("Start BkHdr Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectParseStack("Start BkHdr Expr");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectParseStack("Start");
  ExpectEndOfInput();
  ExpectParseStack("");
}

TEST_F(JsTokenizerTest, Operators1) {
  BeginTokenizing(
      "foo /= bar+++baz;\n"
      "a=b==c&&d===e;\n"
      "a>>>=b>=c?d>>_:$;");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "/=");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "bar");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "baz");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kOperator, "==");
  ExpectToken(JsKeywords::kIdentifier, "c");
  ExpectToken(JsKeywords::kOperator, "&&");
  ExpectToken(JsKeywords::kIdentifier, "d");
  ExpectToken(JsKeywords::kOperator, "===");
  ExpectToken(JsKeywords::kIdentifier, "e");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, ">>>=");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kOperator, ">=");
  ExpectToken(JsKeywords::kIdentifier, "c");
  ExpectToken(JsKeywords::kOperator, "?");
  ExpectToken(JsKeywords::kIdentifier, "d");
  ExpectToken(JsKeywords::kOperator, ">>");
  ExpectToken(JsKeywords::kIdentifier, "_");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kIdentifier, "$");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Operators2) {
  BeginTokenizing(
      "a+++b\n"
      "a+ ++b\n"
      "a+ +b\n"
      "a---b\n"
      "a-++b\n"
      "!!b\n");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "--");
  ExpectToken(JsKeywords::kOperator, "-");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "-");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kOperator, "!");
  ExpectToken(JsKeywords::kOperator, "!");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Colons1) {
  // Each of the three lines below contains the substring ":{}/x/i".  However,
  // in the first two lines that's a label colon, followed by an empty block,
  // followed by a regex literal, while in the third line it's an ternary
  // operator, followed by an empty object literal, followed by division.
  BeginTokenizing(
      "switch(x){default:{}/x/i}\n"
      "foobar:{}/x/i;\n"
      "a?b:{}/x/i");
  ExpectToken(JsKeywords::kSwitch, "switch");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kDefault, "default");
  ExpectParseStack("Start BkHdr { Other");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectParseStack("Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kRegex, "/x/i");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "foobar");
  ExpectParseStack("Start Expr");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kRegex, "/x/i");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "?");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectParseStack("Start Expr ? Expr");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectParseStack("Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "i");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Colons2) {
  // Each of the three lines below contains the substring "{foo:{}/x/i}".  In
  // the first line the outer braces are a block, so the inside is a label,
  // followed by an empty block, followed by a regex literal.  in the second
  // line the outer braces are an object literal, so the inside is an object
  // property equal to an empty object literal divided by some other values.
  // In the third line the outer braces are again a block (even though they're
  // part of an expression), so the inside is again a label/block/regex.
  BeginTokenizing(
      "{foo:{}/x/i}\n"
      "y={foo:{}/x/i}\n"
      "z=function(){foo:{}/x/i}");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kRegex, "/x/i");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "i");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kSemiInsert, "\n");

  ExpectToken(JsKeywords::kIdentifier, "z");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kFunction, "function");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kRegex, "/x/i");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectLiteralsArray) {
  BeginTokenizing("[{a:42},{a:32}]");
  ExpectToken(JsKeywords::kOperator, "[", "Start [");
  ExpectToken(JsKeywords::kOperator, "{", "Start [ {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start [ { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start [ { OVal");
  ExpectToken(JsKeywords::kNumber, "42", "Start [ { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start [ Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start [");
  ExpectToken(JsKeywords::kOperator, "{", "Start [ {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start [ { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start [ { OVal");
  ExpectToken(JsKeywords::kNumber, "32", "Start [ { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TrailingCommas) {
  BeginTokenizing(
      "x={a:1,}\n"
      "y=[,,]");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr Oper { OVal");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n", "Start");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "[", "Start Expr Oper [");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper [");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper [");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CommaOperator) {
  BeginTokenizing("x=(y++,y*5)");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper ( Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper ( Expr Oper");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectLiteralRegexLiteral) {
  // On the first line, this looks like it should be an object literal divided
  // by x divided by i, but nope, that's a block with a labelled expression
  // statement, followed by a regex literal.  The second line, on the other
  // hand, _is_ an object literal, followed by division.
  BeginTokenizing(
      "{foo:1} / x/i;\n"
      "x={foo:1} / x/i;");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kNumber, "1");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ x/i");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kIdentifier, "foo");
  ExpectToken(JsKeywords::kOperator, ":");
  ExpectToken(JsKeywords::kNumber, "1");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "i");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EmptyBlockRegexLiteral) {
  BeginTokenizing("if(true){{}/foo/}");
  ExpectToken(JsKeywords::kIf, "if", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kTrue, "true", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr { {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr {");
  ExpectToken(JsKeywords::kRegex, "/foo/", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TrickyRegexLiteral) {
  BeginTokenizing(
      "var x=a[0] / b /i;\n"
      "var y=a[0]+/ b /i;");
  ExpectToken(JsKeywords::kVar, "var");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "[");
  ExpectToken(JsKeywords::kNumber, "0");
  ExpectToken(JsKeywords::kOperator, "]");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kIdentifier, "i");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kVar, "var");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "[");
  ExpectToken(JsKeywords::kNumber, "0");
  ExpectToken(JsKeywords::kOperator, "]");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kRegex, "/ b /i");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, RegexLiteralsWithBrackets) {
  BeginTokenizing("/http:\\/\\/[^/]+\\// / /z[\\]/ ]/");
  // The / in [^/] doesn't end the regex.
  ExpectToken(JsKeywords::kRegex, "/http:\\/\\/[^/]+\\//");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "/");
  ExpectToken(JsKeywords::kWhitespace, " ");
  // The first ] is escaped and doesn't close the [, so the following / doesn't
  // close the regex.
  ExpectToken(JsKeywords::kRegex, "/z[\\]/ ]/");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ReturnRegex) {
  // Make sure we understand that this is not division; "return" is not an
  // identifier!
  BeginTokenizing(
      "return / x /g;\n"
      "return/#.+/.test(\n'#24' );");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ x /g");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kReturn, "return");
  ExpectToken(JsKeywords::kRegex, "/#.+/");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectToken(JsKeywords::kIdentifier, "test");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kStringLiteral, "'#24'");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ReturnRegex2) {
  // Make sure we understand that this is not division; "return" is not an
  // identifier!
  BeginTokenizing("return / x /g;");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ x /g");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ThrowRegex) {
  // Make sure we understand that this is not division; "throw" is not an
  // identifier!  (And yes, in JS you're allowed to throw a regex.)
  BeginTokenizing("throw / x /g;");
  ExpectToken(JsKeywords::kThrow, "throw");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/ x /g");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, UnicodeRegexFlags) {
  // The Look Of Disapproval emoticon is probably not a semantically valid
  // regex flag, but it is lexically valid, so we should be able to tokenize
  // it.
  BeginTokenizing("/\xE2\x98\x83/\xE0\xB2\xA0_\xE0\xB2\xA0\xE2\x80\xA9;");
  ExpectToken(JsKeywords::kRegex, "/\xE2\x98\x83/\xE0\xB2\xA0_\xE0\xB2\xA0");
  ExpectToken(JsKeywords::kLineSeparator, "\xE2\x80\xA9");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SemicolonInsertion1) {
  BeginTokenizing(
      "3\n"  // Semicolon is not inserted here.
      "// foo\n"
      "--> foo\n"
      "-5\n"  // Semicolon is inserted here.
      "// bar\n"
      "6");
  ExpectToken(JsKeywords::kNumber, "3");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kComment, "// foo");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kComment, "--> foo");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "-");
  ExpectToken(JsKeywords::kNumber, "5");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kComment, "// bar");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kNumber, "6");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SemicolonInsertion2) {
  BeginTokenizing(
      "w\n"    // Semicolon is inserted here...
      "++\n"   // ...but not here.
      "x\n"    // Semicolon inserted here again.
      "y++\n"  // And here again.
      "z");
  ExpectToken(JsKeywords::kIdentifier, "w", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n", "Start");
  ExpectToken(JsKeywords::kOperator, "++", "Start Oper");
  ExpectToken(JsKeywords::kLineSeparator, "\n", "Start Oper");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n", "Start");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n", "Start");
  ExpectToken(JsKeywords::kIdentifier, "z", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SemicolonInsertion3) {
  BeginTokenizing(
      "x\nin\xE1\x9A\x80y;\n"  // U+1680 OGHAM SPACE MARK
      "x\nin\\u0063\ny;\n"
      "x\ninstanceof\ny;\n"
      "x\ninstanceof\xE0\xB2\xA0_\n"  // U+0CA0 KANNADA LETTER TTHA
      "y;");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIn, "in");
  ExpectToken(JsKeywords::kWhitespace, "\xE1\x9A\x80");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "in\\u0063");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kInstanceof, "instanceof");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "instanceof\xE0\xB2\xA0_");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SemicolonInsertion4) {
  BeginTokenizing(
      "x={}\n"
      "{debugger}");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n", "Start");
  ExpectToken(JsKeywords::kOperator, "{", "Start {");
  ExpectToken(JsKeywords::kDebugger, "debugger", "Start { Jump");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, UnclosedBlockComment) {
  BeginTokenizing("debugger; /* foo");
  ExpectToken(JsKeywords::kDebugger, "debugger");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("/* foo");
}

TEST_F(JsTokenizerTest, UnclosedRegexLiteral) {
  BeginTokenizing("var bar=/quux;");
  ExpectParseStack("Start");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ", "Start MVar");
  ExpectToken(JsKeywords::kIdentifier, "bar", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectError("/quux;");
  ExpectParseStack("Start MVar Other Oper");
}

TEST_F(JsTokenizerTest, LinebreakInRegex) {
  // Regexes cannot contain linebreaks.
  BeginTokenizing("x=/foo\nquux/;");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectError("/foo\nquux/;");

  // They can contain Unicode characters, but not Unicode linebreaks.
  BeginTokenizing(
      "x=/foo\xE2\x98\x83quux/+"  // U+2603 SNOWMAN
      "/foo\xE2\x80\xA9quux/;");  // U+2029 PARAGRAPH SEPARATOR
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kRegex, "/foo\xE2\x98\x83quux/");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectError("/foo\xE2\x80\xA9quux/;");

  // Unlike in strings, newlines in regexes cannot be escaped by backslashes.
  BeginTokenizing("x=/foo\\\nquux/;");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectError("/foo\\\nquux/;");
}

TEST_F(JsTokenizerTest, LinebreakInStringLiteral) {
  // Strings can contain linebreaks only if escaped by a backslash.
  BeginTokenizing(
      "'foo\\\nquux'+"
      // A CRLF counts as one linebreak.
      "\"foo\\\r\nquux\"+"
      // Apparently, so does LFCR, for some reason.
      "\"foo\\\n\rquux\"+"
      // But two LFs are two linebreaks!
      "'foo\\\n\nquux';");
  ExpectToken(JsKeywords::kStringLiteral, "'foo\\\nquux'");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "\"foo\\\r\nquux\"");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "\"foo\\\n\rquux\"");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectError("'foo\\\n\nquux';");

  // Strings can contain Unicode characters.
  BeginTokenizing(
      "'foo\xE2\x98\x83quux'+"    // U+2603 SNOWMAN
                                  // Unicode linebreaks are allowed if
                                  // backslash-escaped.
      "'foo\\\xE2\x80\xA8quux'+"  // U+2028 LINE SEPARATOR
      // Since ES2019 (the JSON-superset change) a raw U+2028/U+2029 is
      // legal inside a string literal too.
      "'foo\xE2\x80\xA8quux';");  // U+2028 LINE SEPARATOR
  ExpectToken(JsKeywords::kStringLiteral, "'foo\xE2\x98\x83quux'");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "'foo\\\xE2\x80\xA8quux'");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "'foo\xE2\x80\xA8quux'");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EscapedQuotesInStringLiteral1) {
  BeginTokenizing("'foo\\\\\\'bar';");
  ExpectToken(JsKeywords::kStringLiteral, "'foo\\\\\\'bar'");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EscapedQuotesInStringLiteral2) {
  BeginTokenizing("\"baz\"+'foo\\\\\\'ba\"r';");
  ExpectToken(JsKeywords::kStringLiteral, "\"baz\"");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "'foo\\\\\\'ba\"r'");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EscapedQuotesInStringLiteral3) {
  BeginTokenizing("'b\\\\az'+'foo\\'bar';");
  ExpectToken(JsKeywords::kStringLiteral, "'b\\\\az'");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "'foo\\'bar'");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EscapedQuotesInStringLiteral4) {
  BeginTokenizing("'b\\'az'+'foobar';");
  ExpectToken(JsKeywords::kStringLiteral, "'b\\'az'");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kStringLiteral, "'foobar'");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EscapedQuotesInStringLiteral5) {
  BeginTokenizing("\"f\xFFoo\\\\\\\"bar\";");
  ExpectToken(JsKeywords::kStringLiteral, "\"f\xFFoo\\\\\\\"bar\"");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, UnclosedStringLiteral) {
  BeginTokenizing("bar='quux;");
  ExpectToken(JsKeywords::kIdentifier, "bar");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectError("'quux;");
}

TEST_F(JsTokenizerTest, UnmatchedCloseParen) {
  BeginTokenizing("bar='quux');");
  ExpectToken(JsKeywords::kIdentifier, "bar");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kStringLiteral, "'quux'");
  ExpectError(");");
}

TEST_F(JsTokenizerTest, BogusInputCharacter) {
  // A `#` not followed by an identifier-start is an error (private names
  // themselves tokenize now -- see ClassPrivateElements).
  BeginTokenizing("var #;");
  ExpectToken(JsKeywords::kVar, "var");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("#;");
}

TEST_F(JsTokenizerTest, BackslashesInIdentifier) {
  BeginTokenizing("a\\u03c0b");
  ExpectToken(JsKeywords::kIdentifier, "a\\u03c0b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, BackslashesInString) {
  BeginTokenizing("\"a\\\"b\"");
  ExpectToken(JsKeywords::kStringLiteral, "\"a\\\"b\"");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombinePluses) {
  BeginTokenizing("a+++b");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombinePluses2) {
  BeginTokenizing("a+ ++b");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombinePlusesSpace) {
  BeginTokenizing("a+ +b");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombineMinuses) {
  BeginTokenizing("a---b");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "--");
  ExpectToken(JsKeywords::kOperator, "-");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombineMixed) {
  BeginTokenizing("a--+b");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "--");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombineMixed2) {
  BeginTokenizing("a-++b");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "-");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, CombineBangs) {
  BeginTokenizing("!!b");
  ExpectToken(JsKeywords::kOperator, "!");
  ExpectToken(JsKeywords::kOperator, "!");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, NumbersAndDotsAndIdentifiersAndKeywords) {
  BeginTokenizing("return a.b+5.3");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kNumber, "5.3");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, NumberProperty) {
  BeginTokenizing("1..property");
  ExpectToken(JsKeywords::kNumber, "1.");
  ExpectToken(JsKeywords::kOperator, ".");
  ExpectToken(JsKeywords::kIdentifier, "property");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LineCommentAtEndOfInput) {
  BeginTokenizing("hello//world");
  ExpectToken(JsKeywords::kIdentifier, "hello");
  ExpectToken(JsKeywords::kComment, "//world");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Latin1BlockComment) {
  // Try to tokenize input that is Latin-1 encoded.  This is not valid UTF-8,
  // but we should be able to proceed gracefully (in most cases) if the
  // non-ascii characters only ever appear in string literals and comments.
  BeginTokenizing("/* qu\xE9 pasa */\n");
  ExpectToken(JsKeywords::kComment, "/* qu\xE9 pasa */");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, BlockCommentWithLinebreakInsertsSemicolon) {
  // A block comment containing a line terminator counts as a line
  // terminator for automatic semicolon insertion (ECMA-262 5.1.2/12.4), so
  // it yields a semicolon-insertion token (carrying the comment as its
  // text) wherever a real linebreak would insert one.
  BeginTokenizing("x/*\n*/y");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kSemiInsert, "/*\n*/");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, BlockCommentWithUnicodeLinebreakInsertsSemicolon) {
  // U+2028 LINE SEPARATOR inside a block comment is a line terminator too.
  BeginTokenizing("x/*\xE2\x80\xA8*/y");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kSemiInsert, "/*\xE2\x80\xA8*/");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, BlockCommentWithLinebreakBeforePostfix) {
  // `x/*\n*/++y` is `x; ++y;`: the postfix restricted production forbids a
  // line terminator (including a comment-borne one) before ++.
  BeginTokenizing("x/*\n*/++y");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kSemiInsert, "/*\n*/");
  ExpectToken(JsKeywords::kOperator, "++");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, BlockCommentWithLinebreakContinuation) {
  // No insertion when the next token can continue the statement, exactly as
  // for a real linebreak.
  BeginTokenizing("x/*\n*/(y)");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kComment, "/*\n*/");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ConditionalCommentWithLinebreakStaysComment) {
  // Conditional-compilation comments are retained verbatim by the minifier,
  // so they are never retyped; the retained text itself carries the
  // linebreak into the output.
  BeginTokenizing("x/*@cc_on\n@*/y");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kComment, "/*@cc_on\n@*/");
  ExpectToken(JsKeywords::kIdentifier, "y");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Latin1LineComment) {
  // Try to tokenize input that is Latin-1 encoded.  This is not valid UTF-8,
  // but we should be able to proceed gracefully (in most cases) if the
  // non-ascii characters only ever appear in string literals and comments.
  BeginTokenizing("// qu\xE9 pasa\n");
  ExpectToken(JsKeywords::kComment, "// qu\xE9 pasa");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Latin1StringLiteral) {
  // Try to tokenize input that is Latin-1 encoded.  This is not valid UTF-8,
  // but we should be able to proceed gracefully (in most cases) if the
  // non-ascii characters only ever appear in string literals and comments.
  BeginTokenizing("\"qu\xE9 pasa\"\n");
  ExpectToken(JsKeywords::kStringLiteral, "\"qu\xE9 pasa\"");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();

  // An example with more complicated escaping:
  BeginTokenizing("'\xAA\\'\xBB\\\r\n\xCC'\n");
  ExpectToken(JsKeywords::kStringLiteral, "'\xAA\\'\xBB\\\r\n\xCC'");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, Latin1Input) {
  // Try to tokenize input that is Latin-1 encoded.  This is not valid UTF-8,
  // but we should be able to proceed gracefully (in most cases) if the
  // non-ascii characters only ever appear in string literals and comments.
  BeginTokenizing(
      "str='Qu\xE9 pasa';// 'qu\xE9' means 'what'\n"
      "cents=/* 73\xA2 is $0.73 */73;");

  ExpectToken(JsKeywords::kIdentifier, "str");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kStringLiteral, "'Qu\xE9 pasa'");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectToken(JsKeywords::kComment, "// 'qu\xE9' means 'what'");
  ExpectToken(JsKeywords::kLineSeparator, "\n");

  ExpectToken(JsKeywords::kIdentifier, "cents");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kComment, "/* 73\xA2 is $0.73 */");
  ExpectToken(JsKeywords::kNumber, "73");
  ExpectToken(JsKeywords::kOperator, ";");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, JsonHeuristic) {
  // Sometimes we put JSON data through the JavaScript tokenizer.  Most JSON
  // will parse just fine, but JSON object literals, without parse context to
  // mark them as expressions (rather than code blocks) will generally not
  // parse as JavaScript code.  Therefore, the tokenizer has a heuristic to
  // recognize input consisting of a JSON object literal and alter the parse
  // state to handle it.
  BeginTokenizing("{\"foo\":{\"bar\":1},\"baz\":2}");
  // At first, we assume this is JS code, as usual:
  ExpectToken(JsKeywords::kOperator, "{", "Start {");
  ExpectToken(JsKeywords::kStringLiteral, "\"foo\"", "Start { Expr");
  // Once we see the colon, we know this is actually a JSON object literal
  // rather than a code block (a string literal followed by a colon isn't valid
  // syntax at start-of-statement).  Adding a synthetic Oper parse state in
  // front of the { state allows us to treat this as an object literal rather
  // than a code block.  From here we can proceed as normal.
  ExpectToken(JsKeywords::kOperator, ":", "Start Oper { Oper");
  ExpectToken(JsKeywords::kOperator, "{", "Start Oper { Oper {");
  ExpectToken(JsKeywords::kStringLiteral, "\"bar\"");
  ExpectToken(JsKeywords::kOperator, ":", "Start Oper { Oper { OVal");
  ExpectToken(JsKeywords::kNumber, "1", "Start Oper { Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Oper {");
  ExpectToken(JsKeywords::kStringLiteral, "\"baz\"");
  ExpectToken(JsKeywords::kOperator, ":", "Start Oper { OVal");
  ExpectToken(JsKeywords::kNumber, "2", "Start Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TokenizeAngular) {
  ExpectTokenizeFileSuccessfully("angular.original");
}

TEST_F(JsTokenizerTest, TokenizeJQuery) {
  ExpectTokenizeFileSuccessfully("jquery.original");
}

TEST_F(JsTokenizerTest, TokenizePrototype) {
  ExpectTokenizeFileSuccessfully("prototype.original");
}

// ---------------------------------------------------------------------------
// ES6 template literal tests (POC: teach the tokenizer template literals).
// ---------------------------------------------------------------------------

TEST_F(JsTokenizerTest, TemplateNoSubstitution) {
  BeginTokenizing("var x = `hello world`;");
  ExpectToken(JsKeywords::kVar, "var");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTemplateLiteral, "`hello world`",
              "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateSingleInterpolation) {
  BeginTokenizing("`a${x}b`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`a${", "Start ${");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}b`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateMultipleInterpolations) {
  BeginTokenizing("`${a}mid${b}`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`${", "Start ${");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}mid${", "Start ${");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateNestedTemplate) {
  // A template literal nested inside an interpolation of an outer template.
  BeginTokenizing("`outer${`inner${x}`}done`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`outer${", "Start ${");
  ExpectToken(JsKeywords::kTemplateLiteral, "`inner${", "Start ${ ${");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start ${ ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`", "Start ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}done`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateObjectLiteralInInterpolation) {
  // The brace-depth trap: ${ {a:1} } -- the first '}' closes the object
  // literal, the second '}' resumes the template.
  BeginTokenizing("`v=${ {a:1} }`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`v=${", "Start ${");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start ${ {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start ${ { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start ${ { OVal");
  ExpectToken(JsKeywords::kNumber, "1", "Start ${ { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start ${ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateEscapedBacktickAndDollar) {
  // \` is an escaped backtick (not a terminator); \${ is an escaped
  // interpolation (literal text, not an interpolation); \\ is an escaped
  // backslash.
  BeginTokenizing("`a\\`b\\${c}\\\\d`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`a\\`b\\${c}\\\\d`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateFollowedByDivision) {
  // A completed template literal is a primary expression, so a following
  // slash is division, not a regex.
  BeginTokenizing("`x`/y/g");
  ExpectToken(JsKeywords::kTemplateLiteral, "`x`", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateInterpolationSlashIsRegex) {
  // A slash at the start of an interpolation expression is a regex literal.
  BeginTokenizing("`${/y/g}`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`${", "Start ${");
  ExpectToken(JsKeywords::kRegex, "/y/g", "Start ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateMultiline) {
  // Template literals may contain raw linebreaks; these must not trigger
  // semicolon insertion or terminate the token.
  BeginTokenizing("`line1\nline2\nline3`");
  ExpectToken(JsKeywords::kTemplateLiteral, "`line1\nline2\nline3`",
              "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TaggedTemplate) {
  // tag`x` is a tagged template; the tag identifier and the template stay
  // adjacent and the whole thing is an expression.
  BeginTokenizing("tag`x${y}z`");
  ExpectToken(JsKeywords::kIdentifier, "tag", "Start Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "`x${", "Start Expr ${");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}z`", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TemplateUnterminated) {
  // An unterminated template (no closing backtick) is a conservative error.
  BeginTokenizing("`abc${x}def");
  ExpectToken(JsKeywords::kTemplateLiteral, "`abc${");
  ExpectToken(JsKeywords::kIdentifier, "x");
  ExpectError("}def");
}

// ---------------------------------------------------------------------------
// ES2017-ES2020 Tier-1 tests: nullish coalescing, optional chaining, optional
// catch binding, destructuring declarations, super/import/export, and pins
// for constructs that already passed through as split tokens.
// ---------------------------------------------------------------------------

TEST_F(JsTokenizerTest, NullishCoalescingOperator) {
  BeginTokenizing("a??b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "??", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, NullishAssignmentOperator) {
  BeginTokenizing("x??=y;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "??=", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, NullishFollowedByRegex) {
  // `??` is a binary operator, so a slash after it starts a regex literal.
  BeginTokenizing("a??/re/.test(b);");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "??", "Start Expr Oper");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Expr .");
  ExpectToken(JsKeywords::kIdentifier, "test", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainProperty) {
  BeginTokenizing("a?.b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainReservedWord) {
  // Like kPeriod, a reserved word after `?.` is an identifier.
  BeginTokenizing("a?.class;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kIdentifier, "class", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainWithSpace) {
  // `a ?. b` is legal: `?.` is a single token, whitespace around it is fine.
  BeginTokenizing("a ?. b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainIndex) {
  // Unlike kPeriod, `?.` may be followed by an open bracket: a?.[i].
  BeginTokenizing("a?.[i];");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kOperator, "[", "Start Expr ?. [");
  ExpectToken(JsKeywords::kIdentifier, "i", "Start Expr ?. [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainCallThenDivision) {
  // Unlike kPeriod, `?.` may be followed by an open paren: a?.(x).  The call
  // collapses to an expression, so a slash after it is division.
  BeginTokenizing("a?.(x)/2;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr ?. (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr ?. ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainDigitIsTernary) {
  // ECMA-262: OptionalChainingPunctuator is `?.` [lookahead not in
  // DecimalDigit], so `a?.5:b` is the ternary `a ? .5 : b`.
  BeginTokenizing("a?.5:b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?", "Start Expr ?");
  ExpectToken(JsKeywords::kNumber, ".5", "Start Expr ? Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainInCaseLabelNoStrandedQuestionMark) {
  // Before `?.` had its own parse state, `a?.b` stranded a kQuestionMark on
  // the stack, which a later colon would mis-bind as a ternary.  Now the
  // case label's colon walks straight back to the switch block.
  BeginTokenizing("switch(x){case a?.b:y}");
  ExpectToken(JsKeywords::kSwitch, "switch", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kCase, "case", "Start BkHdr { Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start BkHdr { Expr ?.");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start BkHdr {");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalChainSlashIsError) {
  // A slash directly after `?.` can be neither division nor a regex.
  BeginTokenizing("a?./re/");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectError("/re/");
}

TEST_F(JsTokenizerTest, OptionalChainNoSemicolonInsertion) {
  // A linebreak after `a?.b` does not insert a semicolon when the next token
  // continues the expression, and a linebreak directly after `?.` never
  // inserts one (it is an operator).
  BeginTokenizing("a?.b\n(c);");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "c", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("a?.\nb;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalCatchBinding) {
  // ES2019: `catch` with no parenthesized parameter, followed directly by
  // its block.
  BeginTokenizing("try{}catch{}finally{}");
  ExpectToken(JsKeywords::kTry, "try", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kCatch, "catch", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kFinally, "finally", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, OptionalCatchBindingThenRegex) {
  // The braces of `catch {}` are a block, so a slash after the closing brace
  // starts a new statement: it is a regex literal, not division.
  BeginTokenizing("try{}catch{}/re/.test(x);");
  ExpectToken(JsKeywords::kTry, "try", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kCatch, "catch", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Expr .");
  ExpectToken(JsKeywords::kIdentifier, "test", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, DestructuringConstObject) {
  BeginTokenizing("const {a,b}=x;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar {");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start MVar { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, DestructuringConstArray) {
  BeginTokenizing("const [a]=x;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "[", "Start MVar [");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, DestructuringRenameAndDefault) {
  BeginTokenizing("const {a:b=1}=x;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar { OVal");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start MVar { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar { OVal Expr Oper");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, DestructuringLet) {
  // `let` at statement position is a declaration keyword, like const/var.
  BeginTokenizing("let {a}=b;");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetDeclarationSimple) {
  BeginTokenizing("let x = 1;");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetLinebreakBeforeBinding) {
  // The binding lookahead skips whitespace including linebreaks: `let\nx=1`
  // is a declaration (a linebreak after a declaration `let` never inserts a
  // semicolon), and minifies to the same bytes as the old identifier-path
  // rendering.
  BeginTokenizing("let\nx=1;");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetStatementPositionNonBinding) {
  // When the next non-whitespace character cannot begin a binding, a
  // statement-position `let` stays an identifier -- the pre-ES2015 behavior,
  // byte-identical.  `let = 5;`, `let / 2;`, and `let(x);` are all valid
  // sloppy-mode uses of `let` as a variable.
  BeginTokenizing("let=5;");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("let/2;");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("let(x);");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetPostfixIncrementThenDivision) {
  // Sloppy-mode `let++;` uses `let` as a variable.  The binding lookahead
  // sees `+` and takes the identifier path, so the postfix `++` leaves an
  // expression on the stack and the following slash is DIVISION -- the
  // pre-diff classification.
  BeginTokenizing("let++ /2;");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetInExpression) {
  // `let in x` -- the lookahead sees an identifier-start character (`in`),
  // so `let` is classified as a declaration keyword; the token bytes and the
  // regex/division decisions are identical to the old identifier-path
  // tokenization either way.
  BeginTokenizing("let in x;");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIn, "in", "Start MVar Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetAsIdentifier) {
  // Away from statement position, `let` remains a legal identifier in
  // non-strict code.
  BeginTokenizing("var let=1;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetDeclarationAfterCaseLabel) {
  // After a case label's colon we are back at statement position inside the
  // switch block, so `let` starts a declaration there.
  BeginTokenizing("switch(x){case 1:let {a}=b}");
  ExpectToken(JsKeywords::kSwitch, "switch", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kCase, "case", "Start BkHdr { Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start BkHdr {");
  ExpectToken(JsKeywords::kLet, "let", "Start BkHdr { MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr { MVar {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr { MVar { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr { MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr { MVar Other Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start BkHdr { MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ForConstDestructuringOf) {
  // The for-of "of" is treated as a (speculative) binary operator so that a
  // following slash starts a regex, so "of" pushes a kOperator (see
  // ForOfRegex).
  BeginTokenizing("for(const {a} of xs){}");
  ExpectToken(JsKeywords::kFor, "for", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kConst, "const", "Start BkKwd ( MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkKwd ( MVar {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkKwd ( MVar { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkKwd ( MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "of", "Start BkKwd ( MVar Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "xs", "Start BkKwd ( MVar Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ForLetDestructuringResidual) {
  // Documented residual: the `let` statement-position set excludes the
  // for-header kOpenParen, so `let` there stays an identifier and the
  // binding pattern's brace errors out (byte-preserving).  Use
  // `for (const {a} of xs)` instead; do not widen the statement-position set
  // without revisiting the sloppy-mode traces.
  BeginTokenizing("for(let {a} of xs){}");
  ExpectToken(JsKeywords::kFor, "for", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("{a} of xs){}");
}

TEST_F(JsTokenizerTest, ExportLetDestructuringResidual) {
  // Documented residual: over the module-declaration marker `let` is a
  // declaration only when an identifier-start character follows; before `{`
  // it stays an identifier, so the binding pattern's brace errors out
  // (byte-preserving).  `export const {a} = b` works.
  BeginTokenizing("export let {a} = b;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "let", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("{a} = b;");
}

TEST_F(JsTokenizerTest, NestedDestructuring) {
  BeginTokenizing("const {a:{b}}=x;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar { OVal");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar { OVal {");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start MVar { OVal { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectMethodShorthand) {
  // An identifier (or modifier chain) directly before `(` in an object
  // literal is a method: the parens are a parameter list and the `{...}`
  // after them is a block body, exactly like a function's.  After the body
  // the literal continues at property position.
  BeginTokenizing("var o = { m() {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectGetterSetterMethod) {
  // `get`/`set` are ordinary identifiers that collapse onto the property
  // name; the method gate fires on the paren after the chain.
  BeginTokenizing("var o = { get v() {}, set v(x) {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "v", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "set", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "v", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x",
              "Start MVar Other Oper { Expr BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectAsyncAndStarMethod) {
  // `async` collapses like `get`/`set` and reaches the method gate that way;
  // a leading `*` before a plain name is instead a generator marker that
  // pushes a block keyword, exactly like the `*` of `function*`, so the name
  // and parameter list complete into a block header (see
  // GeneratorMethodStarIsABlockKeyword).  The token stream is the same
  // either way; the block-keyword route installs the same name expression
  // under its block keyword, so both routes close the body back to the
  // literal's property position.
  BeginTokenizing("var o = { async m() {}, *n() {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "*",
              "Start MVar Other Oper { Expr BkKwd");
  ExpectToken(JsKeywords::kIdentifier, "n",
              "Start MVar Other Oper { Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectComputedMethod) {
  // A computed name is an expression too: `{ ['k']() {} }` and
  // `{ get ['k']() {} }` take the same method gate.
  BeginTokenizing("var o = { get ['k']() {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "[", "Start MVar Other Oper { Expr [");
  ExpectToken(JsKeywords::kStringLiteral, "'k'",
              "Start MVar Other Oper { Expr [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectModifierWordsAsPlainProperties) {
  // `get`/`set`/`async` stay plain properties before `:` and as shorthand
  // properties -- the method gate only fires on `(`.
  BeginTokenizing("var o = { get: 1, async: 2 };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar Other Oper { OVal");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar Other Oper { OVal");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("var get = 1; var o = {get};");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectKeywordNamedMethod) {
  // Reserved words are valid method names: the property-name branch
  // consumes them as identifiers, then the same paren gate fires.
  BeginTokenizing("var o = { if() {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "if", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, MethodShorthandStatementPositionIsBlock) {
  // `{ m() {} }` at statement position is a block, not an object literal:
  // `m` is no property name, so the `(` is a call and the `{` after it
  // errors out byte-preservingly (engines report a SyntaxError here too).
  BeginTokenizing("{ m() {} }");
  ExpectToken(JsKeywords::kOperator, "{", "Start {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start { Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("{} }");

  // With a linebreak, ASI splits the block into a call and an empty block.
  BeginTokenizing("{ m()\n{} }");
  ExpectToken(JsKeywords::kOperator, "{", "Start {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start { Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start { Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "{", "Start { {");
  ExpectToken(JsKeywords::kOperator, "}", "Start {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectMethodBodyBoundary) {
  // After the outer literal closes, ordinary operand rules apply: a slash
  // divides, an identifier cannot continue (ASI).
  BeginTokenizing("x = { m() {} }\nfoo();");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "foo", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = { m() {} }\n/re/g;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ObjectCallAfterGroupValueIsNotAMethod) {
  // A parenthesized value or a function literal also collapses to an
  // expression over the literal's brace; a `(` after it is a CALL of that
  // value, never a method (`{ a: function(){}() }`, `{ a: (function(){})()
  // }`).  The gate stays closed because the previous token ended a group.
  BeginTokenizing("var o = { a: function() {}(), b: (function(){})() };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar Other Oper { OVal");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function",
              "Start MVar Other Oper { OVal BkKwd");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { OVal BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { OVal BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { OVal BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { OVal Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar Other Oper { OVal");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start MVar Other Oper { OVal (");
  ExpectToken(JsKeywords::kFunction, "function",
              "Start MVar Other Oper { OVal ( BkKwd");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { OVal ( BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")",
              "Start MVar Other Oper { OVal ( BkHdr");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { OVal ( BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}",
              "Start MVar Other Oper { OVal ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { OVal Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpreadInCall) {
  // ES2015 spread is a single operator token, not three periods.
  BeginTokenizing("h(...[]);");
  ExpectToken(JsKeywords::kIdentifier, "h", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, "...", "Start Expr ( Oper");
  ExpectToken(JsKeywords::kOperator, "[", "Start Expr ( Oper [");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("h(1, ...xs);");
  ExpectToken(JsKeywords::kIdentifier, "h", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "...", "Start Expr ( Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "xs", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SpreadInArrayAndObject) {
  BeginTokenizing("var a = [...[]];");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "[", "Start MVar Other Oper [");
  ExpectToken(JsKeywords::kOperator, "...", "Start MVar Other Oper [ Oper");
  ExpectToken(JsKeywords::kOperator, "[", "Start MVar Other Oper [ Oper [");
  ExpectToken(JsKeywords::kOperator, "]", "Start MVar Other Oper [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Object spread, followed by another property: the spread element is an
  // expression over the brace, so the comma pops back to property
  // position.
  BeginTokenizing("var o = {...b, a: 1};");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kOperator, "...", "Start MVar Other Oper { Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar Other Oper { OVal");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, RestInDeclarations) {
  // The same `...` token is rest in destructuring and parameter lists.
  BeginTokenizing("const [a, ...rest] = x;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "[", "Start MVar [");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar [ Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar [");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "...", "Start MVar [ Oper");
  ExpectToken(JsKeywords::kIdentifier, "rest", "Start MVar [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("function f(...args) {}");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, "...", "Start BkKwd ( Oper");
  ExpectToken(JsKeywords::kIdentifier, "args", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, GeneratorDeclaration) {
  // The `*` of `function*` is a generator marker: it leaves the block
  // keyword on the stack, so the name and parameter list complete into a
  // block header exactly like a plain function's.
  BeginTokenizing("function* n() {}");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "*", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "n", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, GeneratorExpression) {
  BeginTokenizing("var g = function*() {};");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start MVar Other Oper BkKwd");
  ExpectToken(JsKeywords::kOperator, "*", "Start MVar Other Oper BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start MVar Other Oper BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, AsyncGeneratorDeclaration) {
  // `async` interposes an identifier; the generator marker then works the
  // same way.
  BeginTokenizing("async function* n() {}");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "n", "Start Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, GeneratorMethod) {
  // `{ *n() {} }`: the `*` before a plain name is a generator marker that
  // pushes a block keyword (like `function*`), so the name and parameter list
  // complete into a block header and the body closes back to the literal's
  // property position -- the marker installs the same name expression a plain
  // method leaves, so a following comma is the member separator.
  BeginTokenizing("var o = { *n() {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "*",
              "Start MVar Other Oper { Expr BkKwd");
  ExpectToken(JsKeywords::kIdentifier, "n",
              "Start MVar Other Oper { Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, YieldRegexAndLinebreak) {
  // Inside a generator, `yield` is a keyword: a slash after it starts a
  // regex literal (`yield /re/g` yields the regex -- reading it as
  // division would turn the flagged regex into a bare `g` identifier
  // after re-parsing, a ReferenceError).  A linebreak after `yield`
  // always inserts: engines continue `yield\nfoo()` as one expression,
  // and the preserved newline re-parses identically, while `yield\n*x`
  // stays the SyntaxError engines report (a LineTerminator before the
  // delegated `*` is not allowed).
  BeginTokenizing("function* g() { yield /re/g; }");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "*", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kYield, "yield", "Start BkHdr { RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start BkHdr { RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  BeginTokenizing("function* g() { yield\nfoo(); }");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "*", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kYield, "yield", "Start BkHdr { RetTh");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "foo", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr { Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, YieldAsIdentifierControls) {
  // `yield` stays an identifier where engines treat it as one (non-strict
  // code outside generators): a function name, a binding, a label.
  BeginTokenizing("function yield() {}");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "yield", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  BeginTokenizing("var yield = 1;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kYield, "yield", "Start MVar RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar RetTh Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("yield: 1;");
  ExpectToken(JsKeywords::kYield, "yield", "Start RetTh");
  ExpectToken(JsKeywords::kOperator, ":", "Start");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, RawLineSeparatorInString) {
  // Since ES2019, raw U+2028/U+2029 are legal inside string literals (the
  // JSON-superset change): the whole literal is one string token, and the
  // line separator inside it must NOT trigger linebreak logic.  (Raw \n
  // or \r in a string stays an error.)
  BeginTokenizing(
      "var s = 'a\xE2\x80\xA8"
      "b';");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "s", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral,
              "'a\xE2\x80\xA8"
              "b'",
              "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing(
      "var s = 'a\xE2\x80\xA9"
      "b';");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "s", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral,
              "'a\xE2\x80\xA9"
              "b'",
              "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Same in double quotes.
  BeginTokenizing(
      "var s = \"a\xE2\x80\xA8"
      "b\";");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "s", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral,
              "\"a\xE2\x80\xA8"
              "b\"",
              "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // A raw newline in a string is still an error (byte-preserving).
  BeginTokenizing("var s = 'a\nb';");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "s", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("'a\nb';");
}

TEST_F(JsTokenizerTest, Shebang) {
  // A `#!` first line (hashbang, for node-executable scripts) is consumed
  // as a comment including its terminating linebreak, so the minifier can
  // retain it verbatim.
  BeginTokenizing("#!/usr/bin/env node\nvar x = 1;");
  ExpectToken(JsKeywords::kComment, "#!/usr/bin/env node\n", "Start");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // A shebang at EOF without a linebreak is consumed whole.
  BeginTokenizing("#!/usr/bin/env node");
  ExpectToken(JsKeywords::kComment, "#!/usr/bin/env node", "Start");
  ExpectEndOfInput();

  // `#` anywhere else stays a byte-preserving error.
  BeginTokenizing("x = 1; #!y");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("#!y");
}

TEST_F(JsTokenizerTest, ArrowBodyReturnParenTernary) {
  // An arrow BLOCK body is a block, not an object literal: `return` is a
  // keyword there (not a property name), and a `(` after it is a plain
  // parenthesized expression -- so a ternary parses.
  BeginTokenizing("x = () => { return (a) ? b : c; };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kReturn, "return", "Start Expr => { RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { RetTh (");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr => { RetTh ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { RetTh Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "?", "Start Expr => { RetTh Expr ?");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b",
              "Start Expr => { RetTh Expr ? Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr => { RetTh Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "c", "Start Expr => { RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowBodyForOf) {
  // Block keywords inside an arrow body take their keyword paths: the
  // parenthesized header completes into a block header.  As in
  // ForConstDestructuringOf, the for-of "of" is a speculatively-classified
  // binary operator, so it pushes a kOperator.
  BeginTokenizing("x = () => { for (x of y) {} };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFor, "for", "Start Expr => { BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "of",
              "Start Expr => { BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowBodyTryCatch) {
  BeginTokenizing("x = () => { try { f(); } catch (e) {} };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTry, "try", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Expr => { BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkHdr { Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kCatch, "catch", "Start Expr => { BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "e", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowBodyWhileIfSwitchDo) {
  BeginTokenizing("x = () => { while (x) {} };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kWhile, "while", "Start Expr => { BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = () => { if (x) {} else {} };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIf, "if", "Start Expr => { BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kElse, "else", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = () => { switch (x) { case 1: break; } };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kSwitch, "switch", "Start Expr => { BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kCase, "case", "Start Expr => { BkHdr { Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr => { BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kBreak, "break", "Start Expr => { BkHdr { Jump");
  ExpectToken(JsKeywords::kOperator, ";", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = () => { do {} while (x); };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDo, "do", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => { BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kWhile, "while", "Start Expr => { BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => { BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr => { BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => { BkHdr");
  ExpectToken(JsKeywords::kOperator, ";", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowBodyLabel) {
  // `x => { m: 1 }` is a block with a label, not an object literal with a
  // property (engines agree); the paren'd form `x => ({ m: 1 })` is the
  // object.
  BeginTokenizing("x = () => { m: 1 };");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start Expr => { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr => {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr => { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, YieldSloppyCommaAndTernary) {
  // Outside generators, `yield` is an ordinary identifier: a comma or a
  // ternary after it is valid sloppy JS.  (Inside a generator, `yield, 2`
  // is a comma expression over a bare yield and tokenizes the same way;
  // `yield ? 1 : 2` is a SyntaxError in engines but only tolerated
  // byte-preservingly here.)
  BeginTokenizing("f(yield, 2);");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kYield, "yield", "Start Expr ( RetTh");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("var a = [yield, 1];");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "[", "Start MVar Other Oper [");
  ExpectToken(JsKeywords::kYield, "yield", "Start MVar Other Oper [ RetTh");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper [");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start MVar Other Oper [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("var yield, x;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kYield, "yield", "Start MVar RetTh");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = yield ? 1 : 2;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kYield, "yield", "Start Expr Oper RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "?", "Start Expr ?");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr ? Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, ":", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // In a generator, `yield, 2` is a comma expression over a bare yield.
  BeginTokenizing("function* g() { yield, 2; }");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "*", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kYield, "yield", "Start BkHdr { RetTh");
  ExpectToken(JsKeywords::kOperator, ",", "Start BkHdr { Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassDeclaration) {
  // `class X {}`: the keyword starts a header (name ignored, heritage as
  // an operator expression), the `{` completes it into a class body brace,
  // and the closing `}` rolls back to statement base.
  BeginTokenizing("class X {}");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  BeginTokenizing("class X extends Y {}");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kExtends, "extends", "Start Cls Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "Y", "Start Cls Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  // Heritage is an expression (member access, calls, parens).
  BeginTokenizing("class X extends f().y {}");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kExtends, "extends", "Start Cls Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Cls Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Cls Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Cls Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Cls Expr .");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Cls Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassExpressions) {
  // A class expression collapses to an expression when its body closes.
  BeginTokenizing("x = class {};");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start Expr Oper Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = class Named {};");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start Expr Oper Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "Named", "Start Expr Oper Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // `export default class {}` is declaration-complete at its closing
  // brace too (the `default` kOtherKeyword peels to the module marker,
  // like `export default function(){}`).
  BeginTokenizing("export default class {}");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start Mod Other Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectEndOfInput();

  // `export class X {}` is declaration-complete at its closing brace
  // (from-clause shape, like `export function f(){}`).
  BeginTokenizing("export class X {}");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start Mod Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Mod Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassMethodShorthand) {
  // Methods inside a class body take the same parameter-list gate as
  // object-literal methods, with the class brace playing the literal's
  // role.
  BeginTokenizing("class X { m() {} }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  // Getters/setters and static methods: modifier words collapse onto the
  // name before the gate fires.
  BeginTokenizing("class X { get v() {} set v(x) {} static s() {} }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "v", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "set", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "v", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "x",
              "Start BkHdr Cls{ Expr BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "static", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "s", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassFieldsAndStaticBlock) {
  // Field declarations: `= expr` until the element end (`;` or the next
  // element), plus the static-block form.
  BeginTokenizing("class X { a = 1; b; static c = 2; }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "static", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "c", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  // `static { ... }`: the brace after the modifier name opens a block of
  // ordinary statements.
  BeginTokenizing("class X { static { y(); } }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "static", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start BkHdr Cls{ BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ BkHdr { Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ BkHdr { Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr Cls{ BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassComputedGeneratorAsyncPrivate) {
  // Computed names, generator/async methods, and private elements all
  // reach the same method gate.
  BeginTokenizing("class X { ['k']() {} *g() {} async a() {} #p() {} }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "[", "Start BkHdr Cls{ [");
  ExpectToken(JsKeywords::kStringLiteral, "'k'", "Start BkHdr Cls{ [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  // The `*` of a named generator method pushes a block keyword; the
  // computed name above it keeps the ordinary operator path.
  ExpectToken(JsKeywords::kOperator, "*", "Start BkHdr Cls{ Expr BkKwd");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start BkHdr Cls{ Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "#p", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassFieldAsi) {
  // After a field initializer the ORDINARY expression continuation rules
  // apply (node-verified): a name (or `static`, `#`, `get`) starts a new
  // element, so ASI fires and the linebreak survives...
  BeginTokenizing("class X { a = 1\nb = 2 }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  // ...while `(`, `/`, `+` genuinely continue the initializer (call,
  // division, binary operator), so no insertion happens there.
  BeginTokenizing("class X { a = 1\n(2) }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Other Expr (");
  ExpectToken(JsKeywords::kNumber, "2", "Start BkHdr Cls{ Other Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  BeginTokenizing("class X { a = 1\n/re/ }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "/", "Start BkHdr Cls{ Other Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start BkHdr Cls{ Other Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassPrivateElements) {
  // `#name` tokenizes as a single identifier: private field, and `#x in`
  // inside a method body.
  BeginTokenizing("class X { #x = 1; check(o) { return #x in o; } }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "#x", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkHdr Cls{ Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkHdr Cls{ Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "check", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "o",
              "Start BkHdr Cls{ Expr BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kReturn, "return",
              "Start BkHdr Cls{ Expr BkHdr { RetTh");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "#x",
              "Start BkHdr Cls{ Expr BkHdr { RetTh Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIn, "in",
              "Start BkHdr Cls{ Expr BkHdr { RetTh Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o",
              "Start BkHdr Cls{ Expr BkHdr { RetTh Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassBodyCloseBoundary) {
  // After a class DECLARATION the following line is at statement position:
  // a slash starts a regex literal there, with or without the linebreak
  // (node-verified), so none is needed.  After a class EXPRESSION the same
  // slash is division (node-verified ReferenceError).
  BeginTokenizing("class X {}\n/re/g;");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = class {}\n/re/g;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start Expr Oper Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr Oper BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportRenameKeywordComma) {
  // A keyword rename target in an export clause (`as default`, `as if`)
  // is complete at the following comma: the clause continues.  (Plain
  // renames worked all along -- control.)
  BeginTokenizing("export { a as default, b };");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod { Expr Other");
  ExpectToken(JsKeywords::kOperator, ",", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("export { a as if, b };");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIf, "if", "Start Mod { Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, ",", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Control: a plain rename already took the clause comma path.
  BeginTokenizing("export { a as b, c };");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "c", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Import side: engines reject reserved-word bindings, but the
  // tokenizer accepts byte-preservingly (the same comma path).
  BeginTokenizing("import { a as default, b } from 'x';");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod { Expr Other");
  ExpectToken(JsKeywords::kOperator, ",", "Start Mod {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Mod { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultFunctionAsi) {
  // `export default function(){}` is a HoistableDeclaration: it is
  // grammatically complete at its closing brace, so a linebreak before
  // ANY following token inserts a semicolon -- and a slash starts a
  // regex statement, with or without the linebreak (node-verified).
  BeginTokenizing("export default function(){}\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("export default async function(){}\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Same for `export default class {}` (a ClassDeclaration).
  BeginTokenizing("export default class {}\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start Mod Other Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowCommaEndsBody) {
  // A comma directly over a completed arrow expression body ends the
  // arrow (the body is an AssignmentExpression; node-verified): a
  // property value arrow, then a setter METHOD after the comma.
  BeginTokenizing("var o = { get: () => 1, set(v) {} };");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "o", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "get", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, ":", "Start MVar Other Oper { OVal");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start MVar Other Oper { OVal (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { OVal Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=",
              "Start MVar Other Oper { OVal Expr =>");
  ExpectToken(JsKeywords::kOperator, ">",
              "Start MVar Other Oper { OVal Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1",
              "Start MVar Other Oper { OVal Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar Other Oper {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "set", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper { Expr BkKwd (");
  ExpectToken(JsKeywords::kIdentifier, "v",
              "Start MVar Other Oper { Expr BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Oper { Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper { Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Two call arguments after an arrow, not a continued body.
  BeginTokenizing("foo(() => 1, 2);");
  ExpectToken(JsKeywords::kIdentifier, "foo", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr ( (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr ( Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr ( Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr ( Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // A sequence expression at statement level, and nested arrows (both
  // arrow heads end at the comma).
  BeginTokenizing("x = () => 1, 2;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("x = a => b => 1, 2;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr => Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr => Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr => Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr => Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, DestructuringDefaultCall) {
  // A destructuring default is an AssignmentExpression: the `=` installs a
  // kOtherKeyword over the pattern brace, so a call in the default is a
  // call, not a method (the three.js constructor shape).
  BeginTokenizing("const { canvas = f() } = x;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start MVar {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "canvas", "Start MVar { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar { Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start MVar { Other Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start MVar { Other Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar { Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Same in a parameter pattern, with a following element.
  BeginTokenizing("function f({ a = g(), b = 1 } = {}) {}");
  ExpectToken(JsKeywords::kFunction, "function", "Start BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd (");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkKwd ( {");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start BkKwd ( { Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkKwd ( { Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start BkKwd ( { Other Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkKwd ( { Other Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkKwd ( { Other Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start BkKwd ( { Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start BkKwd ( { Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkKwd ( { Other Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start BkKwd ( { Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start BkKwd ( Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkKwd ( Expr Oper {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkKwd ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassHeritageLinebreakBeforeBody) {
  // A linebreak between the heritage expression and the class body is
  // insignificant (node-verified for empty and method bodies): the `{` is
  // the class body, not a statement block, so no semicolon inserts.
  BeginTokenizing("class X extends Y\n{ m() {} }");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kExtends, "extends", "Start Cls Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "Y", "Start Cls Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();

  BeginTokenizing("class X extends Y\n{}");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "X", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kExtends, "extends", "Start Cls Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "Y", "Start Cls Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectToken(JsKeywords::kOperator, "}", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ClassBodyInsideTemplateInterpolation) {
  // Inside `${...}`, a class body `}` closes the class; it is NOT template
  // text.  A template after the class is a tagged template on the class
  // expression (node-verified: parses; only the runtime tag call fails,
  // exactly as in the input).
  BeginTokenizing("const s = `x${ class A { m() {} } `y${z}` }`;");
  ExpectToken(JsKeywords::kConst, "const", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "s", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTemplateLiteral, "`x${", "Start MVar Other Oper ${");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kClass, "class", "Start MVar Other Oper ${ Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "A", "Start MVar Other Oper ${ Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper ${ BkHdr Cls{");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "m",
              "Start MVar Other Oper ${ BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kOperator, "(",
              "Start MVar Other Oper ${ BkHdr Cls{ Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")",
              "Start MVar Other Oper ${ BkHdr Cls{ Expr BkHdr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{",
              "Start MVar Other Oper ${ BkHdr Cls{ Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}",
              "Start MVar Other Oper ${ BkHdr Cls{ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "}", "Start MVar Other Oper ${ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTemplateLiteral, "`y${",
              "Start MVar Other Oper ${ Expr ${");
  ExpectToken(JsKeywords::kIdentifier, "z",
              "Start MVar Other Oper ${ Expr ${ Expr");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`",
              "Start MVar Other Oper ${ Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SuperCallAndDivision) {
  // `super` is a primary-expression head, so a slash after `super(...)` (or
  // after `super` itself) is division.
  BeginTokenizing("super(a)/2;");
  ExpectToken(JsKeywords::kSuper, "super", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Expr Oper");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, SuperProperty) {
  BeginTokenizing("super.x;");
  ExpectToken(JsKeywords::kSuper, "super", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Expr .");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, DynamicImport) {
  // import(...) collapses to an expression, so `.then` chains (and division)
  // work after it.  The `(` converts the module-declaration marker back to
  // a plain kOperator (see ConsumeOpenParen).
  BeginTokenizing("import('x').then(f);");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kOperator, "(", "Start Oper (");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Oper ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Expr .");
  ExpectToken(JsKeywords::kIdentifier, "then", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportMeta) {
  BeginTokenizing("import.meta.url;");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kOperator, ".", "Start Oper .");
  ExpectToken(JsKeywords::kIdentifier, "meta", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ".", "Start Expr .");
  ExpectToken(JsKeywords::kIdentifier, "url", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportStatementBraces) {
  BeginTokenizing("import {a} from 'x';");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportNoSemicolonInsertion) {
  // `import` never has a semicolon inserted after it, so `import\n{a}`
  // continues the statement.
  BeginTokenizing("import\n{a} from'x'");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportConst) {
  BeginTokenizing("export const x=1;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kConst, "const", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod MVar Other Oper");
  ExpectToken(JsKeywords::kNumber, "1", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportBraces) {
  BeginTokenizing("export {a,b};");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Mod {");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultFunctionStatement) {
  BeginTokenizing("export default function(){};");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultFunctionSlashSameLine) {
  // A slash directly after the function body (no linebreak) reads as
  // division over the from-clause expression -- byte-preserving, and the
  // re-parse is identical because engines read the slash as a regex at
  // statement position there (node-verified).  The linebreak form is the
  // ASI case (see ExportDefaultFunctionAsi); a same-line slash before an
  // operator-character regex (`/a*/`) still mis-scans into a
  // byte-preserving error, like the yield family.
  BeginTokenizing("export default function(){}/re/");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod From Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod From Expr Oper");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultAsyncFunctionSlashSameLine) {
  // Same byte-preserving division read for the `async` form (its
  // identifier interposes a kExpression over the `default` kOtherKeyword,
  // which the declaration-close peel now pops).
  BeginTokenizing("export default async function(){}/re/;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod From Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod From Expr Oper");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportAsiAfterBareSpecifier) {
  // A static import declaration grammatically ends at its module specifier:
  // the linebreak after `import 'x'` must insert a semicolon even though
  // `(main)` could continue an ordinary expression statement.  Before the
  // module-declaration state existed this linebreak was misclassified as a
  // mere line separator, so the minifier fused the statements into a
  // SyntaxError.
  BeginTokenizing("import 'x'\n(main)()");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kIdentifier, "main", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportAsiAfterFromSpecifier) {
  // Same insertion after the module specifier of a from-import: a regex
  // literal on the next line cannot continue the declaration.
  BeginTokenizing("import a from 'x'\n/re/g;");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportAsiAfterClause) {
  // An export declaration without a from-clause ends at the clause's `}`.
  BeginTokenizing("export {a}\n(re);");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportLetAsiAfterBinding) {
  // `export let x` grammatically ends at the binding: a regex literal on the
  // next line cannot continue it (only `=` or `,` could), so ASI fires.
  BeginTokenizing("export let x\n/re/;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kLet, "let", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportVarAsiAfterBinding) {
  // Same bare-binding insertion for `export var x` (a `const` declaration
  // always has an initializer, so it has no bare-binding form).
  BeginTokenizing("export var x\n/re/;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kVar, "var", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportVarListAsiAfterBinding) {
  // After a declarator comma the next binding is again a bare binding of the
  // declaration, so ASI fires after it.
  BeginTokenizing("export var x = 5, y\n/re/;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kVar, "var", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportFromContinuations) {
  // Declaration continuations still suppress ASI: `from` continues the
  // declaration after a default binding, and a linebreak after `from`
  // (before the module specifier) never inserts a semicolon.
  BeginTokenizing("import a\nfrom 'x';");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("import a from\n'x';");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportStarAsFromContinuations) {
  // `export * as ns` continues with `from`; a linebreak after the namespace
  // binding does not insert a semicolon when `from` follows.
  BeginTokenizing("export * as ns\nfrom 'x';");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "*", "Start Mod Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod Oper .");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "ns", "Start Mod Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportStarAsBinding) {
  BeginTokenizing("import * as ns from 'x';");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "*", "Start Mod Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod Oper .");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "ns", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportLetInitializerContinuation) {
  // `=` after the binding continues the declaration; once the initializer
  // expression is open, ordinary expression continuation rules apply, so a
  // slash after `5` is division, not the start of a regex statement.
  BeginTokenizing("export let x\n= 5;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kLet, "let", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("export let x = 5\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kLet, "let", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod MVar Other Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod MVar Other Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultExpressionContinuation) {
  // An export-default expression keeps ordinary expression continuation
  // rules: the slash genuinely continues the initializer as division.
  BeginTokenizing("export default 5\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod Other Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod Other Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultFunctionAsiAfterBody) {
  // A function-bodied default export is grammatically complete at its
  // closing brace: a linebreak before any following token inserts a
  // semicolon (the from-clause shape, like `export function`).
  BeginTokenizing("export default function(){}\nfoo();");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "foo", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ModulePositiveControl) {
  // A realistic module: two imports, an export const, and a call statement.
  BeginTokenizing(
      "import a from 'm1';\n"
      "import {b} from 'm2';\n"
      "export const z = 1;\n"
      "log(z);");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'m1'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod {");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Mod { Expr");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'m2'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kConst, "const", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "z", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "1", "Start Mod MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kIdentifier, "log", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "z", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportAsiBeforeFromIdentifier) {
  // After the module specifier the declaration is grammatically complete:
  // `from` on the next line is an ordinary identifier starting a new
  // statement, so ASI must fire.  (The `from` continuation only applies
  // BEFORE the specifier.)
  BeginTokenizing("import 'x'\nfrom = 5;");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ImportFromSpecifierAsiBeforeFromIdentifier) {
  // Same after a from-clause specifier (`import a from 'x'`).
  BeginTokenizing("import a from 'x'\nfrom = 5;");
  ExpectToken(JsKeywords::kImport, "import", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportStarAsFromSpecifierAsiBeforeFromIdentifier) {
  // Same after `export * as ns from 'x'`.
  BeginTokenizing("export * as ns from 'x'\nfrom = 5;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "*", "Start Mod Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "as", "Start Mod Oper .");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "ns", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Mod From");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kStringLiteral, "'x'", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportLetAsiBeforeFromIdentifier) {
  // The bare binding of an export variable declaration continues only with
  // `,` or `=` -- never with `from`.
  BeginTokenizing("export let x\nfrom = 5;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kLet, "let", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportVarAsiBeforeFromIdentifier) {
  // Same for `export var x` followed by a call of the identifier `from`.
  BeginTokenizing("export var x\nfrom([1,2]);");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kVar, "var", "Start Mod MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Mod MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, "[", "Start Expr ( [");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr ( [ Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start Expr ( [");
  ExpectToken(JsKeywords::kNumber, "2", "Start Expr ( [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportFunctionAsiBeforeFromIdentifier) {
  // A function-bodied export declaration is complete at its closing brace;
  // `from` after it is an ordinary identifier, so ASI fires.
  BeginTokenizing("export function f(){}\nfrom = 5;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Mod BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportAsyncFunctionAsiBeforeFromIdentifier) {
  // Same with `async` interposing a kExpression over the module marker.
  BeginTokenizing("export async function f(){}\nfrom = 5;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start Mod Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Expr BkKwd");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f", "Start Mod Expr BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Expr BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Expr BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Expr BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod From Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "from", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetAsiAfterBinding) {
  // A plain `let` declaration grammatically ends at the bare binding:
  // `(a)()` on the next line cannot continue it (only `=` or `,` could), so
  // ASI must fire -- exactly as it does for `export let x`.  Before the
  // declaration keyword marked the bare-binding point this linebreak was
  // misclassified as a mere line separator (the generic expression
  // continuation set matches `(`), and the minifier fused the statements
  // into a SyntaxError.
  BeginTokenizing("let x\n(a)()");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, VarAsiAfterBinding) {
  // Same bare-binding insertion for `var x`: a regex literal on the next
  // line cannot continue the declaration.
  BeginTokenizing("var x\n/re/;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, VarListAsiAfterBinding) {
  // After a declarator comma the next binding is again a bare binding of
  // the declaration, so ASI fires after it.
  BeginTokenizing("var x = 5, y\n/re/;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ",", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, VarAsiBeforeSignPrefixedOperand) {
  // A unary `+`/`-` cannot continue a bare binding either: `var x\n-5;` is
  // two statements.  (The generic expression continuation set matches the
  // `-`, so this misclassified the same way before.)
  BeginTokenizing("var x\n-5;");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "-", "Start Oper");
  ExpectToken(JsKeywords::kNumber, "5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetBracketTemplateAsiAfterBinding) {
  // `[` and a template literal already fail the generic expression
  // continuation check, so these inserted a semicolon even before the
  // bare-binding point was marked -- and they still must: neither can
  // continue a bare binding.
  BeginTokenizing("let x\n[a];");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "[", "Start [");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("let x\n`t`;");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kTemplateLiteral, "`t`", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, LetVarBindingContinuations) {
  // The continuations that genuinely extend a declaration still suppress
  // ASI: `=` opens an initializer.
  BeginTokenizing("let x\n= 5;");
  ExpectToken(JsKeywords::kLet, "let", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Once the initializer is open the ordinary expression continuation rules
  // apply, so a call continues it (the bare binding is the only restricted
  // point).  `const` has no bare-binding form and behaves the same.
  BeginTokenizing("var x = 5\n(re)();");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start MVar Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start MVar Other Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start MVar Other Expr (");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start MVar Other Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start MVar Other Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start MVar Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultArrow) {
  // `export default () => {}`: the parenthesized arrow parameter list after
  // `default` begins an expression.  (The `(` directly after the `default`
  // kOtherKeyword used to error out, so the minifier passed such files
  // through.)  The arrow itself still tokenizes as `=` `>` like any other
  // arrow (tracked as a kArrow head), and the body collapses onto the
  // `default` kOtherKeyword exactly like `export default 5`.
  BeginTokenizing("export default () => {};");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultArrowBoundary) {
  // A block-bodied arrow is grammatically terminal: an ArrowFunction is an
  // AssignmentExpression that no operator, call, or index can continue
  // (`() => {} / re / g` and `() => {}(0)` are SyntaxErrors in engines), so
  // ASI fires after the body before ANY following token.
  BeginTokenizing("export default () => {}\nfoo();");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kIdentifier, "foo", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // A slash after a block-bodied arrow starts a regex statement (ASI), not
  // a division.  The minifier used to fuse these into a SyntaxError.
  BeginTokenizing("export default () => {}\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Same before a call: `() => {}\n(0)` is two statements.
  BeginTokenizing("export default () => {}\n(0);");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kNumber, "0", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultAsyncArrowAsi) {
  // `async` interposes an identifier, but `async () => {}` is the same
  // terminal block-bodied arrow: ASI fires after it.
  BeginTokenizing("export default async () => {}\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "async", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other Expr (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowBodyAsi) {
  // The terminal-arrow rule is not module-specific: a block-bodied arrow is
  // complete at its closing brace, so ASI fires before `(`...
  BeginTokenizing("x = () => {}\n(0);");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kNumber, "0", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // ...and before a slash, which starts a regex statement.
  BeginTokenizing("x = () => {}\n/re/g;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, NestedArrowBodyAsi) {
  // Nested arrows `a => b => {}`: the inner arrow's block body completes
  // the inner arrow, which IS the outer arrow's (expression) body, so the
  // outer arrow completes at the same brace -- the one-shot flag armed at
  // that brace correctly fires for both.  ASI follows; engines parse the
  // fused form as a SyntaxError.
  BeginTokenizing("x = a => b => {}\n/re/g;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr => Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr => Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr => Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => Expr => {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kRegex, "/re/g", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

// The PostfixUpdate* tests below cover the one-shot postfix_update_pending_
// flag: after a postfix ++/-- the expression is a completed
// UpdateExpression, to which no call or member access can attach, so a
// linebreak before `(` or before a `.`-led numeric literal inserts a
// semicolon even though the generic continuation set contains both.
// See https://github.com/We-Amp/pagespeed-optimizer/issues/1092

TEST_F(JsTokenizerTest, PostfixUpdateAsiParen) {
  // The kSemiInsert also proves the flag's lifetime: ConsumeOperator arms
  // it after Emit() has run for the `++` token, so it survives the
  // whitespace token and is still set at the ASI consult.
  BeginTokenizing("a++\n(b);");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PostfixUpdateAsiIndentedParen) {
  // The whitespace token handed to the ASI check includes the linebreak AND
  // the next line's indentation: by the time TryInsertLinebreakSemicolon
  // consults the remaining input, its first byte is the `(` itself, never
  // a space or tab (the generic continuation regex relies on the same
  // convention).
  BeginTokenizing("a++\n  (b);");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n  ");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PostfixUpdateAsiNumericDot) {
  // `a++\n.5` is `a++; .5;` -- the `.5` is a new statement's numeric
  // literal, not a member access.
  BeginTokenizing("a++\n.5;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kNumber, ".5", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PostfixUpdateAsiCommentCarried) {
  // A block comment containing a linebreak counts as a line terminator for
  // ASI, exactly as after return/throw.
  BeginTokenizing("a++/*\n*/(b);");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "/*\n*/");
  ExpectToken(JsKeywords::kOperator, "(", "Start (");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PostfixUpdateAsiOtherStarts) {
  // `[` is absent from the generic continuation set, so it was already
  // retained; assert the insertion stays.
  BeginTokenizing("a++\n[0];");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kSemiInsert, "\n");
  ExpectToken(JsKeywords::kOperator, "[", "Start [");
  ExpectToken(JsKeywords::kNumber, "0", "Start [ Expr");
  ExpectToken(JsKeywords::kOperator, "]", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PostfixUpdateContinuationStaysOpen) {
  // A binary operator legally continues the UpdateExpression: the linebreak
  // stays a mere separator and no semicolon is inserted.
  BeginTokenizing("a++\n*b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PostfixUpdateFlagClearedByNextToken) {
  // The flag is one-shot: the next significant token clears it, so after
  // `a++ +b` the trailing `b` is a general expression again and a linebreak
  // before `(` is an ordinary call continuation (no insertion).
  BeginTokenizing("a++ +b\n(c);");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "++", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "+", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr (");
  ExpectToken(JsKeywords::kIdentifier, "c", "Start Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowExpressionBodyContinuation) {
  // An expression body stays open: the body is an AssignmentExpression, so
  // a slash continues it as division (engines agree), exactly like
  // `export default 5`.
  BeginTokenizing("export default () => 5\n/re/g;");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Mod Other Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kNumber, "5", "Start Mod Other Expr => Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod Other Expr => Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "re", "Start Mod Other Expr => Expr");
  ExpectToken(JsKeywords::kOperator, "/", "Start Mod Other Expr => Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "g", "Start Mod Other Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ArrowFunctionExpressionBodyContinuation) {
  // A function EXPRESSION body is not a block body: it can be called (or
  // divided), so ordinary expression continuation rules apply and no ASI
  // fires at the linebreak.
  BeginTokenizing("x => function(){}\n(0);");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Expr => BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => Expr (");
  ExpectToken(JsKeywords::kNumber, "0", "Start Expr => Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  // Same continuation with paren'd arrow params (engines agree: calling
  // the result runs `function(){}(0)` as the arrow body).
  BeginTokenizing("x = () => function(){}\n(0);");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr Oper (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kFunction, "function", "Start Expr => BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Expr => BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Expr => Expr");
  ExpectToken(JsKeywords::kLineSeparator, "\n");
  ExpectToken(JsKeywords::kOperator, "(", "Start Expr => Expr (");
  ExpectToken(JsKeywords::kNumber, "0", "Start Expr => Expr ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, ExportDefaultParenthesized) {
  // The same `(`-after-`default` gate covers other parenthesized default
  // exports, which also errored out before.
  BeginTokenizing("export default (function(){});");
  ExpectToken(JsKeywords::kExport, "export", "Start Mod");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kDefault, "default", "Start Mod Other");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other (");
  ExpectToken(JsKeywords::kFunction, "function", "Start Mod Other ( BkKwd");
  ExpectToken(JsKeywords::kOperator, "(", "Start Mod Other ( BkKwd (");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other ( BkHdr");
  ExpectToken(JsKeywords::kOperator, "{", "Start Mod Other ( BkHdr {");
  ExpectToken(JsKeywords::kOperator, "}", "Start Mod Other ( Expr");
  ExpectToken(JsKeywords::kOperator, ")", "Start Mod Other Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, VarOpenParenIsError) {
  // A paren directly after a variable-declaration keyword is never valid
  // (`var (x)`): it errors out byte-preservingly, now via the
  // kModuleVarKeyword arm of ConsumeOpenParen (the `export default (...)`
  // gate above is the only exception for a keyword-state paren).
  BeginTokenizing("var (x);");
  ExpectToken(JsKeywords::kVar, "var", "Start MVar");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectError("(x);");
}

TEST_F(JsTokenizerTest, ClassIsNowModeled) {
  // `class` opens a class header/body (see the Class* tests); only
  // `enum` and a stray `extends` still error, byte-preservingly.
  BeginTokenizing("class {");
  ExpectToken(JsKeywords::kClass, "class", "Start Cls");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kOperator, "{", "Start BkHdr Cls{");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TruncatedOperatorsAtEndOfInput) {
  // Truncated input ending right after the new operators tokenizes sanely:
  // the operator token is emitted and end-of-input follows (same shape as
  // `a.` at EOF today).
  BeginTokenizing("a??");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "??", "Start Expr Oper");
  ExpectEndOfInput();

  BeginTokenizing("a?.");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "?.", "Start Expr ?.");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, EnumIsStillAnError) {
  // `enum` is reserved in ALL modes; reaching it means the input is not
  // valid JS.  The error token is the entire input (byte-recovery).
  BeginTokenizing("enum Foo{}");
  ExpectError("enum Foo{}");
}

TEST_F(JsTokenizerTest, LeadingNullishIsError) {
  BeginTokenizing("??a");
  ExpectError("??a");
}

TEST_F(JsTokenizerTest, LeadingOptionalChainIsError) {
  BeginTokenizing("?.a");
  ExpectError("?.a");
}

TEST_F(JsTokenizerTest, HashIsStillAnError) {
  // Private names (`#x`) tokenize as identifiers now (see
  // ClassPrivateElements); a bare `#` (or one not followed by an
  // identifier-start) still errors, preserving the input.
  BeginTokenizing("#5");
  ExpectError("#5");
}

TEST_F(JsTokenizerTest, PassThroughExponentiation) {
  // `**` and `**=` split into adjacent operator tokens; byte-preserving.
  BeginTokenizing("a**b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("a**=b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "*", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "*=", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PassThroughLogicalAssignment) {
  // `&&=` and `||=` split into `&&` / `||` plus `=`; byte-preserving.
  BeginTokenizing("a&&=b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "&&", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("a||=b;");
  ExpectToken(JsKeywords::kIdentifier, "a", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "||", "Start Expr Oper");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr Oper");
  ExpectToken(JsKeywords::kIdentifier, "b", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PassThroughArrow) {
  // `=>` still splits into `=` and `>` (byte-preserving), but the pair is
  // tracked as an arrow head: the kArrow state sits under the body so that
  // a block body is recognized when it closes (see ConsumeCloseBrace).
  BeginTokenizing("x=>y;");
  ExpectToken(JsKeywords::kIdentifier, "x", "Start Expr");
  ExpectToken(JsKeywords::kOperator, "=", "Start Expr =>");
  ExpectToken(JsKeywords::kOperator, ">", "Start Expr =>");
  ExpectToken(JsKeywords::kIdentifier, "y", "Start Expr => Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, PassThroughModernNumericLiterals) {
  // Numeric separators, BigInt suffixes, and binary/octal bases split into a
  // number token plus an adjacent identifier token; byte-preserving.
  BeginTokenizing("1_000;");
  ExpectToken(JsKeywords::kNumber, "1", "Start Expr");
  ExpectToken(JsKeywords::kIdentifier, "_000", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("123n;");
  ExpectToken(JsKeywords::kNumber, "123", "Start Expr");
  ExpectToken(JsKeywords::kIdentifier, "n", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();

  BeginTokenizing("0b101;0o17;");
  ExpectToken(JsKeywords::kNumber, "0", "Start Expr");
  ExpectToken(JsKeywords::kIdentifier, "b101", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectToken(JsKeywords::kNumber, "0", "Start Expr");
  ExpectToken(JsKeywords::kIdentifier, "o17", "Start Expr");
  ExpectToken(JsKeywords::kOperator, ";", "Start");
  ExpectEndOfInput();
}

TEST_F(JsTokenizerTest, TokenizeEs2020) {
  // Hand-written esbuild-style fixture covering the Tier-1 constructs
  // end-to-end; the concatenated tokens must reproduce the file exactly.
  ExpectTokenizeFileSuccessfully("es2020.js");
}

TEST_F(JsTokenizerTest, DeeplyNestedInputHitsParseStackCap) {
  // Crafted deeply nested input must produce a graceful error (the parse
  // stack depth cap) rather than growing the parse stack without bound.
  // Each entry repeats a construct that pushes a parse state without an
  // intervening pop.
  const char* const kNestings[] = {"[", "(", "{", "1?", "`${"};
  const JsTokenizerPatterns patterns;
  for (const char* nesting : kNestings) {
    GoogleString input;
    for (int i = 0; i < 10000; ++i) {
      input.append(nesting);
    }
    JsTokenizer tokenizer(&patterns, input);
    StringPiece token;
    JsKeywords::Type type;
    int num_tokens = 0;
    while ((type = tokenizer.NextToken(&token)) != JsKeywords::kError) {
      ASSERT_NE(JsKeywords::kEndOfInput, type) << "Nesting: " << nesting;
      ASSERT_LE(++num_tokens, 30000) << "Nesting: " << nesting;
    }
    EXPECT_TRUE(tokenizer.has_error()) << "Nesting: " << nesting;
  }
}

TEST_F(JsTokenizerTest, TemplateInReturnPosition) {
  // A slash-context / ASI check: `return` followed by a template on the same
  // line yields the template as the returned expression.
  BeginTokenizing("function f(){return `v=${a+b}`}");
  ExpectToken(JsKeywords::kFunction, "function");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kIdentifier, "f");
  ExpectToken(JsKeywords::kOperator, "(");
  ExpectToken(JsKeywords::kOperator, ")");
  ExpectToken(JsKeywords::kOperator, "{");
  ExpectToken(JsKeywords::kReturn, "return");
  ExpectToken(JsKeywords::kWhitespace, " ");
  ExpectToken(JsKeywords::kTemplateLiteral, "`v=${");
  ExpectToken(JsKeywords::kIdentifier, "a");
  ExpectToken(JsKeywords::kOperator, "+");
  ExpectToken(JsKeywords::kIdentifier, "b");
  ExpectToken(JsKeywords::kTemplateLiteral, "}`");
  ExpectToken(JsKeywords::kOperator, "}");
  ExpectEndOfInput();
}

}  // namespace
