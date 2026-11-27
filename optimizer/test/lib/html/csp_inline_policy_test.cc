// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the CSP inline-injection policy helper.

#include "lib/html/csp_inline_policy.h"

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

// ============================================================================
// No policy / absent directive -> inline allowed
// ============================================================================

TEST(CspInlinePolicyTest, EmptyCspAllowsInline) {
  EXPECT_TRUE(InlineStyleAllowed(""));
  EXPECT_TRUE(InlineScriptAllowed(""));
}

TEST(CspInlinePolicyTest, UnrelatedDirectivesAllowInline) {
  // No style-src*/default-src governs inline style -> allowed.
  EXPECT_TRUE(InlineStyleAllowed("img-src 'self'; font-src 'self'"));
  // No script-src*/default-src governs inline script -> allowed.
  EXPECT_TRUE(InlineScriptAllowed("img-src 'self'; style-src 'self'"));
}

// ============================================================================
// Directive present, no 'unsafe-inline' -> blocked
// ============================================================================

TEST(CspInlinePolicyTest, DefaultSrcSelfBlocksBoth) {
  EXPECT_FALSE(InlineStyleAllowed("default-src 'self'"));
  EXPECT_FALSE(InlineScriptAllowed("default-src 'self'"));
}

TEST(CspInlinePolicyTest, StyleSrcSelfBlocksStyle) {
  EXPECT_FALSE(InlineStyleAllowed("style-src 'self'"));
}

TEST(CspInlinePolicyTest, ScriptSrcSelfBlocksScript) {
  EXPECT_FALSE(InlineScriptAllowed("script-src 'self'"));
}

// ============================================================================
// 'unsafe-inline' present, no neutralizing nonce/hash -> allowed
// ============================================================================

TEST(CspInlinePolicyTest, StyleSrcUnsafeInlineAllowsStyle) {
  EXPECT_TRUE(InlineStyleAllowed("style-src 'self' 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, ScriptSrcUnsafeInlineAllowsScript) {
  EXPECT_TRUE(InlineScriptAllowed("script-src 'self' 'unsafe-inline'"));
}

// ============================================================================
// 'unsafe-inline' neutralized by nonce/hash -> blocked
// ============================================================================

TEST(CspInlinePolicyTest, NonceNeutralizesUnsafeInlineStyle) {
  EXPECT_FALSE(InlineStyleAllowed("style-src 'unsafe-inline' 'nonce-abc'"));
}

TEST(CspInlinePolicyTest, HashNeutralizesUnsafeInlineStyle) {
  EXPECT_FALSE(InlineStyleAllowed(
      "style-src 'unsafe-inline' 'sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='"));
}

TEST(CspInlinePolicyTest, NonceNeutralizesUnsafeInlineScript) {
  EXPECT_FALSE(InlineScriptAllowed("script-src 'unsafe-inline' 'nonce-xyz'"));
}

// ============================================================================
// CSP3 fallback chain resolution
// ============================================================================

TEST(CspInlinePolicyTest, ScriptFallsBackToDefaultSrc) {
  // No script-src*, so default-src governs -> blocked.
  EXPECT_FALSE(InlineScriptAllowed("default-src 'self'; img-src *"));
  // default-src permits inline -> allowed.
  EXPECT_TRUE(InlineScriptAllowed("default-src 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, StyleFallsBackToDefaultSrc) {
  EXPECT_FALSE(InlineStyleAllowed("default-src 'self'"));
  EXPECT_TRUE(InlineStyleAllowed("default-src 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, StyleSrcElemTakesPrecedenceOverStyleSrc) {
  // style-src-elem is the most specific: its permissive value wins even though
  // style-src is restrictive.
  EXPECT_TRUE(
      InlineStyleAllowed("style-src 'self'; style-src-elem 'unsafe-inline'"));
  // And the reverse: a restrictive style-src-elem overrides a permissive
  // style-src.
  EXPECT_FALSE(
      InlineStyleAllowed("style-src 'unsafe-inline'; style-src-elem 'self'"));
}

TEST(CspInlinePolicyTest, ScriptSrcTakesPrecedenceOverDefaultSrc) {
  // script-src (mid) overrides default-src (fallback).
  EXPECT_TRUE(
      InlineScriptAllowed("default-src 'self'; script-src 'unsafe-inline'"));
  EXPECT_FALSE(
      InlineScriptAllowed("default-src 'unsafe-inline'; script-src 'self'"));
}

// ============================================================================
// Whitespace / casing tolerance
// ============================================================================

TEST(CspInlinePolicyTest, ToleratesWhitespaceAndTrailingSemicolon) {
  EXPECT_TRUE(
      InlineStyleAllowed("  style-src   'self'   'unsafe-inline'  ;  "));
  EXPECT_FALSE(InlineStyleAllowed(";;  style-src 'self' ;;"));
}

TEST(CspInlinePolicyTest, DirectiveNameCaseInsensitive) {
  EXPECT_FALSE(InlineStyleAllowed("STYLE-SRC 'self'"));
  EXPECT_TRUE(InlineScriptAllowed("Script-Src 'unsafe-inline'"));
}

// ============================================================================
// 'strict-dynamic' — neutralizes unsafe-inline for SCRIPT only, inert for STYLE
// ============================================================================

TEST(CspInlinePolicyTest, StrictDynamicNeutralizesUnsafeInlineScript) {
  // strict-dynamic makes the browser ignore 'unsafe-inline' for scripts, so a
  // nonce-less inline script we inject is dropped.
  EXPECT_FALSE(
      InlineScriptAllowed("script-src 'strict-dynamic' 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, StrictDynamicInertForStyle) {
  // strict-dynamic is meaningless for style-src, so 'unsafe-inline' still wins.
  EXPECT_TRUE(InlineStyleAllowed("style-src 'strict-dynamic' 'unsafe-inline'"));
}

// ============================================================================
// Duplicate directive — first occurrence wins (spec: later dupes ignored)
// ============================================================================

TEST(CspInlinePolicyTest, DuplicateDirectiveFirstWinsRestrictive) {
  // First style-src is restrictive; the later permissive dupe is ignored.
  EXPECT_FALSE(
      InlineStyleAllowed("style-src 'self'; style-src 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, DuplicateDirectiveFirstWinsPermissive) {
  // First style-src permits inline; the later restrictive dupe is ignored.
  EXPECT_TRUE(
      InlineStyleAllowed("style-src 'unsafe-inline'; style-src 'self'"));
}

// ============================================================================
// Comma-separated policy list — every member must allow (header-list
// semantics; browsers enforce each member even when delivered in one <meta>)
// ============================================================================

TEST(CspInlinePolicyTest, PolicyListRestrictiveMemberBlocksStyle) {
  // Evaluated as one policy, the second policy's 'unsafe-inline' would be
  // swallowed into the first policy's restrictive style-src source list and
  // the whole string would fail open. Each member must be evaluated alone,
  // and the restrictive first member must win.
  EXPECT_FALSE(
      InlineStyleAllowed("style-src 'none', style-src 'unsafe-inline'"));
  // Order-independent: restrictive member last also blocks.
  EXPECT_FALSE(
      InlineStyleAllowed("style-src 'unsafe-inline', style-src 'none'"));
}

TEST(CspInlinePolicyTest, PolicyListRestrictiveMemberBlocksScript) {
  EXPECT_FALSE(
      InlineScriptAllowed("script-src 'unsafe-inline', default-src 'self'"));
  // Script-side fail-open pin (mirrors the style-side case): parsed as ONE
  // policy, the later member's 'unsafe-inline' would fail open.
  EXPECT_FALSE(
      InlineScriptAllowed("script-src 'none', script-src 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, PolicyListAllPermissiveMembersAllow) {
  EXPECT_TRUE(InlineStyleAllowed(
      "style-src 'unsafe-inline', default-src 'unsafe-inline'"));
}

TEST(CspInlinePolicyTest, PolicyListMemberWithoutGoverningDirectiveIsInert) {
  // The img-src member does not govern inline style, so only the style-src
  // member decides.
  EXPECT_TRUE(InlineStyleAllowed("img-src 'self', style-src 'unsafe-inline'"));
  EXPECT_FALSE(InlineStyleAllowed("img-src 'self', style-src 'self'"));
}

TEST(CspInlinePolicyTest, PolicyListToleratesEmptyMembersAndWhitespace) {
  // Trailing comma / empty members are no-policy members -> inert.
  EXPECT_TRUE(InlineStyleAllowed("style-src 'unsafe-inline',"));
  EXPECT_FALSE(InlineStyleAllowed(" , style-src 'self' , "));
}

}  // namespace
}  // namespace net_instaweb
