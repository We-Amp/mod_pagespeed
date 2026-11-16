// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Tests for lib/html/doctype.cc, ported from mod_pagespeed 1.15's
// test/pagespeed/kernel/html/doctype_test.cc and extended with the
// doctype behavior battery (39-input matrix measured against both parsers):
// the case-insensitive FPI extension, the about:legacy-compat fix, and every
// input on which the old substring-heuristic cascade diverged from 1.15's
// exact-matching parser.
//
// Inputs below are the directive token exactly as the lexer produces it
// (the bytes between "<!" and ">", per HtmlLexer::EmitDirective).

#include "lib/html/doctype.h"

#include "gtest/gtest.h"
#include "lib/html/content_type.h"

namespace net_instaweb {
namespace {

// Parses |directive| under text/html and returns the resulting DocType.
// Expects Parse() to accept the directive.
DocType ParseHtml(std::string_view directive) {
  DocType doctype;
  EXPECT_TRUE(doctype.Parse(directive, kContentTypeHtml));
  return doctype;
}

DocType ParseXhtml(std::string_view directive) {
  DocType doctype;
  EXPECT_TRUE(doctype.Parse(directive, kContentTypeXhtml));
  return doctype;
}

// --- Ported from 1.15's doctype_test.cc ---

TEST(DocTypeTest, NonDoctypeDirective) {
  DocType doctype;
  EXPECT_FALSE(doctype.Parse("foobar", kContentTypeHtml));
  EXPECT_EQ(DocType::kUnknown, doctype);
}

TEST(DocTypeTest, UnknownDoctype) {
  EXPECT_EQ(DocType::kUnknown, ParseHtml("doctype foo bar baz"));
}

TEST(DocTypeTest, DetectHtml5) {
  EXPECT_EQ(DocType::kHTML5, ParseHtml("doctype html"));
  EXPECT_EQ(DocType::kHTML5, ParseHtml("doctype HTML"));
  EXPECT_EQ(DocType::kHTML5, ParseHtml("dOcTyPe HtMl"));
}

TEST(DocTypeTest, DetectXhtml5) {
  EXPECT_EQ(DocType::kXHTML5, ParseXhtml("DOCTYPE html"));

  DocType doctype;
  EXPECT_TRUE(doctype.Parse("DOCTYPE html", kContentTypeXml));
  EXPECT_EQ(DocType::kXHTML5, doctype);
}

// CE-HTML (CEA-2014) is XHTML, read by XML parsers: it gets the rules for
// pages that may be parsed as XML, so the HTML5 doctype is the XHTML one.
TEST(DocTypeTest, CeHtmlIsReadAsXml) {
  EXPECT_TRUE(kContentTypeCeHtml.IsXmlLike());
  DocType doctype;
  EXPECT_TRUE(doctype.Parse("DOCTYPE html", kContentTypeCeHtml));
  EXPECT_EQ(DocType::kXHTML5, doctype);
  EXPECT_TRUE(doctype.IsXhtml());
}

TEST(DocTypeTest, DetectHtml4) {
  EXPECT_EQ(DocType::kHTML4Strict,
            ParseHtml("DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
                      "\"http://www.w3.org/TR/html4/strict.dtd\""));
  EXPECT_EQ(
      DocType::kHTML4Transitional,
      ParseHtml(
          "DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" "
          "\"http://www.w3.org/TR/html4/loose.dtd\""));
}

TEST(DocTypeTest, DetectXhtml11) {
  EXPECT_EQ(DocType::kXHTML11,
            ParseXhtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
                       "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\""));
}

TEST(DocTypeTest, DetectXhtml10) {
  EXPECT_EQ(
      DocType::kXHTML10Strict,
      ParseXhtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" "
                 "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\""));
  EXPECT_EQ(
      DocType::kXHTML10Transitional,
      ParseXhtml(
          "DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
          "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\""));
}

TEST(DocTypeTest, DetectVariousXhtmlTypes) {
  // Some of these are listed here:
  //   http://www.w3.org/QA/2002/04/valid-dtd-list.html
  EXPECT_TRUE(ParseXhtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
                         "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\"")
                  .IsXhtml());
  EXPECT_TRUE(
      ParseHtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Frameset//EN\" "
                "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-frameset.dtd\"")
          .IsXhtml());
  EXPECT_TRUE(
      ParseXhtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML+RDFa 1.0//EN\" "
                 "\"http://www.w3.org/MarkUp/DTD/xhtml-rdfa-1.dtd\"")
          .IsXhtml());
  EXPECT_TRUE(
      ParseXhtml(
          "DOCTYPE html PUBLIC "
          "\"-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN\" "
          "\"http://www.w3.org/2002/04/xhtml-math-svg/xhtml-math-svg.dtd\"")
          .IsXhtml());
  EXPECT_TRUE(
      ParseXhtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML Basic 1.1//EN\" "
                 "\"http://www.w3.org/TR/xhtml-basic/xhtml-basic11.dtd\"")
          .IsXhtml());

  EXPECT_FALSE(ParseHtml("DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
                         "\"http://www.w3.org/TR/html4/strict.dtd\"")
                   .IsXhtml());
}

// --- Deviation 1: case-insensitive FPI matching (extension over 1.15) ---

TEST(DocTypeTest, CaseInsensitiveFpi) {
  // Browsers sniff doctypes ASCII case-insensitively; a lowercased FPI must
  // still classify.  1.15 (byte-exact FPI match) returns UNKNOWN here.
  EXPECT_EQ(
      DocType::kXHTML10Strict,
      ParseHtml("DOCTYPE html PUBLIC \"-//w3c//dtd xhtml 1.0 strict//en\" "
                "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\""));
  EXPECT_EQ(DocType::kHTML4Strict,
            ParseHtml("DOCTYPE HTML PUBLIC \"-//w3c//dtd html 4.01//en\" "
                      "\"http://www.w3.org/TR/html4/strict.dtd\""));
  // The lowercase "public" keyword and arbitrary extra whitespace are
  // accepted too (1.15 behavior, kept).
  EXPECT_EQ(
      DocType::kXHTML10Strict,
      ParseHtml("DOCTYPE html public \"-//W3C//DTD XHTML 1.0 Strict//EN\" "
                "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\""));
  EXPECT_EQ(DocType::kHTML4Strict,
            ParseHtml("DOCTYPE  html  PUBLIC  \"-//W3C//DTD HTML 4.01//EN\"  "
                      "\"http://www.w3.org/TR/html4/strict.dtd\""));
}

// --- Deviation 2: about:legacy-compat is an HTML5 doctype ---

TEST(DocTypeTest, AboutLegacyCompat) {
  // The HTML spec's legacy-compat doctype; 1.15 returns UNKNOWN for it.
  EXPECT_EQ(DocType::kHTML5,
            ParseHtml("DOCTYPE html SYSTEM \"about:legacy-compat\""));
  EXPECT_EQ(DocType::kXHTML5,
            ParseXhtml("DOCTYPE html SYSTEM \"about:legacy-compat\""));
}

// --- Battery divergence cases: exact parser degrades to UNKNOWN ---

TEST(DocTypeTest, NotADoctype) {
  DocType doctype;
  EXPECT_FALSE(doctype.Parse("DOCTYPEhtml", kContentTypeHtml));
  EXPECT_FALSE(doctype.Parse("garbage", kContentTypeHtml));
  EXPECT_FALSE(doctype.Parse("", kContentTypeHtml));
  EXPECT_FALSE(doctype.Parse("XYDOCTYPE html", kContentTypeHtml));
  // A bare "DOCTYPE" keyword is a doctype directive with unknown type
  // (matches 1.15).
  EXPECT_TRUE(doctype.Parse("DOCTYPE", kContentTypeHtml));
  EXPECT_EQ(DocType::kUnknown, doctype);
}

TEST(DocTypeTest, UnrecognizedDoctypesDegradeToUnknown) {
  // Every one of these was confidently misclassified by the old
  // substring-heuristic cascade; the exact parser must say UNKNOWN.

  // HTML 4.01 Frameset: no frameset bucket exists.
  EXPECT_EQ(
      DocType::kUnknown,
      ParseHtml("DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Frameset//EN\" "
                "\"http://www.w3.org/TR/html4/frameset.dtd\""));
  // Known FPIs without a system identifier are not matched (exact parser
  // requires the full PUBLIC "FPI" "URL" form).
  EXPECT_EQ(DocType::kUnknown,
            ParseHtml("DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\""));
  EXPECT_EQ(
      DocType::kUnknown,
      ParseHtml(
          "DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\""));
  // Non-W3C XHTML FPI (XHTML Mobile 1.2).
  EXPECT_EQ(
      DocType::kUnknown,
      ParseHtml("DOCTYPE html PUBLIC \"-//WAPFORUM//DTD XHTML Mobile 1.2//EN\" "
                "\"http://www.openmobilealliance.org/tech/DTD/"
                "xhtml-mobile12.dtd\""));
  // HTML 2.0 / HTML 3.2 FPIs.
  EXPECT_EQ(DocType::kUnknown,
            ParseHtml("DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\""));
  EXPECT_EQ(
      DocType::kUnknown,
      ParseHtml("DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\""));
  // Trailing junk, dangling PUBLIC, unbalanced quote, unquoted FPI.
  EXPECT_EQ(DocType::kUnknown, ParseHtml("DOCTYPE html foo"));
  EXPECT_EQ(DocType::kUnknown, ParseHtml("DOCTYPE html PUBLIC"));
  EXPECT_EQ(
      DocType::kUnknown,
      ParseHtml("DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\""));
  EXPECT_EQ(DocType::kUnknown,
            ParseHtml("DOCTYPE html PUBLIC -//W3C//DTD HTML 4.01//EN "
                      "http://www.w3.org/TR/html4/strict.dtd"));
  // Non-html root element.
  EXPECT_EQ(DocType::kUnknown, ParseHtml("DOCTYPE svg"));
  // Unregistered / garbage FPIs: no Strict-from-substring hallucination.
  EXPECT_EQ(
      DocType::kUnknown,
      ParseHtml("DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Strict//EN\""));
  EXPECT_EQ(DocType::kUnknown,
            ParseHtml("DOCTYPE html PUBLIC \"garbage strict garbage\""));
  EXPECT_EQ(DocType::kUnknown, ParseHtml("DOCTYPE html PUBLIC \"garbage\""));
  // "xhtml" as a bare substring must not flip IsXhtml under text/html.
  EXPECT_EQ(DocType::kUnknown,
            ParseHtml("DOCTYPE html PUBLIC \"xhtml-ish garbage\""));
}

TEST(DocTypeTest, FpiIsAuthoritativeOverSystemIdentifier) {
  // Transitional FPI + strict system identifier: the FPI wins.  The old
  // cascade matched "strict" in the URL and overrode the FPI.
  EXPECT_EQ(
      DocType::kXHTML10Transitional,
      ParseHtml(
          "DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
          "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\""));
}

TEST(DocTypeTest, WhitespaceSeparators) {
  EXPECT_EQ(DocType::kHTML5, ParseHtml("DOCTYPE   html"));
  EXPECT_EQ(DocType::kHTML5, ParseHtml("DOCTYPE\thtml"));
  EXPECT_EQ(DocType::kHTML5, ParseHtml("DOCTYPE\nhtml"));
  // Trailing whitespace after "html" still parses as HTML5.
  EXPECT_EQ(DocType::kHTML5, ParseHtml("DOCTYPE html "));
}

TEST(DocTypeTest, SingleQuotedFpi) {
  // ParseShellLikeString treats single quotes like double quotes.
  EXPECT_EQ(DocType::kHTML4Strict,
            ParseHtml("DOCTYPE html PUBLIC '-//W3C//DTD HTML 4.01//EN' "
                      "'http://www.w3.org/TR/html4/strict.dtd'"));
}

}  // namespace
}  // namespace net_instaweb
