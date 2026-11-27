// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Types for scripts/check-built-content.mjs (the built-output content lint),
// so the vitest suite in test/check-built-content.test.ts imports it with
// `astro check` none the wiser. Keep in sync with the .mjs exports.

export type RuleLevel = 'error' | 'warn';
export type RuleConfig = { level: RuleLevel; indexableOnly: boolean };
export type RuleName =
  | 'title-count'
  | 'title-length'
  | 'description-present'
  | 'description-length'
  | 'description-duplicate'
  | 'h1-count'
  | 'document-structure'
  | 'term-drift-daemon'
  | 'product-naming'
  | 'retired-product-label'
  | 'canonical';
export type Failure = { url: string; rule: string; level: RuleLevel; message: string };
export type Page = { url: string; html: string };
export type TokenKind = 'open' | 'close' | 'comment' | 'doctype';
export type Token = {
  kind: TokenKind;
  name: string;
  attrs: string;
  start: number;
  end: number;
  unterminated?: boolean;
};

export const RULES: Record<RuleName, RuleConfig>;
export function decodeEntities(text: string): string;
export function collapseWhitespace(text: string): string;
export function tokens(html: string): Token[];
export function titleInfo(html: string): { count: number; text: string };
export function descriptionInfo(html: string): { count: number; text: string };
export function isNoindex(html: string): boolean;
export function isRedirectStub(html: string): boolean;
export function stripNonVisible(html: string): string;
export function visibleText(html: string): string;
export function h1Count(html: string): number;
export function documentStructureProblems(html: string): string[];
export function canonicalHrefs(html: string): string[];
export function canonicalProblem(href: string): string | null;
export function lintPage(html: string): { rule: RuleName; message: string; count?: number }[];
export function lintPages(
  pages: Page[],
  allowlist?: Record<string, Record<string, string>>,
  pending?: Record<string, number>,
): { failures: Failure[]; warnings: Failure[] };
export function urlOf(distRoot: string, file: string): string;
export function collectPages(distRoot: string): string[];
export function loadAllowlist(file: string): Record<string, Record<string, string>>;
export function loadPending(file: string): Record<string, number>;
export function labelSurfaces(html: string): [string, string][];
