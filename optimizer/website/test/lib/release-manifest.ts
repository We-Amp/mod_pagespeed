// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Reads the real 2.1 release manifest once, for tests that stand in for
// `getRelease` (which needs astro:content, available only inside an Astro
// build). Kept out of `test/**/*.test.ts` so vitest never runs it as a test.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { parse as parseYaml } from 'yaml';

type ReleaseManifest = {
  release: { semver: string; tag: string; released_on: string };
};

export const releaseManifest = parseYaml(
  readFileSync(
    fileURLToPath(new URL('../../src/content/releases-2.1/release.yaml', import.meta.url)),
    'utf8',
  ),
) as ReleaseManifest;
