// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Types for scripts/visual-diff.mjs, so the vitest suite in
// test/visual-diff.test.ts imports it with `astro check` none the wiser.
// Keep in sync with the .mjs exports.

export type Viewport = { name: string; width: number; height: number };
export type RawImage = { data: Buffer; width: number; height: number };
export type PixelComparison = {
  width: number;
  height: number;
  total: number;
  sameSize: boolean;
  changed: number;
  maxDelta: number;
  top: number | null;
  bottom: number | null;
  mask: Uint8Array | null;
};
export type DistDiff = {
  empty: boolean;
  baseId: string | null;
  headId: string | null;
  sameBuild: boolean;
  html: {
    compared: number;
    onlyBase: string[];
    onlyHead: string[];
    changed: { file: string; offset: number; base: string; head: string }[];
  };
  css: {
    baseRules: number;
    headRules: number;
    onlyBase: string[];
    onlyHead: string[];
    sheetsOnlyBase: string[];
    sheetsOnlyHead: string[];
  };
};

export const VIEWPORTS: Viewport[];
export const DEFAULT_PATHS: string[];
export const DEFAULT_MASKS: string[];
export function normalizeText(text: string): string;
export function splitCssRules(css: string, context?: string): string[];
export function multisetDiff(a: string[], b: string[]): { onlyA: string[]; onlyB: string[] };
export function distDiff(
  baseInput: string,
  headInput: string,
  options?: { allowSameBuild?: boolean },
): DistDiff;
export type RoundResult = Pick<PixelComparison, 'changed' | 'maxDelta' | 'sameSize'>;
export const SAME_BUILD_MARKER: string;
export const NOISE_MAX_PIXELS: number;
export const NOISE_MAX_DELTA: number;
export function isRasterNoise(cmp: RoundResult): boolean;
export function shouldReshoot(rounds: RoundResult[], maxRounds?: number): boolean;
export function summarizeRounds(
  rounds: RoundResult[],
  kept: number,
): { reshot: boolean; flaky: boolean; note: string | null };
export function comparePixels(a: RawImage, b: RawImage, threshold?: number): PixelComparison;
export function formatPercent(changed: number, total: number): string;
export function slugForPath(path: string): string;
export function freezeSvg(svg: string): string;
export function main(argv: string[]): Promise<number>;
