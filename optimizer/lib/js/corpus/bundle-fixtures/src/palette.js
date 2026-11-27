// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export const DEFAULTS = Object.freeze({ accent: "#ff6600", contrast: 4.5 });

// Deliberately class-free, with a plain function property instead of method
// shorthand: class bodies and object-method shorthand are both "unmodelable"
// for the minifier under test (known residuals K4/N3), and either one would
// make every bundle built from these sources pass through byte-preserved —
// masking the bundler shapes (runtime boilerplate, banners, sourcemap
// comments) this fixture exists to push THROUGH the minifier.
export function createPalette(accent = DEFAULTS.accent) {
  return {
    mix: function (other) {
      if (typeof other !== "string" || !other.startsWith("#")) return null;
      return { hex: `${accent}+${other}`, ratio: DEFAULTS.contrast };
    },
  };
}
