// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import eslintPluginAstro from 'eslint-plugin-astro';

export default [
  { ignores: ['.astro/**', 'dist/**'] },
  ...eslintPluginAstro.configs.recommended,
  {
    rules: {
      'no-unused-vars': ['warn', { argsIgnorePattern: '^_' }],
      'no-console': 'warn',
    },
  },
];
