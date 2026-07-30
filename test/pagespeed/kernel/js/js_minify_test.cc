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

#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/js/js_keywords.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace {

// This sample code comes from Douglas Crockford's jsmin example.
const char* kBeforeCompilation =
    "// is.js\n"
    "\n"
    "// (c) 2001 Douglas Crockford\n"
    "// 2001 June 3\n"
    "\n"
    "\n"
    "// is\n"
    "\n"
    "// The -is- object is used to identify the browser.  "
    "Every browser edition\n"
    "// identifies itself, but there is no standard way of doing it, "
    "and some of\n"
    "// the identification is deceptive. This is because the authors of web\n"
    "// browsers are liars. For example, Microsoft's IE browsers claim to be\n"
    "// Mozilla 4. Netscape 6 claims to be version 5.\n"
    "\n"
    "var is = {\n"
    "    ie:      navigator.appName == 'Microsoft Internet Explorer',\n"
    "    java:    navigator.javaEnabled(),\n"
    "    ns:      navigator.appName == 'Netscape',\n"
    "    ua:      navigator.userAgent.toLowerCase(),\n"
    "    version: parseFloat(navigator.appVersion.substr(21)) ||\n"
    "             parseFloat(navigator.appVersion),\n"
    "    win:     navigator.platform == 'Win32'\n"
    "}\n"
    "is.mac = is.ua.indexOf('mac') >= 0;\n"
    "if (is.ua.indexOf('opera') >= 0) {\n"
    "    is.ie = is.ns = false;\n"
    "    is.opera = true;\n"
    "}\n"
    "if (is.ua.indexOf('gecko') >= 0) {\n"
    "    is.ie = is.ns = false;\n"
    "    is.gecko = true;\n"
    "}\n";

const char* kAfterCompilation =
    "var is={ie:navigator.appName=='Microsoft Internet Explorer',"
    "java:navigator.javaEnabled(),ns:navigator.appName=='Netscape',"
    "ua:navigator.userAgent.toLowerCase(),version:parseFloat("
    "navigator.appVersion.substr(21))||parseFloat(navigator.appVersion)"
    ",win:navigator.platform=='Win32'}\n"
    "is.mac=is.ua.indexOf('mac')>=0;if(is.ua.indexOf('opera')>=0){"
    "is.ie=is.ns=false;is.opera=true;}"
    "if(is.ua.indexOf('gecko')>=0){is.ie=is.ns=false;is.gecko=true;}";

const char kTestRootDir[] = "/test/pagespeed/kernel/js/testdata/third_party/";

class JsMinifyTest : public testing::Test {
 protected:
  void CheckNewMinification(StringPiece before, StringPiece after) {
    GoogleString output;
    EXPECT_TRUE(pagespeed::js::MinifyUtf8Js(&patterns_, before, &output));
    EXPECT_EQ(after, output);
  }

  // The tokenizer-based minifier is the only minifier; these names are kept
  // so the ~90 call sites below read unchanged.
  void CheckMinification(StringPiece before, StringPiece after) {
    CheckNewMinification(before, after);
  }

  void CheckNewError(StringPiece input) {
    GoogleString output;
    EXPECT_FALSE(pagespeed::js::MinifyUtf8Js(&patterns_, input, &output));
  }

  void CheckError(StringPiece input) { CheckNewError(input); }

  void CheckFileMinification(StringPiece before_filename,
                             StringPiece after_filename) {
    net_instaweb::StdioFileSystem file_system;
    net_instaweb::GoogleMessageHandler message_handler;
    GoogleString original;
    {
      const GoogleString filepath =
          StrCat(net_instaweb::GTestSrcDir(), kTestRootDir, before_filename);
      ASSERT_TRUE(
          file_system.ReadFile(filepath.c_str(), &original, &message_handler));
    }
    GoogleString expected;
    {
      const GoogleString filepath =
          StrCat(net_instaweb::GTestSrcDir(), kTestRootDir, after_filename);
      ASSERT_TRUE(
          file_system.ReadFile(filepath.c_str(), &expected, &message_handler));
    }
    GoogleString actual;
    EXPECT_TRUE(pagespeed::js::MinifyUtf8Js(&patterns_, original, &actual));
    EXPECT_STREQ(expected, actual);
  }

  pagespeed::js::JsTokenizerPatterns patterns_;
};

TEST_F(JsMinifyTest, Basic) {
  CheckNewMinification(kBeforeCompilation, kAfterCompilation);
}

TEST_F(JsMinifyTest, AlreadyMinified) {
  CheckMinification(kAfterCompilation, kAfterCompilation);
}

TEST_F(JsMinifyTest, Json) {
  CheckMinification("{  \"foo\": {\"bar\": 1}, \"baz\": 2 } ",
                    "{\"foo\":{\"bar\":1},\"baz\":2}");
}

TEST_F(JsMinifyTest, ErrorUnclosedComment) {
  CheckError("/* not valid javascript");
}

TEST_F(JsMinifyTest, ErrorUnclosedString) {
  CheckError("\"not valid javascript");
}

TEST_F(JsMinifyTest, ErrorUnclosedRegex) {
  CheckError("/not_valid_javascript");
}

TEST_F(JsMinifyTest, ErrorTrailingBackslashInString) {
  // A lone backslash at end-of-input must not advance the scan position past
  // the end of the input; it is simply an unclosed string.
  CheckError("var x = 'abc\\");
}

TEST_F(JsMinifyTest, ErrorTrailingBackslashInRegex) {
  // Likewise for a lone backslash ending a regex literal.
  CheckError("/abc\\");
}

TEST_F(JsMinifyTest, DeeplyNestedInputFailsGracefully) {
  // The tokenizer caps its parse stack depth, so crafted deeply nested input
  // must produce a graceful error rather than unbounded memory growth.
  CheckNewError(GoogleString(100000, '['));
  CheckNewError(GoogleString(100000, '('));
}

TEST_F(JsMinifyTest, ErrorRegexNewline) {
  CheckError("/not_valid\njavascript/;");
}

TEST_F(JsMinifyTest, Es2020NullishCoalescing) {
  CheckNewMinification("a ?? b;", "a??b;");
  CheckNewMinification("x ??= y;", "x??=y;");
}

TEST_F(JsMinifyTest, Es2020NullishThenRegex) {
  // `??` is a binary operator, so the slash after it is a regex literal and
  // must survive minification intact.
  CheckNewMinification("a ?? /re/;", "a??/re/;");
}

TEST_F(JsMinifyTest, Es2020OptionalChaining) {
  CheckNewMinification("a ?. b;", "a?.b;");
  CheckNewMinification("r = a?.b ?? c;", "r=a?.b??c;");
}

TEST_F(JsMinifyTest, Es2020TernaryWithLeadingDecimal) {
  // `a ? .5 : b` may safely lose its spaces: `a?.5:b` re-tokenizes as the
  // same ternary because `?.` followed by a digit is `?` + `.5` (the
  // optional-chaining digit-lookahead rule).
  CheckNewMinification("a ? .5 : b;", "a?.5:b;");
}

TEST_F(JsMinifyTest, Es2019OptionalCatch) {
  CheckMinification("try { f(); } catch { g(); }", "try{f();}catch{g();}");
}

TEST_F(JsMinifyTest, Es2015Destructuring) {
  CheckNewMinification("const { a, b } = x;", "const{a,b}=x;");
  CheckNewMinification("const [ a ] = xs;", "const[a]=xs;");
  CheckNewMinification("let { a } = x;", "let{a}=x;");
}

TEST_F(JsMinifyTest, Es2015LetDeclaration) {
  CheckMinification("let x = 1;", "let x=1;");
  // A linebreak between `let` and its binding is preserved (name-after-name
  // linebreak).
  CheckMinification("let\nx = 1;", "let\nx=1;");
}

TEST_F(JsMinifyTest, Es2015LetNonBindingUses) {
  // Sloppy-mode `let`-as-variable at statement position: the binding
  // lookahead takes the identifier path, so these minify exactly as before
  // the destructuring change (slash after `let` is division).
  CheckMinification("let / 2;", "let/2;");
  CheckMinification("let = 5;", "let=5;");
  // Postfix ++ leaves an expression, so the slash is division.
  CheckMinification("let++ / 2;", "let++/2;");
}

TEST_F(JsMinifyTest, Es2020KeywordExpressions) {
  CheckNewMinification("import { a } from 'x';", "import{a}from'x';");
  CheckNewMinification("export const x = 1;", "export const x=1;");
  CheckNewMinification("p = import('x').then(f);", "p=import('x').then(f);");
  CheckNewMinification("u = import.meta.url;", "u=import.meta.url;");
  CheckNewMinification("r = super.f() / 2;", "r=super.f()/2;");
}

TEST_F(JsMinifyTest, Es2015ModuleDeclarationAsi) {
  // ASI fires at the grammatical end of an import/export declaration
  // regardless of the following token, so the linebreak is significant:
  // dropping it fuses the next statement into the declaration and produces
  // a SyntaxError.
  CheckNewMinification("import 'x'\n(main)()", "import'x'\n(main)()");
  CheckNewMinification("import a from 'x'\n/re/g", "import a from'x'\n/re/g");
  CheckNewMinification("export {a}\n(re)", "export{a}\n(re)");
  CheckNewMinification("export let x\n/re/", "export let x\n/re/");
  CheckNewMinification("export var x\n/re/", "export var x\n/re/");
}

TEST_F(JsMinifyTest, Es2015ModuleDeclarationContinuations) {
  // Declaration continuations still suppress ASI: `from` after a binding or
  // a clause, a linebreak before the module specifier, and `=` starting a
  // variable initializer.  The linebreaks that survive below are the ones
  // the minifier must keep for token separation anyway (name after name).
  CheckNewMinification("import a\nfrom 'x';", "import a\nfrom'x';");
  CheckNewMinification("import a from\n'x';", "import a from'x';");
  CheckNewMinification("export {a}\nfrom 'x';", "export{a}from'x';");
  CheckNewMinification("export let x\n= 5;", "export let x=5;");
  // An export-default or variable initializer expression keeps the ordinary
  // expression continuation rules: a slash continues it as division.
  CheckNewMinification("export default 5\n/re/g;", "export default 5/re/g;");
  CheckNewMinification("export let x = 5\n/re/g;", "export let x=5/re/g;");
  // No semicolon is needed after a function declaration -- but the
  // declaration-complete (from-clause) shape inserts anyway, keeping the
  // linebreak; engines parse both forms identically.
  CheckNewMinification("export default function(){}\nfoo();",
                       "export default function(){}\nfoo();");
}

TEST_F(JsMinifyTest, ExportDefaultFunctionAsi) {
  // A function-bodied (or class-bodied) default export is grammatically
  // complete at its closing brace: a linebreak before any following token
  // survives, and a slash starts a regex statement (node-verified, with
  // or without the linebreak).  A same-line slash keeps the
  // byte-preserving division read (re-parse identical).
  CheckNewMinification("export default function(){}\n/re/g;",
                       "export default function(){}\n/re/g;");
  CheckNewMinification("export default async function(){}\n/re/g;",
                       "export default async function(){}\n/re/g;");
  CheckNewMinification("export default function(){}/re/g;",
                       "export default function(){}/re/g;");
  CheckNewMinification("export default class {}\n/re/g;",
                       "export default class{}\n/re/g;");
  CheckNewMinification("export default function(){}\n(0);",
                       "export default function(){}\n(0);");
}

TEST_F(JsMinifyTest, Es2015ModulePositiveControl) {
  // A realistic module: two imports, an export const, and an IIFE after an
  // import line; every statement boundary survives minification.
  CheckNewMinification(
      "import a from 'm1';\n"
      "import {b} from 'm2';\n"
      "export const z = 1;\n"
      "import './polyfill'\n"
      "(main)();",
      "import a from'm1';import{b}from'm2';export const z=1;"
      "import'./polyfill'\n(main)();");
}

TEST_F(JsMinifyTest, Es2015ModuleSpecifierAsi) {
  // After the module specifier (or a function-bodied export's closing
  // brace) the declaration is grammatically complete: a following `from`
  // is an ordinary identifier (RxJS's `from`, a variable), so ASI must
  // fire and the linebreak must survive.
  CheckNewMinification("import 'x'\nfrom = 5;", "import'x'\nfrom=5;");
  CheckNewMinification("import a from 'x'\nfrom = 5;",
                       "import a from'x'\nfrom=5;");
  CheckNewMinification("import {from} from 'rxjs'\nfrom([1,2]);",
                       "import{from}from'rxjs'\nfrom([1,2]);");
  CheckNewMinification("export {a} from 'x'\nfrom = 5;",
                       "export{a}from'x'\nfrom=5;");
  CheckNewMinification("export * from 'x'\nfrom = 5;",
                       "export*from'x'\nfrom=5;");
  CheckNewMinification("export * as ns from 'x'\nfrom = 5;",
                       "export*as ns from'x'\nfrom=5;");
  CheckNewMinification("export function f(){}\nfrom = 5;",
                       "export function f(){}\nfrom=5;");
  CheckNewMinification("export async function f(){}\nfrom = 5;",
                       "export async function f(){}\nfrom=5;");
  // The bare binding of an export variable declaration continues only with
  // `,`/`=`, never with `from`.  (On these two the unfixed minifier keeps
  // the linebreak anyway for name separation, so the tokenizer-level
  // kSemiInsert assertions are the discriminating ones.)
  CheckNewMinification("export let x\nfrom = 5;", "export let x\nfrom=5;");
  CheckNewMinification("export var x\nfrom([1,2]);",
                       "export var x\nfrom([1,2]);");
}

TEST_F(JsMinifyTest, LetVarDeclarationAsi) {
  // ASI fires after the bare binding of a plain let/var declaration,
  // exactly as it does for `export let x`: dropping the linebreak fuses
  // the next statement into the declaration and produces a SyntaxError.
  CheckNewMinification("let x\n(a)()", "let x\n(a)()");
  CheckNewMinification("var x\n(a)()", "var x\n(a)()");
  CheckNewMinification("let x\n/re/;", "let x\n/re/;");
  CheckNewMinification("var x\n/re/;", "var x\n/re/;");
  CheckNewMinification("var x\n-5;", "var x\n-5;");
  CheckNewMinification("var x = 5, y\n/re/;", "var x=5,y\n/re/;");
}

TEST_F(JsMinifyTest, LetVarDeclarationContinuations) {
  // `=` after the binding continues the declaration, and an open
  // initializer keeps the ordinary expression continuation rules (a call
  // or a division continues it).  `[` and template literals cannot
  // continue a bare binding, so those linebreaks survive either way.
  CheckNewMinification("let x\n= 5;", "let x=5;");
  CheckNewMinification("var x = 5\n(re)();", "var x=5(re)();");
  CheckNewMinification("var x = 5\n/re/;", "var x=5/re/;");
  CheckNewMinification("let x\n[a];", "let x\n[a];");
  CheckNewMinification("let x\n`t`;", "let x\n`t`;");
}

TEST_F(JsMinifyTest, ExportDefaultArrow) {
  // An arrow (or any other parenthesized expression) as the default export
  // tokenizes now.  A block-bodied arrow is grammatically terminal (nothing
  // can continue it -- engines parse the fused forms as SyntaxErrors), so
  // the linebreak after it always survives; an expression body stays open
  // and keeps the ordinary continuation rules.
  CheckNewMinification("export default () => {};", "export default()=>{};");
  CheckNewMinification("export default (a) => a + 1;",
                       "export default(a)=>a+1;");
  CheckNewMinification("export default () => {}\nfoo();",
                       "export default()=>{}\nfoo();");
  CheckNewMinification("export default () => {}\n/re/g;",
                       "export default()=>{}\n/re/g;");
  CheckNewMinification("export default () => {}\n(0);",
                       "export default()=>{}\n(0);");
  CheckNewMinification("export default () => 5\n/re/g;",
                       "export default()=>5/re/g;");
  CheckNewMinification("export default (function(){});",
                       "export default(function(){});");
}

TEST_F(JsMinifyTest, ArrowBodyAsi) {
  // The terminal-arrow rule is not module-specific: ASI fires after any
  // block-bodied arrow, while an expression body continues.
  CheckNewMinification("x = () => {}\n(0);", "x=()=>{}\n(0);");
  CheckNewMinification("x = () => {}\n/re/g;", "x=()=>{}\n/re/g;");
  CheckNewMinification("x = () => 5\n/re/g;", "x=()=>5/re/g;");
}

TEST_F(JsMinifyTest, LinebreakBeforeArrowHead) {  //
  // ECMA-262 forbids a LineTerminator between an arrow head and its =>, so
  // the linebreak before => always inserts a semicolon and must survive.
  // The input was already invalid; the byte-preserving contract demands the
  // output stay invalid too (it must not minify to the valid "a=x=>y").
  CheckNewMinification("a = x\n=> y", "a=x\n=>y");
  // The same trap inside a class-field initializer.
  CheckNewMinification("class C { x = a\n=> b; }", "class C{x=a\n=>b;}");
  // Positive controls: a linebreak AFTER the arrow head is insignificant,
  // and the other =-starting continuations still apply.
  CheckNewMinification("x = a =>\nb;", "x=a=>b;");
  CheckNewMinification("a = b\n>= c;", "a=b>=c;");
  CheckNewMinification("x = y\n== w;", "x=y==w;");
}

TEST_F(JsMinifyTest, ObjectMethodShorthand) {
  // Method/get/set/async/star shorthand in object literals: the parameter
  // list is a block header and the body a block; the literal then
  // continues at property position.
  CheckNewMinification("var o = { m() {} };", "var o={m(){}};");
  CheckNewMinification("var o = { get v() { return 1; } };",
                       "var o={get v(){return 1;}};");
  CheckNewMinification("var o = { set v(x) { this._v = x; } };",
                       "var o={set v(x){this._v=x;}};");
  CheckNewMinification("var o = { async m() {}, *n() {} };",
                       "var o={async m(){},*n(){}};");
  CheckNewMinification("var o = { get ['k']() {} };", "var o={get['k'](){}};");
  CheckNewMinification("var o = { if() {} };", "var o={if(){}};");
  // Modifier words stay plain properties.
  CheckNewMinification("var o = { get: 1, async: 2 };",
                       "var o={get:1,async:2};");
  // Statement position: `{ m() {} }` is a block (SyntaxError in engines),
  // so it stays a byte-preserving pass-through; with a linebreak ASI
  // splits it into a call and an empty block.
  CheckNewMinification("{ m()\n{} }", "{m()\n{}}");
  // After the outer literal closes the ordinary operand rules apply.
  CheckNewMinification("x = { m() {} }\nfoo();", "x={m(){}}\nfoo();");
  CheckNewMinification("x = { m() {} }\n/re/g;", "x={m(){}}/re/g;");
  // A `(` after a collapsed group value is a call, not a method.
  CheckNewMinification("var o = { a: function() {}() };",
                       "var o={a:function(){}()};");
  CheckNewMinification("var o = { a: (function(){})() };",
                       "var o={a:(function(){})()};");
}

TEST_F(JsMinifyTest, Spread) {
  // `...` is a single operator token in every position.
  CheckNewMinification("h(...[]);", "h(...[]);");
  CheckNewMinification("h(1, ...xs);", "h(1,...xs);");
  CheckNewMinification("var a = [...[]];", "var a=[...[]];");
  CheckNewMinification("var o = {...{}};", "var o={...{}};");
  CheckNewMinification("var o = {...b, a: 1};", "var o={...b,a:1};");
  CheckNewMinification("const [a, ...rest] = x;", "const[a,...rest]=x;");
  CheckNewMinification("function f(...args) {}", "function f(...args){}");
}

TEST_F(JsMinifyTest, Generators) {
  // The `*` of `function*` is a generator marker in every form.
  CheckNewMinification("function* n() {}", "function*n(){}");
  CheckNewMinification("var g = function*() {};", "var g=function*(){};");
  CheckNewMinification("async function* n() {}", "async function*n(){}");
  CheckNewMinification("var o = { *n() {} };", "var o={*n(){}};");
  // `yield` is keyword-classified: a following slash starts a regex, and
  // the linebreak after it is always preserved.
  CheckNewMinification("function* g() { yield /re/g; }",
                       "function*g(){yield/re/g;}");
  CheckNewMinification("function* g() { yield\nfoo(); }",
                       "function*g(){yield\nfoo();}");
  // Sloppy-mode identifier uses still tokenize.
  CheckNewMinification("function yield() {}", "function yield(){}");
  CheckNewMinification("var yield = 1;", "var yield=1;");
  CheckNewMinification("yield: 1;", "yield:1;");
}

TEST_F(JsMinifyTest, GeneratorMethodCommaSeparator) {  //
  // A generator method whose `*` marker pushed the block keyword directly
  // onto the member-name brace used to leave that bare brace on top when the
  // body closed, so the comma separating the next member was an error and
  // the whole file declined.  The marker now installs the same kExpression a
  // plain method name leaves, and the separator takes the normal path --
  // for plain names and for the speculative words alike.
  CheckNewMinification("var o = { *m() {}, b: 2 };", "var o={*m(){},b:2};");
  CheckNewMinification("var o = { *await() { yield 1; }, b: 2 };",
                       "var o={*await(){yield 1;},b:2};");
  CheckNewMinification("var o = { *yield() {}, b: 2 };",
                       "var o={*yield(){},b:2};");
  // Mixed member forms after a generator method.
  CheckNewMinification("var o = { *m() {}, \"s\": 1, [k]: 2, n() {}, *p() {} };",
                       "var o={*m(){},\"s\":1,[k]:2,n(){},*p(){}};");
  // The same separator path inside a class-field initializer.
  CheckNewMinification("class C { x = { *m() {}, b: 2 }; }",
                       "class C{x={*m(){},b:2};}");
}

TEST_F(JsMinifyTest, RawLineSeparatorInString) {
  // Raw U+2028/U+2029 inside a string literal (legal since ES2019) pass
  // through verbatim; the byte inside the string is not a linebreak.
  CheckNewMinification(
      "var s = 'a\xE2\x80\xA8"
      "b';",
      "var s='a\xE2\x80\xA8"
      "b';");
  CheckNewMinification(
      "var s = 'a\xE2\x80\xA9"
      "b';",
      "var s='a\xE2\x80\xA9"
      "b';");
  CheckNewMinification(
      "var s = 'a\xE2\x80\xA8"
      "b'\nvar t = 1;",
      "var s='a\xE2\x80\xA8"
      "b'\nvar t=1;");
}

TEST_F(JsMinifyTest, Shebang) {
  // The hashbang line is preserved verbatim (including its linebreak), so
  // node-executable scripts stay directly executable.
  CheckNewMinification("#!/usr/bin/env node\nvar x = 1;",
                       "#!/usr/bin/env node\nvar x=1;");
  CheckNewMinification("#!/usr/bin/env node\nvar x\n(re)();",
                       "#!/usr/bin/env node\nvar x\n(re)();");
  CheckNewMinification("#!/usr/bin/env node", "#!/usr/bin/env node");
}

TEST_F(JsMinifyTest, ArrowBodyStatements) {
  // An arrow BLOCK body is a block: control statements inside it take
  // their keyword paths (webpack/esbuild runtime-arrow shapes).
  CheckNewMinification(
      "const load = () => { try { f(); } catch (e) { g(); } };",
      "const load=()=>{try{f();}catch(e){g();}};");
  CheckNewMinification("const load = () => { for (const k in obj) { f(k); } };",
                       "const load=()=>{for(const k in obj){f(k);}};");
  CheckNewMinification("const load = () => { for (x of y) {} };",
                       "const load=()=>{for(x of y){}};");
  CheckNewMinification("const load = () => { while (cond) { f(); } };",
                       "const load=()=>{while(cond){f();}};");
  CheckNewMinification("x = () => { return (a) ? b : c; };",
                       "x=()=>{return(a)?b:c;};");
  CheckNewMinification("x = () => { if (x) {} else {} };",
                       "x=()=>{if(x){}else{}};");
  CheckNewMinification("x = () => { switch (x) { case 1: break; } };",
                       "x=()=>{switch(x){case 1:break;}};");
  CheckNewMinification("x = () => { do {} while (x); };",
                       "x=()=>{do{}while(x);};");
  CheckNewMinification("x = async () => { for (x of y) {} };",
                       "x=async()=>{for(x of y){}};");
  // `x => { m: 1 }` is a block with a label, not an object literal.
  CheckNewMinification("x = () => { m: 1 };", "x=()=>{m:1};");
  CheckNewMinification("x = () => ({ m: 1 });", "x=()=>({m:1});");
}

TEST_F(JsMinifyTest, YieldSloppyForms) {
  // Sloppy-mode `yield` as an identifier before `,` or `?` (all valid
  // non-generator JS).
  CheckNewMinification("f(yield, 2);", "f(yield,2);");
  CheckNewMinification("var a = [yield, 1];", "var a=[yield,1];");
  CheckNewMinification("var yield, x;", "var yield,x;");
  CheckNewMinification("x = yield ? 1 : 2;", "x=yield?1:2;");
  // In a generator, `yield, 2` is a comma expression over a bare yield.
  CheckNewMinification("function* g() { yield, 2; }", "function*g(){yield,2;}");
}

TEST_F(JsMinifyTest, ClassBodies) {
  // Declarations, heritage, expressions, and the export forms.
  CheckNewMinification("class X {}", "class X{}");
  CheckNewMinification("class X extends Y {}", "class X extends Y{}");
  CheckNewMinification("x = class {};", "x=class{};");
  CheckNewMinification("x = class Named {};", "x=class Named{};");
  CheckNewMinification("export default class {}", "export default class{}");
  CheckNewMinification("export class X {}", "export class X{}");
  // Methods of every flavor (the object-literal method gate, reused).
  CheckNewMinification("class X { m() {} constructor() {} }",
                       "class X{m(){}constructor(){}}");
  CheckNewMinification("class X { get v() { return 1; } set v(x) {} }",
                       "class X{get v(){return 1;}set v(x){}}");
  CheckNewMinification("class X { static s() {} static {} }",
                       "class X{static s(){}static{}}");
  CheckNewMinification("class X { ['k']() {} *g() {} async a() {} #p() {} }",
                       "class X{['k'](){}*g(){}async a(){}#p(){}}");
  // Fields, private elements, and stray semicolons.
  CheckNewMinification("class X { a = 1; b; static c = 2; #d = 3; }",
                       "class X{a=1;b;static c=2;#d=3;}");
  CheckNewMinification("class X { a(){} ; b(){} ;; }",
                       "class X{a(){};b(){};;}");
  // Field ASI (node-verified): a new element name inserts, `(` continues.
  CheckNewMinification("class X { a = 1\nb = 2 }", "class X{a=1\nb=2}");
  CheckNewMinification("class X { a = 1\n(2) }", "class X{a=1(2)}");
  // After a declaration the slash is a regex at statement position with
  // or without the linebreak; after an expression it is division.
  CheckNewMinification("class X {}\n/re/g;", "class X{}/re/g;");
  CheckNewMinification("x = class {}\n/re/g;", "x=class{}/re/g;");
  CheckNewMinification("x = class {}\nfoo();", "x=class{}\nfoo();");
}

TEST_F(JsMinifyTest, ClassBareFieldAsiBeforeStar) {
  // The linebreak after a bare class field is ASI-load-bearing when the
  // next element is a generator method: dropping it fuses the field and
  // the method into one invalid `x*gen(){}` element (`*` lands in
  // initializer position as a multiplication; node-verified).
  CheckNewMinification("class C {\n  x\n  *gen() { yield 1; }\n}",
                       "class C{x\n*gen(){yield 1;}}");
  CheckNewMinification("class C {\n  static x\n  *gen() { yield 1; }\n}",
                       "class C{static x\n*gen(){yield 1;}}");
  CheckNewMinification("class C {\n  [a]\n  *gen() { yield 1; }\n}",
                       "class C{[a]\n*gen(){yield 1;}}");
  // A `*` INSIDE a field initializer is ordinary multiplication and stays
  // on one line; the linebreak before the next bare field still inserts.
  CheckNewMinification("class C {\n  x = 3 * 4\n  y\n}", "class C{x=3*4\ny}");
  // `=` and `(` legitimately continue an element name across a linebreak
  // (an initializer and a method parameter list) and are not sensitive to
  // the linebreak, so it still collapses.
  CheckNewMinification("class C {\n  x\n  = 5\n  y\n}", "class C{x=5\ny}");
  CheckNewMinification("class C {\n  x\n  () { return 1; }\n}",
                       "class C{x(){return 1;}}");
  // Between a completed method and a following generator the linebreak is
  // conservatively preserved (a just-closed method sits in the same
  // element position as a field name; both spellings are valid).
  CheckNewMinification("class C { m(){}\n*gen(){ yield 1; } }",
                       "class C{m(){}\n*gen(){yield 1;}}");
  // Object literals separate members with commas, never ASI; unchanged.
  CheckNewMinification("o = { x: 1,\n*gen(){ yield 1; } };",
                       "o={x:1,*gen(){yield 1;}};");
  // The spec-trap variant: a bare field named `async` before a generator
  // method.  Collapsing the linebreak would manufacture an ASYNC generator
  // (`async *gen(){}`); with the linebreak the spec forbids the
  // async-method reading, so it stays a plain generator.
  CheckNewMinification("class C {\n  async\n  *gen() { yield 1; }\n}",
                       "class C{async\n*gen(){yield 1;}}");
  CheckNewMinification("class C { async *gen() { yield 1; } }",
                       "class C{async*gen(){yield 1;}}");
  // Private bare field, and a class expression inside parens (the
  // element-position rule must hold with enclosing paren states on the
  // parse stack).
  CheckNewMinification("class C {\n  #x\n  *gen() { yield 1; }\n}",
                       "class C{#x\n*gen(){yield 1;}}");
  CheckNewMinification("f((class { y\n*gen() { yield 1; } }));",
                       "f((class{y\n*gen(){yield 1;}}));");
}

TEST_F(JsMinifyTest, ExportRenameKeywordComma) {
  // Keyword rename targets in an export clause, followed by a comma.
  CheckNewMinification("export { a as default, b };",
                       "export{a as default,b};");
  CheckNewMinification("export { a as if, b };", "export{a as if,b};");
  // Control: a plain rename worked all along.
  CheckNewMinification("export { a as b, c };", "export{a as b,c};");
  // Import-side reserved bindings are invalid JS in engines; the
  // tokenizer accepts them byte-preservingly.
  CheckNewMinification("import { a as default, b } from 'x';",
                       "import{a as default,b}from'x';");
}

TEST_F(JsMinifyTest, ArrowCommaEndsBody) {
  // A comma over a completed arrow expression body ends the arrow(s)
  // (node-verified): property-then-method, two call args, a sequence.
  CheckNewMinification("var o = { get: () => 1, set(v) {} };",
                       "var o={get:()=>1,set(v){}};");
  CheckNewMinification("var o = { get: (t) => t === 1 ? 2 : 3, set: 4 };",
                       "var o={get:(t)=>t===1?2:3,set:4};");
  CheckNewMinification("foo(() => 1, 2);", "foo(()=>1,2);");
  CheckNewMinification("x = () => 1, 2;", "x=()=>1,2;");
  CheckNewMinification("x = a => b => 1, 2;", "x=a=>b=>1,2;");
}

TEST_F(JsMinifyTest, DestructuringDefaultCall) {
  // A destructuring default is an expression: calls in it are calls.
  CheckNewMinification("const { canvas = f() } = x;", "const{canvas=f()}=x;");
  CheckNewMinification("const { canvas = f(), context = null } = p;",
                       "const{canvas=f(),context=null}=p;");
  CheckNewMinification("function f({ a = g() } = {}) {}",
                       "function f({a=g()}={}){}");
  CheckNewMinification("class X { constructor({ a = f() } = {}) {} }",
                       "class X{constructor({a=f()}={}){}}");
}

TEST_F(JsMinifyTest, ClassHeritageLinebreakAndTemplate) {
  // The heritage-to-body linebreak is insignificant; it drops.
  CheckNewMinification("class X extends Y\n{ m() {} }",
                       "class X extends Y{m(){}}");
  // A class body `}` inside `${...}` closes the class; the following
  // template tags it.
  CheckNewMinification("const s = `x${ class A { m() {} } `y${z}` }`;",
                       "const s=`x${class A{m(){}}`y${z}`}`;");
}

TEST_F(JsMinifyTest, SignedCharDoesntSignExtend) {
  const unsigned char input[] = {0xe0, 0xb2, 0xa0, 0x00};
  const char* input_nosign = reinterpret_cast<const char*>(input);
  CheckMinification(input_nosign, input_nosign);
}

TEST_F(JsMinifyTest, DealWithCrlf) {
  CheckMinification("var x = 1;\r\nvar y = 2;", "var x=1;var y=2;");
}

TEST_F(JsMinifyTest, DealWithTabs) {
  CheckMinification("var x = 1;\n\tvar y = 2;", "var x=1;var y=2;");
}

TEST_F(JsMinifyTest, EscapedCrlfInStringLiteral) {
  CheckMinification("var x = 'foo\\\r\nbar';", "var x='foo\\\r\nbar';");
}

TEST_F(JsMinifyTest, EmptyInput) { CheckMinification("", ""); }

TEST_F(JsMinifyTest, TreatCarriageReturnAsLinebreak) {
  CheckMinification("x = 1\ry = 2", "x=1\ny=2");
}

// See http://code.google.com/p/page-speed/issues/detail?id=607
TEST_F(JsMinifyTest, CarriageReturnEndsLineComment) {
  CheckMinification("x = 1 // foobar\ry = 2", "x=1\ny=2");
}

// See http://code.google.com/p/page-speed/issues/detail?id=198
TEST_F(JsMinifyTest, LeaveIEConditionalCompilationComments) {
  // IE conditional-compilation comments are retained verbatim.
  CheckNewMinification(
      "/*@cc_on\n"
      "  /*@if (@_win32)\n"
      "    document.write('IE');\n"
      "  @else @*/\n"
      "    document.write('other');\n"
      "  /*@end\n"
      "@*/",
      "/*@cc_on\n"
      "  /*@if (@_win32)\n"
      "    document.write('IE');\n"
      "  @else @*/document.write('other');/*@end\n"
      "@*/");
}

TEST_F(JsMinifyTest, RetainedCommentNoRegexGlue) {  //
  // Without a separator, the "/" and the retained comment's "/*" fuse into
  // "//" -- a line comment that swallows the rest of the line and silently
  // changes the program's meaning (x becomes 6 instead of 2).
  CheckNewMinification("var await = 1;\nx = await / 2 / /*@ c @*/ y;",
                       "var await=1;x=await/ 2 / /*@ c @*/y;");
  CheckNewMinification("x = a / /*@c@*/ b;", "x=a/ /*@c@*/b;");
  CheckNewMinification("var a=6,b=3,x; x = a / /*@c@*/ b;",
                       "var a=6,b=3,x;x=a/ /*@c@*/b;");
}

TEST_F(JsMinifyTest, RetainedCommentTrailingSlashNoGlue) {  //
  // The trailing "@*/" ends in "/", so a following "/" would form "//".
  CheckNewMinification("a = b /*@c@*/ / d;", "a=b/*@c@*/ /d;");
}

TEST_F(JsMinifyTest, RetainedCommentTightWhenNoGlue) {
  // When no glue hazard exists, the separator around a retained comment is
  // dropped (this is a compression improvement over the old early-return
  // path, which left a stray separator after the comment).
  CheckNewMinification("x = a /*@c@*/ b;", "x=a/*@c@*/b;");
  CheckNewMinification("f(); /*@c@*/ g();", "f();/*@c@*/g();");
  CheckNewMinification("x = 4 /*@c@*/ 5;", "x=4/*@c@*/5;");
}

TEST_F(JsMinifyTest, RetainedCommentPreservesAsiLinebreak) {  //
  CheckNewMinification("return\n/*@c@*/ 42;", "return\n/*@c@*/42;");
}

TEST_F(JsMinifyTest, RetainedCommentPreservesAsiAfterComment) {  //
  // A comment is grammatically inert: a linebreak *after* the retained comment
  // must still trigger ASI when the token before it was a speculatively-
  // classified operator word (await / of) or a restricted-production keyword.
  CheckNewMinification("var await = 1;\nawait /*@c@*/\ny;",
                       "var await=1;await/*@c@*/\ny;");
  CheckNewMinification("function f(){return /*@c@*/\n42;}",
                       "function f(){return/*@c@*/\n42;}");
  // Back-to-back retained comments: the first comment's closing "*/" already
  // terminates it, so no line/block comment forms across the boundary.
  CheckNewMinification("/*@a@*//*@b@*/", "/*@a@*//*@b@*/");
}

TEST_F(JsMinifyTest, DoNotJoinPlusses) {
  CheckMinification("var x = 'date=' + +new Date();",
                    "var x='date='+ +new Date();");
}

TEST_F(JsMinifyTest, DoNotJoinPlusAndPlusPlus) {
  CheckMinification("var x = y + ++z;", "var x=y+ ++z;");
}

TEST_F(JsMinifyTest, DoNotJoinPlusPlusAndPlus) {
  CheckMinification("var x = y++ + z;", "var x=y++ +z;");
}

TEST_F(JsMinifyTest, DoNotJoinMinuses) {
  CheckMinification("var x = 'date=' - -new Date();",
                    "var x='date='- -new Date();");
}

TEST_F(JsMinifyTest, DoNotJoinMinusAndMinusMinus) {
  CheckMinification("var x = y - --z;", "var x=y- --z;");
}

TEST_F(JsMinifyTest, DoNotJoinMinusMinusAndMinus) {
  CheckMinification("var x = y-- - z;", "var x=y-- -z;");
}

TEST_F(JsMinifyTest, DoJoinBangs) {
  CheckMinification("var x = ! ! y;", "var x=!!y;");
}

// See http://code.google.com/p/page-speed/issues/detail?id=242
TEST_F(JsMinifyTest, RemoveSurroundingSgmlComment) {
  CheckMinification("<!--\nvar x = 42;\n//-->", "var x=42;");
}

TEST_F(JsMinifyTest, RemoveSurroundingSgmlCommentWithoutSlashSlash) {
  CheckMinification("<!--\nvar x = 42;\n-->\n", "var x=42;");
}

// See http://code.google.com/p/page-speed/issues/detail?id=242
TEST_F(JsMinifyTest, SgmlLineComment) {
  CheckMinification("var x = 42; <!-- comment\nvar y = 17;",
                    "var x=42;var y=17;");
}

TEST_F(JsMinifyTest, RemoveSgmlCommentCloseOnOwnLine1) {
  CheckMinification("var x = 42;\n    --> \n", "var x=42;");
}

TEST_F(JsMinifyTest, RemoveSgmlCommentCloseOnOwnLine2) {
  CheckMinification("-->\nvar x = 42;\n", "var x=42;");
}

TEST_F(JsMinifyTest, DoNotRemoveSgmlCommentCloseInMidLine) {
  CheckMinification("var x = 42; --> \n", "var x=42;-->");
}

TEST_F(JsMinifyTest, DoNotCreateLineComment) {
  // Yes, this is legal code.  It sets x to NaN.
  CheckMinification("var x = 42 / /foo/;\n", "var x=42/ /foo/;");
}

TEST_F(JsMinifyTest, DoNotCreateBlockComment) {
  // Removing the whitespace between a regex literal's closing slash and a
  // following "*" would form "/*" -- an unintended block comment -- in any
  // downstream lexer that reads the slash as division (e.g. when "await" is
  // really a plain identifier, making the pseudo-regex two divisions).
  CheckMinification("x = / 2 / * y;", "x=/ 2 / *y;");
  CheckMinification("x = await / 2 / * y;", "x=await/ 2 / *y;");
  // A regex with flags cannot glue: the flags end the token.
  CheckMinification("x = / 2 /g * y;", "x=/ 2 /g*y;");
}

TEST_F(JsMinifyTest, DoNotCreateSgmlLineComment1) {
  // Yes, this is legal code.  It tests if x is less than not(decrement y).
  CheckMinification("if (x <! --y) { x = 0; }\n", "if(x<! --y){x=0;}");
}

TEST_F(JsMinifyTest, DoNotCreateSgmlLineComment2) {
  // Yes, this is legal code.  It tests if x is less than not(decrement y).
  CheckMinification("if (x < !--y) { x = 0; }\n", "if(x< !--y){x=0;}");
}

TEST_F(JsMinifyTest, DoNotJoinDecimalIntegerAndDot) {
  // 34 .toString() is legal code, but 34.toString() isn't, because the . in
  // the second example gets parsed as part of the literal (decimal point).  So
  // we need to leave a space in there.
  CheckNewMinification("0192  . toString()", "0192 .toString()");
}

TEST_F(JsMinifyTest, BareZeroDotPreservesSpace) {  //
  // Bare 0 is a decimal literal that can absorb a decimal point, so
  // "0 .toString()" must keep the space before the period: "0.toString()"
  // is a SyntaxError ("0." lexes as a number and the property access is
  // gone).
  CheckNewMinification("0 .toString()", "0 .toString()");
  // 00 is octal and cannot take a decimal point, so no space is needed.
  CheckNewMinification("00 .toString()", "00.toString()");
}

TEST_F(JsMinifyTest, DoJoinHexOctalIntegerAndDot) {
  // On the other hand, hex and octal literals can't have decimal points, so we
  // don't need the space here.
  CheckMinification("0x3e2  . toString() + 0172  . toString()",
                    "0x3e2.toString()+0172.toString()");
}

TEST_F(JsMinifyTest, DoJoinDecimalFractionAndDot) {
  // Also, if the decimal literal can't take another decimal point, then we can
  // safely remove the space.
  CheckMinification("3.5 . toString() + 3e2 . toString()",
                    "3.5.toString()+3e2.toString()");
}

TEST_F(JsMinifyTest, TrickyRegexLiteral) {
  // The first assignment is two divisions; the second assignment is a regex
  // literal.  JSMin gets this wrong (it removes whitespace from the regex).
  CheckMinification("var x = a[0] / b /i;\n var y = a[0] + / b /i;",
                    "var x=a[0]/b/i;var y=a[0]+/ b /i;");
}

TEST_F(JsMinifyTest, ObjectLiteralRegexLiteral) {
  // On the first line, this looks like it should be an object literal divided
  // by x divided by i, but nope, that's a block with a labelled expression
  // statement, followed by a regex literal.  The second line, on the other
  // hand, _is_ an object literal, followed by division.
  CheckMinification("{foo: 123} / x /i;", "{foo:123}/ x /i;");
  CheckNewMinification("x={foo: 1} / x /i;", "x={foo:1}/x/i;");
}

// See http://github.com/apache/incubator-pagespeed-mod/issues/327
TEST_F(JsMinifyTest, RegexLiteralWithBrackets1) {
  // The / in [^/] doesn't end the regex, so the // is not a comment.
  CheckMinification("var x = /http:\\/\\/[^/]+\\//, y = 3;",
                    "var x=/http:\\/\\/[^/]+\\//,y=3;");
}

TEST_F(JsMinifyTest, RegexLiteralWithBrackets2) {
  // The first ] is escaped and doesn't close the [, so the following / doesn't
  // close the regex, so the following space is still in the regex and must be
  // preserved.
  CheckMinification("var x = /z[\\]/ ]/, y = 3;", "var x=/z[\\]/ ]/,y=3;");
}

TEST_F(JsMinifyTest, ReturnRegex1) {
  // Make sure we understand that this is not division; "return" is not an
  // identifier!
  CheckMinification("return / x /g;", "return/ x /g;");
}

TEST_F(JsMinifyTest, ReturnRegex2) {
  // This test comes from the real world.  If "return" is incorrectly treated
  // as an identifier, the second slash will be treated as opening a regex
  // rather than closing it, and we'll error due to an unclosed regex.
  CheckMinification("return/#.+/.test(\n'#24' );", "return/#.+/.test('#24');");
}

TEST_F(JsMinifyTest, ThrowRegex) {
  // Make sure we understand that this is not division; "throw" is not an
  // identifier!  (And yes, in JS you're allowed to throw a regex.)
  CheckMinification("throw / x /g;", "throw/ x /g;");
}

TEST_F(JsMinifyTest, ReturnThrowNumber) {
  CheckMinification("return 1;\nthrow 2;", "return 1;throw 2;");
}

TEST_F(JsMinifyTest, AwaitRegex) {
  // A slash after "await" starts a regex literal, not division; the interior
  // whitespace of the regex must be preserved.
  CheckMinification("async function f(){ return await /a +b/.test(s); }",
                    "async function f(){return await/a +b/.test(s);}");
  CheckMinification("async () => { await / x /g.test(s); };",
                    "async()=>{await/ x /g.test(s);};");
  CheckMinification("await / x /g;", "await/ x /g;");
  CheckMinification("yield / x /g;", "yield/ x /g;");
}

TEST_F(JsMinifyTest, ForOfRegex) {
  // The contextual "of" keyword acts like a binary operator: a slash after it
  // starts a regex literal, not division.
  CheckMinification("for (const m of / x +y /.exec(s)) m();",
                    "for(const m of/ x +y /.exec(s))m();");
}

TEST_F(JsMinifyTest, YieldSemicolonInsertion) {
  // "yield" is a restricted production: a linebreak after it induces
  // semicolon insertion, so the linebreak must be preserved...
  CheckMinification("yield\n/ x /g;", "yield\n/ x /g;");
  // ...even when the linebreak hides inside a block comment.
  CheckMinification("yield/*\n*/x;", "yield\nx;");
  CheckMinification("yield/*\n*/'s';", "yield\n's';");
}

TEST_F(JsMinifyTest, AwaitYieldOfAsOrdinaryIdentifiers) {
  // After a period these words are ordinary property names, and a slash after
  // the member expression is division.
  CheckMinification("x.await / 2;", "x.await/2;");
  CheckMinification("x.of / 2;", "x.of/2;");
  CheckMinification("x.yield / 2;", "x.yield/2;");
  // "of" in a plain expression context is an ordinary identifier; the slashes
  // are division.
  CheckMinification("of / 2 / 2;", "of/2/2;");
  CheckMinification("x = of / 2 / 2;", "x=of/2/2;");
  CheckMinification("typeof x / 2;", "typeof x/2;");
  // As object literal property names they are ordinary identifiers.
  CheckMinification("x = { await: 1, of: 2, yield: 3 };",
                    "x={await:1,of:2,yield:3};");
  // "for await" and function declarations named await/yield still work.
  CheckMinification("for await (const x of s) f(x);",
                    "for await(const x of s)f(x);");
  CheckMinification("function await() {}", "function await(){}");
  CheckMinification("function yield() {}", "function yield(){}");
}

TEST_F(JsMinifyTest, AwaitAsVariableDegradation) {
  // Outside an async function "await" is a legal variable name, but the
  // minifier always assumes a slash after "await" begins a regex literal.
  // This is an intentional, fail-safe degradation: the pseudo-regex (really
  // two divisions) is emitted byte-for-byte, so the code still behaves the
  // same -- we merely miss removing the whitespace inside it.  (When
  // byte-for-byte reassembly would be unsafe, minification is declined
  // instead; see SpeculativeRegexCommentDelimiterGuard.)
  CheckMinification("var await = 4; x = await / 2 / 2;",
                    "var await=4;x=await/ 2 / 2;");
  // When the pseudo-regex is unterminated, minification fails outright and
  // the caller serves the original input unmodified.
  CheckError("x = await / 2;");
}

TEST_F(JsMinifyTest, SpeculativeRegexCommentDelimiterGuard) {
  // In sloppy mode "await" and "yield" can be plain identifiers, making a
  // slash after them division -- possibly followed by a comment.  If the
  // misread pseudo-regex contains or abuts a comment delimiter, removing
  // whitespace/comments around it would reassemble "//" or "/*" and change
  // the program.  The minifier declines (fails) instead, and the caller
  // serves the original input unmodified.
  CheckError("x = await / 2 // c\n+ y;");
  CheckError("function f(){ var yield = 2; return yield / 2 // half\n}");
  CheckError("x = await / a /* // */ / d;");
  // An IE conditional compilation comment between the word and the slash is
  // preserved as a token but must not end the speculative window.
  CheckError("x = await /*@ @*/ / 2 // c\n+ y;");
}

TEST_F(JsMinifyTest, SpeculativeRegexGuardNarrowness) {
  // The comment-delimiter guard exists to stop whitespace removal from
  // WELDING a "//" or "/*" that was not in the input, which can only happen
  // at the pseudo-regex's right boundary and only when it has no flags (a
  // flag letter is the last byte emitted, and nothing welds onto that).
  // Comment delimiters that cannot reassemble must not decline.
  CheckMinification("async function f(){ return await /a/g/*c*/ }",
                    "async function f(){return await/a/g}");
  CheckMinification("function* g(){ yield /2/g/*c*/ }",
                    "function*g(){yield/2/g}");
  // A "//" or "/*" inside a character class is inert: it is part of the
  // verbatim literal and cannot open a comment that escapes it.
  CheckMinification("async function f(){ return await /[//]/.test(x) }",
                    "async function f(){return await/[//]/.test(x)}");
  CheckMinification("async function f(){ return await /[/*]/.test(x) }",
                    "async function f(){return await/[/*]/.test(x)}");
  CheckMinification("for (m of /[//]/.exec(s)) m();",
                    "for(m of/[//]/.exec(s))m();");
  // The genuine protection is unchanged: an unflagged pseudo-regex whose
  // closing slash abuts a "/" or "*" would weld into a comment delimiter.
  CheckError("x = await / 2 // c\n+ y;");
  CheckError("x = await / a /* // */ / d;");
  CheckError("log(await //*c*/b//c\n/ /*@cc@*/ c)");
}

TEST_F(JsMinifyTest, SpeculativeOperatorPreservesLinebreak) {
  // If "await" (or an "of" after an expression) is really a plain identifier,
  // the linebreak after it may be load-bearing for semicolon insertion, so it
  // is never removed.  Preserving it is always semantics-neutral: in real
  // async/for-of code the linebreak is legal there too.
  CheckMinification("x = await\n'use strict';", "x=await\n'use strict';");
  CheckMinification("x = await\n!function(){}();", "x=await\n!function(){}();");
  CheckMinification("let of\n's';", "let of\n's';");
}

TEST_F(JsMinifyTest, SpeculativeUpdateOperatorPreservesLinebreak) {
  // If "await"/"yield" is really a plain identifier, `await++` is a
  // completed postfix UpdateExpression and the linebreak after it may be
  // load-bearing for semicolon insertion; under the operator-word reading
  // the `++` is a prefix operator and a linebreak before its operand is
  // legal.  Preserving the linebreak is semantics-neutral in both readings,
  // so it is never removed.
  CheckMinification("var await = 5;\nawait++\nconsole.log(await);",
                    "var await=5;await++\nconsole.log(await);");
  CheckMinification("var await = 5;\nawait--\nconsole.log(await);",
                    "var await=5;await--\nconsole.log(await);");
  CheckMinification("function f(){ var yield = 2; yield++\ng(); }",
                    "function f(){var yield=2;yield++\ng();}");
  // Same line: nothing is load-bearing; minifies fully.
  CheckMinification("var await = 5; await++; f();", "var await=5;await++;f();");
  // Control: an ordinary postfix update keeps its ASI linebreak via the
  // postfix-update path, unchanged.
  CheckMinification("q++\nconsole.log(q);", "q++\nconsole.log(q);");
}

TEST_F(JsMinifyTest, SpeculativeOperatorClosePaths) {
  // A dangling speculative kOperator (an `await` that turned out to be a
  // plain identifier with no operand) must be tolerated by every close and
  // separator path.
  CheckMinification("f( await );", "f(await);");
  CheckMinification("x = ( await ) / 2;", "x=(await)/2;");
  CheckMinification("a[ await ] = 1;", "a[await]=1;");
  CheckMinification("x = await ;", "x=await;");
  CheckMinification("x = `a${ await }b`;", "x=`a${await}b`;");
  CheckMinification("function f(){ return await }",
                    "function f(){return await}");
  CheckMinification("await: while(1) break await;",
                    "await:while(1)break await;");
  CheckMinification("switch(x){ case await: break; }",
                    "switch(x){case await:break;}");
  CheckMinification("o?.await / 2;", "o?.await/2;");
}

TEST_F(JsMinifyTest, SpeculativeOperatorBlockGuard) {
  // If "await" is really a plain identifier, "await\n{ }" is an
  // ASI-separated statement followed by a BLOCK, and a slash after the block
  // starts a regex; the operator reading would instead make the braces an
  // object literal and the slash division, stripping the regex's interior
  // whitespace.  "{" is the one token whose reading diverges, so the
  // minifier declines and the caller serves the original input unmodified.
  CheckError("var await = 1;\nawait\n{ } / x /.test(y);");
  CheckError("await\n{ }\n/ x /.test(y);");
  CheckError("await\n{ } /* c */ / x /.test(y);");
  CheckError("await\n{ a: 1 } / x /.test(y);");
  CheckError("await\n{ x = 1 } / y /.test(z);");
  // The decline covers any "{" reached across a line terminator in the
  // speculative window, whether or not a slash follows the block (decline
  // is acceptable; wrong output is not).
  CheckError("await\n{ }\n(x);");
  // WITHOUT a line terminator the identifier reading is impossible
  // ("expr {" is a SyntaxError), so the "{" is provably the operand's
  // object literal and the minifier commits instead of declining.
  CheckNewMinification("x = yield { a: 1 };", "x=yield{a:1};");
  CheckNewMinification("function* g() { yield { a: 1 }; }",
                       "function*g(){yield{a:1};}");
  CheckNewMinification("async function f() { await { a: 1 }; }",
                       "async function f(){await{a:1};}");
  CheckNewMinification("for (x of { a: 1 }) {}", "for(x of{a:1}){}");
  // A line terminator INSIDE a block comment counts for ASI (the spec
  // treats such a comment as a line terminator): still ambiguous, still
  // declined.  A same-line comment without one commits.
  CheckError("function* g() { yield /*\n*/ { a: 1 }; }");
  CheckNewMinification("function* g() { yield /* c */ { a: 1 }; }",
                       "function*g(){yield{a:1};}");
  // `await` in a BINDING position names the thing being declared (legal in
  // sloppy mode) -- it is not an operator and must not open the speculative
  // window: the class body's `{` takes the class-brace path and a slash
  // after the completed declaration is a statement-position REGEX, never
  // division (the adversarial review's counterexample to the same-line
  // commit).
  CheckNewMinification("class await {}\n/ a /.test(b) ? f() : g();",
                       "class await{}/ a /.test(b)?f():g();");
  CheckNewMinification("class await extends B {}",
                       "class await extends B{}");
  CheckNewMinification("function await() { return 1; }",
                       "function await(){return 1;}");
  // `class yield {}` is a SyntaxError in every mode; byte-preserving.
  CheckNewMinification("class yield {}", "class yield{}");
}

TEST_F(JsMinifyTest, AwaitYieldOfAsMemberNames) {
  // `await`, `yield` and `of` are all legal member names.  After a
  // `get`/`set`/`async` modifier -- or a generator `*` -- the word is
  // unambiguously the member's NAME, so it must not take the
  // speculative-operator classification: doing so leaves the method's block
  // unmatched and declines the whole file.
  CheckMinification("obj = { get of(){ return 1 } };",
                    "obj={get of(){return 1}};");
  CheckMinification("obj = { set of(v){} };", "obj={set of(v){}};");
  CheckMinification("obj = { async of(){} };", "obj={async of(){}};");
  CheckMinification("obj = { *await(){} };", "obj={*await(){}};");
  CheckMinification("obj = { get await(){} };", "obj={get await(){}};");
  CheckMinification("obj = { async await(){} };", "obj={async await(){}};");
  CheckMinification("obj = { *yield(){} };", "obj={*yield(){}};");
  CheckMinification("obj = { get yield(){} };", "obj={get yield(){}};");
  // The same in a class body, and with the `static` modifier.
  CheckMinification("class C { get of(){} }", "class C{get of(){}}");
  CheckMinification("class C { get await(){} }", "class C{get await(){}}");
  CheckMinification("class C { static *yield(){} }",
                    "class C{static*yield(){}}");
  // The generator `*` only becomes a method marker before a plain name;
  // computed and string names keep the ordinary operator path.
  CheckMinification("obj = { *[Symbol.iterator](){} };",
                    "obj={*[Symbol.iterator](){}};");
  CheckMinification("class C { *[Symbol.iterator](){} }",
                    "class C{*[Symbol.iterator](){}}");
  CheckMinification("obj = { async *gen(){ yield 1 } };",
                    "obj={async*gen(){yield 1}};");
}

TEST_F(JsMinifyTest, AwaitAsSeparatedIdentifier) {
  // In sloppy mode `await` is an ordinary identifier, so a separator may
  // follow it directly.  A speculative operator with no operand is exactly
  // that case, and the separator collapses it back to an expression.
  CheckMinification("var await, x;", "var await,x;");
  CheckMinification("f(await, 1);", "f(await,1);");
  CheckMinification("x = cond ? await : val;", "x=cond?await:val;");
  CheckMinification("x = [await, 1];", "x=[await,1];");
  CheckMinification("x = {a: await, b: 1};", "x={a:await,b:1};");
  CheckMinification("function f(await, b){ return await + b }",
                    "function f(await,b){return await+b}");
  // ...while a genuine await expression still collapses into the surrounding
  // expression, so the enclosing list separators keep working.
  CheckMinification("async function f(){ return { a: await g(), b: 1 }; }",
                    "async function f(){return{a:await g(),b:1};}");
  CheckMinification("async function f(){ await g(), await h(); }",
                    "async function f(){await g(),await h();}");
}

TEST_F(JsMinifyTest, KeywordPrecedesRegex) {
  // Make sure "typeof /./" sees the first "/" as a regex and not division.
  // If it thinks it's a division then it will treat the "/    /" as a regex
  // and not remove the comment.  Do the same for all such keywords.
  // Example, "typeof /./    /* hi there */;" ->  "typeof/./;"
  for (pagespeed::JsKeywords::Iterator iter; !iter.AtEnd(); iter.Next()) {
    if (pagespeed::JsKeywords::CanKeywordPrecedeRegEx(iter.name())) {
      GoogleString input = StrCat(iter.name(), " /./   /* hi there */;");
      GoogleString expected = StrCat(iter.name(), "/./;");
      CheckMinification(input, expected);
    }
  }
}

TEST_F(JsMinifyTest, LoopRegex) {
  // Make sure we understand that a slash after "while (...)" or "for (...)" is
  // a regex, not division.
  CheckNewMinification("while (0) /\\//.exec('');", "while(0)/\\//.exec('');");
  CheckNewMinification("for (x in y) / z /.exec(x);",
                       "for(x in y)/ z /.exec(x);");
}

TEST_F(JsMinifyTest, LabelRegex) {
  // Make sure we understand that a slash after a label is a regex, not
  // division.
  CheckMinification("{ foo: / x /.exec(''); }", "{foo:/ x /.exec('');}");
}

const char kCrashTestString[] =
    "var x = 'asd \\' lse'\n"
    "var y /*comment*/ = /regex/\n"
    "var z = \"x =\" + x\n";

TEST_F(JsMinifyTest, DoNotCrash) {
  // Run on all possible prefixes of kCrashTestString.  We don't care about the
  // result; we just want to make sure it doesn't crash.
  for (int i = 0, size = sizeof(kCrashTestString); i <= size; ++i) {
    GoogleString input(kCrashTestString, i);
    GoogleString output;
    pagespeed::js::MinifyUtf8Js(&patterns_, input, &output);
  }
}

// The below tests check for some corner cases of semicolon insertion, to make
// sure that we are minifying as much as possible (and no more!).
// See http://inimino.org/~inimino/blog/javascript_semicolons for details.

TEST_F(JsMinifyTest, SemicolonInsertionIncrement) {
  CheckMinification("a\n++b\nc++\nd", "a\n++b\nc++\nd");
  // A trickier case: the linebreak inside the operator is removable.
  CheckNewMinification("a\n++\nb\nc++\nd", "a\n++b\nc++\nd");
}

TEST_F(JsMinifyTest, SemicolonInsertionDecrement) {
  CheckMinification("a\n--b\nc--\nd", "a\n--b\nc--\nd");
  // A trickier case: the linebreak inside the operator is removable.
  CheckNewMinification("a\n--\nb\nc--\nd", "a\n--b\nc--\nd");
}

TEST_F(JsMinifyTest, SemicolonInsertionAddition) {
  // No semicolons will be inserted, so the linebreaks can be removed.
  CheckMinification("i\n+\nj", "i+j");
}

TEST_F(JsMinifyTest, SemicolonInsertionSubtraction) {
  // No semicolons will be inserted, so the linebreaks can be removed.
  CheckMinification("i\n-\nj", "i-j");
}

TEST_F(JsMinifyTest, SemicolonInsertionLogicalOr) {
  // No semicolons will be inserted, so the linebreaks can be removed.
  CheckMinification("i\n||\nj", "i||j");
}

TEST_F(JsMinifyTest, SemicolonInsertionFuncCall) {
  // No semicolons will be inserted, so the linebreak can be removed.  This is
  // actually a function call, not two statements.
  CheckMinification("a = b + c\n(d + e).print()", "a=b+c(d+e).print()");
}

// The SemicolonInsertionPostfix* tests below cover ASI after a completed
// postfix ++/-- expression: no call or member access can attach to an
// UpdateExpression, so a linebreak before `(`, or before a `.`-led numeric
// literal, inserts a semicolon and must be retained -- even though the same
// tokens after a general expression would continue the statement (see
// SemicolonInsertionFuncCall above).  All assertions are new-minifier-only:
// the deprecated legacy path is independently broken for this class and is
// slated for deletion.
// See https://github.com/We-Amp/pagespeed-optimizer/issues/1092

TEST_F(JsMinifyTest, SemicolonInsertionPostfixParen) {
  // A `(` after a postfix operator starts a new statement, not a call, so
  // the linebreak inserts a semicolon and cannot be removed.
  CheckNewMinification("a++\n(b)()", "a++\n(b)()");
  CheckNewMinification("a--\n(a)", "a--\n(a)");
  CheckNewMinification("a.v--\n(a)", "a.v--\n(a)");
  CheckNewMinification("x = a++\n(b)", "x=a++\n(b)");
  CheckNewMinification("(a)++\n(b)", "(a)++\n(b)");
  // An indented continuation line: the whitespace run (linebreak plus the
  // indentation) collapses to just the linebreak.
  CheckNewMinification("a++\n  (b)", "a++\n(b)");
}

TEST_F(JsMinifyTest, SemicolonInsertionPostfixNumericDot) {
  // A `.`-led numeric literal after a postfix operator starts a new
  // statement (`a++; .5;`), not a member access: the linebreak stays.
  CheckNewMinification("a++\n.5", "a++\n.5");
}

TEST_F(JsMinifyTest, SemicolonInsertionPostfixCommentCarried) {
  // A comment-borne linebreak after a postfix operator behaves exactly like
  // a real one: the comment is dropped and the linebreak materializes.
  CheckNewMinification("a++/*\n*/(b)", "a++\n(b)");
  CheckNewMinification("a++//c\n(b)", "a++\n(b)");
}

TEST_F(JsMinifyTest, SemicolonInsertionPostfixOtherStatementStarts) {
  // Statement starters that never appeared in the generic continuation set
  // were already retained; lock them in.
  CheckNewMinification("a++\n[0]", "a++\n[0]");
  CheckNewMinification("a++\n++b", "a++\n++b");
  CheckNewMinification("a++\nb", "a++\nb");
  CheckNewMinification("a++\n~b", "a++\n~b");
  CheckNewMinification("a++\n!b", "a++\n!b");
  CheckNewMinification("a++\n`t`", "a++\n`t`");
}

TEST_F(JsMinifyTest, SemicolonInsertionPostfixContinuationsStillJoin) {
  // Binary and ternary operators legally continue an UpdateExpression, so
  // these linebreaks stay insignificant and are still removed.
  CheckNewMinification("a++\n* b", "a++*b");
  CheckNewMinification("a++\n/ b", "a++/b");
  CheckNewMinification("a++\n% b", "a++%b");
  CheckNewMinification("a++\n, b", "a++,b");
  CheckNewMinification("a++\n? b : c", "a++?b:c");
  CheckNewMinification("a++\n== b", "a++==b");
  CheckNewMinification("x = a++\nin b", "x=a++in b");
  CheckNewMinification("x = a++\ninstanceof b", "x=a++instanceof b");
  // A `+` continues too, but a separator must remain to avoid fusing
  // `+++`; the minifier keeps the linebreak (same size as a space).
  CheckNewMinification("a++\n+ b", "a++\n+b");
  // A real semicolon or a closing paren suppresses insertion outright.
  CheckNewMinification("a++\n;", "a++;");
  CheckNewMinification("f(a++\n)", "f(a++)");
}

TEST_F(JsMinifyTest, SemicolonInsertionPostfixIdempotent) {
  // Minifying already-minified output changes nothing.
  CheckNewMinification("a++\n(b)()", "a++\n(b)()");
  CheckNewMinification("x=a++\n(b)", "x=a++\n(b)");
  CheckNewMinification("a++\n(b)", "a++\n(b)");
  CheckNewMinification("a++\n.5", "a++\n.5");
  CheckNewMinification("a++*b", "a++*b");
  CheckNewMinification("a++\n+b", "a++\n+b");
  CheckNewMinification("x=a++in b", "x=a++in b");
  CheckNewMinification("x=a++instanceof b", "x=a++instanceof b");
  CheckNewMinification("a++;", "a++;");
  CheckNewMinification("f(a++)", "f(a++)");
}

TEST_F(JsMinifyTest, SemicolonInsertionRegex) {
  // No semicolon will be inserted, so the linebreak and spaces can be removed
  // (this is two divisions, not a regex).
  CheckMinification("i=0\n/ [a-z] /g.exec(s)", "i=0/[a-z]/g.exec(s)");
}

TEST_F(JsMinifyTest, SemicolonInsertionComment) {
  CheckMinification("a=b\n /*hello*/ c=d\n", "a=b\nc=d");
}

TEST_F(JsMinifyTest, SemicolonInsertionWhileStmt) {
  // No semicolon will be inserted, so the linebreak can be removed.
  CheckMinification("while\n(true);", "while(true);");
}

TEST_F(JsMinifyTest, SemicolonInsertionReturnStmt1) {
  // A semicolon _will_ be inserted, so the linebreak _cannot_ be removed.
  CheckMinification("return\n(true);", "return\n(true);");
}

TEST_F(JsMinifyTest, SemicolonInsertionReturnStmt2) {
  // A semicolon _will_ be inserted, so the linebreak _cannot_ be removed.
  CheckMinification("return\n/*comment*/(true);", "return\n(true);");
}

TEST_F(JsMinifyTest, SemicolonInsertionCommentWithLinebreak1) {
  // A block comment containing a linebreak counts as a line terminator, so a
  // semicolon _will_ be inserted; the linebreak _cannot_ be removed.
  CheckMinification("return/*\n*/x", "return\nx");
}

TEST_F(JsMinifyTest, SemicolonInsertionCommentWithLinebreak2) {
  CheckMinification("throw/*\n*/x", "throw\nx");
}

TEST_F(JsMinifyTest, SemicolonInsertionCommentWithLinebreak3) {
  // Here one-token lookahead alone would say the paren could continue the
  // statement, but after `return` the (comment-borne) linebreak still
  // inserts a semicolon.
  CheckMinification("return/*\n*/(true);", "return\n(true);");
}

TEST_F(JsMinifyTest, SemicolonInsertionCommentWithoutLinebreak) {
  // A block comment with no linebreak is mere whitespace: no semicolon is
  // inserted, and the tokens stay joined on one line.
  CheckMinification("return/* */x", "return x");
}

TEST_F(JsMinifyTest, SemicolonInsertionCommentLinebreakStatementBoundaries) {
  // Postfix ++/-- are restricted productions: a comment-borne linebreak
  // between the operand and the operator inserts a semicolon, so joining
  // them would turn valid code ("x; ++y;") into a syntax error ("x++y").
  CheckMinification("x/*\n*/++y", "x\n++y");
  CheckMinification("x/*\n*/--y", "x\n--y");
  // Likewise a linebreak after a statement-ending ++/-- must be kept.
  CheckMinification("x++/*\n*/y", "x++\ny");
  CheckMinification("x--/*\n*/y", "x--\ny");
  // A string literal cannot continue an expression, so the linebreak before
  // it inserts a semicolon; joining would produce invalid code ("x"s"").
  CheckMinification("x/*\n*/\"s\"", "x\n\"s\"");
  CheckMinification("\"use strict\"/*\n*/x", "\"use strict\"\nx");
  // Same for a call expression followed by a new statement.
  CheckMinification("foo()/*\n*/bar()", "foo()\nbar()");
  // Control cases: continuations must still join (no semicolon is inserted
  // before a token that can continue the statement).
  CheckMinification("x/*\n*/(y)", "x(y)");
  CheckMinification("x = 1/*\n*/+ 2", "x=1+2");
  // `[` and a template literal are treated conservatively (the linebreak is
  // kept, exactly as for a real linebreak in the same position); the result
  // re-parses identically to the input.
  CheckNewMinification("x/*\n*/[y]", "x\n[y]");
  CheckNewMinification("x/*\n*/`t`", "x\n`t`");
}

TEST_F(JsMinifyTest, SemicolonInsertionCommentLinebreakEdges) {
  // A comment-borne linebreak at the very start or end of the input has no
  // ASI effect and must not emit a stray linebreak.
  CheckMinification("/*\n*/x", "x");
  CheckMinification("x/*\n*/", "x");
  // A conditional-compilation comment is retained verbatim even when it
  // contains a linebreak; the retained text itself carries the linebreak.
  CheckMinification("x/*@cc_on\n@*/y", "x/*@cc_on\n@*/y");
  // do/while: the linebreak is kept (conservatively, as for a real
  // linebreak) and the loop still parses.
  CheckMinification("do x/*\n*/while(y)", "do x\nwhile(y)");
  // break/continue are restricted productions, like return/throw.
  CheckMinification("break/*\n*/label;", "break\nlabel;");
  CheckMinification("continue/*\n*/label;", "continue\nlabel;");
  // yield is restricted inside generators.
  CheckMinification("function* f(){yield/*\n*/x}", "function*f(){yield\nx}");
  // instanceof continues the expression; the linebreak is kept only because
  // two names may not be joined.
  CheckMinification("x = 5/*\n*/instanceof y", "x=5\ninstanceof y");
  // A class body brace after the heritage expression is not a statement
  // block, so it is joined (as for a real linebreak).
  CheckNewMinification("class X extends Y/*\n*/{}", "class X extends Y{}");
}

TEST_F(JsMinifyTest, SemicolonInsertionThrowStmt) {
  // This is _not_ legal code; don't accidentally make it legal by removing the
  // linebreak.  (Eliminating a syntax error would change the semantics!)
  CheckMinification("throw\n  'error';", "throw\n'error';");
}

TEST_F(JsMinifyTest, SemicolonInsertionBreakStmt) {
  // A semicolon _will_ be inserted, so the linebreak _cannot_ be removed.
  CheckMinification("break\nlabel;", "break\nlabel;");
}

TEST_F(JsMinifyTest, SemicolonInsertionContinueStmt) {
  // A semicolon _will_ be inserted, so the linebreak _cannot_ be removed.
  CheckMinification("continue\nlabel;", "continue\nlabel;");
}

TEST_F(JsMinifyTest, SemicolonInsertionDebuggerStmt) {
  // A semicolon _will_ be inserted, so the linebreak _cannot_ be removed.
  CheckMinification("debugger\nfoo;", "debugger\nfoo;");
}

TEST_F(JsMinifyTest, Latin1Input) {
  // Try to minify input that is Latin-1 encoded.  This is not valid UTF-8, but
  // we should be able to proceed gracefully (in most cases) if the non-ascii
  // characters only ever appear in string literals and comments.
  CheckMinification(
      "str='Qu\xE9 pasa';// 'qu\xE9' means 'what'\n"
      "cents=/* 73\xA2 is $0.73 */73;",
      "str='Qu\xE9 pasa';cents=73;");
}

TEST_F(JsMinifyTest, MinifyAngular) {
  CheckFileMinification("angular.original", "angular.minified");
}

TEST_F(JsMinifyTest, MinifyJQuery) {
  CheckFileMinification("jquery.original", "jquery.minified");
}

TEST_F(JsMinifyTest, MinifyPrototype) {
  CheckFileMinification("prototype.original", "prototype.minified");
}

// Simple method for serializing Mappings so that they can be compared against
// gold versions.
GoogleString MappingsToString(
    const net_instaweb::source_map::MappingVector& mappings) {
  GoogleString result("{");
  for (int i = 0, n = mappings.size(); i < n; ++i) {
    StrAppend(&result, "(", net_instaweb::IntegerToString(mappings[i].gen_line),
              ", ", net_instaweb::IntegerToString(mappings[i].gen_col), ", ");
    StrAppend(&result, net_instaweb::IntegerToString(mappings[i].src_file),
              ", ", net_instaweb::IntegerToString(mappings[i].src_line), ", ",
              net_instaweb::IntegerToString(mappings[i].src_col), "), ");
  }
  result += "}";
  return result;
}

TEST_F(JsMinifyTest, SourceMapsSimple) {
  const char js_before[] =
      "/* Simple hello world program. */\n"
      "alert( 'Hello, World!' );\n";
  const char expected_js_after[] = "alert('Hello, World!');";
  const char expected_map[] =
      "{"
      "(0, 0, 0, 1, 0), "    // alert(
      "(0, 6, 0, 1, 7), "    // 'Hello, World!'
      "(0, 21, 0, 1, 23), "  // );
      "}";

  GoogleString output;
  net_instaweb::source_map::MappingVector mappings;
  EXPECT_TRUE(pagespeed::js::MinifyUtf8JsWithSourceMap(&patterns_, js_before,
                                                       &output, &mappings));

  EXPECT_EQ(expected_js_after, output);

  EXPECT_EQ(expected_map, MappingsToString(mappings));
}

TEST_F(JsMinifyTest, SourceMapsComplex) {
  GoogleString output;
  net_instaweb::source_map::MappingVector mappings;
  EXPECT_TRUE(pagespeed::js::MinifyUtf8JsWithSourceMap(
      &patterns_, kBeforeCompilation, &output, &mappings));

  EXPECT_EQ(kAfterCompilation, output);

  const char expected_map[] =
      "{"
      "(0, 0, 0, 14, 0), "  // var is
      "(0, 6, 0, 14, 7), "  // =
      "(0, 7, 0, 14, 9), "  // {

      "(0, 8, 0, 15, 4), "    // ie:
      "(0, 11, 0, 15, 13), "  // navigator.appName
      "(0, 28, 0, 15, 31), "  // ==
      "(0, 30, 0, 15, 34), "  // 'Microsoft Internet Explorer',

      "(0, 60, 0, 16, 4), "   // java:
      "(0, 65, 0, 16, 13), "  // navigator.javaEnabled(),

      "(0, 89, 0, 17, 4), "    // ns:
      "(0, 92, 0, 17, 13), "   // navigator.appName
      "(0, 109, 0, 17, 31), "  // ==
      "(0, 111, 0, 17, 34), "  // 'Netscape',

      "(0, 122, 0, 18, 4), "   // ua:
      "(0, 125, 0, 18, 13), "  // navigator.userAgent.toLowerCase(),

      "(0, 159, 0, 19, 4), "   // version:
      "(0, 167, 0, 19, 13), "  // parseFloat(navigator.appVersion.substr(21))
      "(0, 210, 0, 19, 57), "  // ||
      "(0, 212, 0, 20, 13), "  // parseFloat(navigator.appVersion),

      "(0, 245, 0, 21, 4), "   // win:
      "(0, 249, 0, 21, 13), "  // navigator.platform
      "(0, 267, 0, 21, 32), "  // ==
      "(0, 269, 0, 21, 35), "  // 'Win32'
      "(0, 276, 0, 22, 0), "   // }

      "(1, 0, 0, 23, 0), "    // is.mac
      "(1, 6, 0, 23, 7), "    // =
      "(1, 7, 0, 23, 9), "    // is.ua.indexOf('mac')
      "(1, 27, 0, 23, 30), "  // >=
      "(1, 29, 0, 23, 33), "  // 0;

      "(1, 31, 0, 24, 0), "   // if
      "(1, 33, 0, 24, 3), "   // (is.ua.indexOf('opera')
      "(1, 56, 0, 24, 27), "  // >=
      "(1, 58, 0, 24, 30), "  // 0)
      "(1, 60, 0, 24, 33), "  // {

      "(1, 61, 0, 25, 4), "   // is.ie
      "(1, 66, 0, 25, 10), "  // =
      "(1, 67, 0, 25, 12), "  // is.ns
      "(1, 72, 0, 25, 18), "  // =
      "(1, 73, 0, 25, 20), "  // false;

      "(1, 79, 0, 26, 4), "   // is.opera
      "(1, 87, 0, 26, 13), "  // =
      "(1, 88, 0, 26, 15), "  // true;
      "(1, 93, 0, 27, 0), "   // }

      "(1, 94, 0, 28, 0), "    // if
      "(1, 96, 0, 28, 3), "    // (is.ua.indexOf('gecko')
      "(1, 119, 0, 28, 27), "  // >=
      "(1, 121, 0, 28, 30), "  // 0)
      "(1, 123, 0, 28, 33), "  // {

      "(1, 124, 0, 29, 4), "   // is.ie
      "(1, 129, 0, 29, 10), "  // =
      "(1, 130, 0, 29, 12), "  // is.ns
      "(1, 135, 0, 29, 18), "  // =
      "(1, 136, 0, 29, 20), "  // false;

      "(1, 142, 0, 30, 4), "   // is.gecko
      "(1, 150, 0, 30, 13), "  // =
      "(1, 151, 0, 30, 15), "  // true;
      "(1, 156, 0, 31, 0), "   // }
      "}";

  EXPECT_EQ(expected_map, MappingsToString(mappings));
}

}  // namespace
