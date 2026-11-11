// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** WCAG 2 contrast ratio between two "#rrggbb" colours (1 to 21). */
export function contrastRatio(foreground: string, background: string): number {
  const a = luminance(foreground);
  const b = luminance(background);
  const [hi, lo] = a > b ? [a, b] : [b, a];
  return (hi + 0.05) / (lo + 0.05);
}

/** The linear-light red, green and blue (0 to 1) of a "#rrggbb" colour. */
function linearRgb(hex: string): [number, number, number] {
  const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex.trim());
  if (m === null) throw new Error(`not a #rrggbb colour: ${hex}`);
  const [r, g, b] = [m[1], m[2], m[3]].map((h) => {
    const c = parseInt(h, 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return [r, g, b];
}

function luminance(hex: string): number {
  const [r, g, b] = linearRgb(hex);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** How a colour is seen: unimpaired, or with no working green or red cones. */
export type Vision = "normal" | "deuteranope" | "protanope";

// Machado, Oliveira and Fernandes (2009), severity 1.0, applied to linear RGB.
const SIMULATION: Record<Exclude<Vision, "normal">, readonly (readonly number[])[]> = {
  deuteranope: [
    [0.367322, 0.860646, -0.227968],
    [0.280085, 0.672501, 0.047413],
    [-0.01182, 0.04294, 0.968881],
  ],
  protanope: [
    [0.152286, 1.052583, -0.204868],
    [0.114503, 0.786281, 0.099216],
    [-0.003882, -0.048116, 1.051998],
  ],
};

/** CIE L*a*b* (D65) of a colour as the given vision sees it. */
function lab(hex: string, vision: Vision): [number, number, number] {
  let rgb = linearRgb(hex);
  if (vision !== "normal") {
    const m = SIMULATION[vision];
    const seen = m.map((row) => Math.min(1, Math.max(0, row[0] * rgb[0] + row[1] * rgb[1] + row[2] * rgb[2])));
    rgb = [seen[0], seen[1], seen[2]];
  }
  const [r, g, b] = rgb;
  const xyz = [
    (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047,
    0.2126729 * r + 0.7151522 * g + 0.072175 * b,
    (0.0193339 * r + 0.119192 * g + 0.9503041 * b) / 1.08883,
  ];
  const [fx, fy, fz] = xyz.map((t) => (t > 216 / 24389 ? Math.cbrt(t) : ((24389 / 27) * t + 16) / 116));
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}

/**
 * How far apart two "#rrggbb" colours look: the CIE76 distance (0 is the
 * same colour; about 2 is just noticeable; under about 20, two thin lines
 * read as one colour), for unimpaired vision or under a simulated
 * colour-vision deficiency.
 */
export function colorDistance(a: string, b: string, vision: Vision = "normal"): number {
  const [p, q] = [lab(a, vision), lab(b, vision)];
  return Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
}

/**
 * The colour tokens of theme.css: the first `:root { … }` block (light) and
 * the `:root` block inside `@media (prefers-color-scheme: dark)` (dark wins
 * over light where it overrides).
 */
export function themeTokens(css: string): { light: Record<string, string>; dark: Record<string, string> } {
  const light = declarations(/:root\s*\{([^}]*)\}/.exec(css)?.[1] ?? "");
  const darkBlock = /@media\s*\(prefers-color-scheme:\s*dark\)\s*\{\s*:root\s*\{([^}]*)\}/.exec(css)?.[1] ?? "";
  return { light, dark: { ...light, ...declarations(darkBlock) } };
}

function declarations(block: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const m of block.matchAll(/(--ps-[\w-]+)\s*:\s*(#[0-9a-fA-F]{6})\s*;/g)) out[m[1]] = m[2];
  return out;
}
