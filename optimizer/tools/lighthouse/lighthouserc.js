// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Lighthouse CI configuration for mod_pagespeed.
 *
 * Runs against the nginx proxy (port 8080) which serves optimized content.
 * Used by `lhci autorun` for CI pipelines and local validation.
 *
 * Assertions enforce minimum quality bars for optimized pages.
 */
module.exports = {
  ci: {
    collect: {
      url: [
        "http://localhost:8080/index.html",
        "http://localhost:8080/style.css",
      ],
      numberOfRuns: 3,
      settings: {
        formFactor: "mobile",
        throttlingMethod: "simulate",
        onlyCategories: ["performance"],
        budgetPath: "./budget.json",
      },
    },
    assert: {
      assertions: {
        "categories:performance": ["warn", { minScore: 0.7 }],
        "render-blocking-resources": ["warn", { maxLength: 1 }],
        "unminified-css": ["error", { maxLength: 0 }],
        "unminified-javascript": ["error", { maxLength: 0 }],
        "uses-webp-images": ["warn", { maxLength: 0 }],
        "largest-contentful-paint": ["warn", { maxNumericValue: 4000 }],
        "cumulative-layout-shift": ["warn", { maxNumericValue: 0.25 }],
      },
    },
    upload: {
      target: "filesystem",
      outputDir: "./.lighthouseci",
    },
  },
};
