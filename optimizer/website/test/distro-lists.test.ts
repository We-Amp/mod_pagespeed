// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// distro-lists.test.ts — a page that lists Debian or Ubuntu releases for the
// native packages ("Debian 12/13", "Ubuntu 22.04/24.04/26.04", "Debian 12 or
// 13") must name exactly the releases the signed apt repository serves, as
// NGINX_APT_DISTROS in src/data/product-facts.mjs records them: none it does
// not serve, and none left out. Install blurbs
// should render the list from the derived NGINX_APT_SHORT constants; this test
// catches a hand-written list that drifts from the data.
//
// Release notes, migration guides and blog posts are dated records and are not
// scanned; neither is the ASP.NET Core sidecar host list, which is written as
// a range ("Debian 11 to 13") and describes a different artifact.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { NGINX_APT_DISTROS, NGINX_APT_SHORT } from '../src/data/product-facts.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const SCAN_DIRS = ['src/pages', 'src/components', 'src/layouts', 'src/content/docs'];
const SKIP = [/\/release-notes[^/]*$/, /\/migrat[^/]*$/];
const EXTENSIONS = new Set(['.astro', '.md', '.mdx', '.ts', '.mjs']);

const SUPPORTED = new Map<string, Set<string>>();
for (const row of NGINX_APT_DISTROS) {
  const [name, version] = row.distro.split(' ');
  if (!SUPPORTED.has(name)) SUPPORTED.set(name, new Set());
  SUPPORTED.get(name)!.add(version);
}

const SPACE = String.raw`(?:\s|&nbsp;| )`;
const SEP = String.raw`(?:/|,${SPACE}|${SPACE}or${SPACE})`;
const LIST_PATTERNS = [
  new RegExp(String.raw`\b(Debian)${SPACE}(\d+(?:${SEP}\d+)+)\b`, 'g'),
  new RegExp(String.raw`\b(Ubuntu)${SPACE}(\d\d\.\d\d(?:${SEP}\d\d\.\d\d)+)\b`, 'g'),
];

/**
 * Every problem with the release lists in `text`: a listed release the data
 * does not support, or a supported release the list leaves out.
 */
function listProblems(text: string): string[] {
  const bad: string[] = [];
  for (const pattern of LIST_PATTERNS) {
    for (const m of text.matchAll(pattern)) {
      const [, name, list] = m;
      const versions = list.split(new RegExp(SEP));
      const supported = SUPPORTED.get(name) ?? new Set<string>();
      for (const v of versions) {
        if (!supported.has(v)) bad.push(`${name} ${v} not supported in "${m[0]}"`);
      }
      for (const v of supported) {
        if (!versions.includes(v)) bad.push(`${name} ${v} missing from "${m[0]}"`);
      }
    }
  }
  return bad;
}

function walk(dir: string, out: string[] = []): string[] {
  for (const entry of readdirSync(dir)) {
    const full = path.join(dir, entry);
    if (statSync(full).isDirectory()) walk(full, out);
    else if (EXTENSIONS.has(path.extname(entry))) out.push(full);
  }
  return out;
}

describe('native package platform lists', () => {
  it('flags an unsupported or a missing release, and passes the derived list', () => {
    expect(listProblems('Debian&nbsp;11/12/13, Ubuntu&nbsp;22.04/24.04/26.04')).toEqual([
      'Debian 11 not supported in "Debian&nbsp;11/12/13"',
    ]);
    expect(listProblems('Ubuntu 22.04/24.04')).toEqual([
      'Ubuntu 26.04 missing from "Ubuntu 22.04/24.04"',
    ]);
    expect(listProblems('Debian 11 or 12')).toEqual([
      'Debian 11 not supported in "Debian 11 or 12"',
      'Debian 13 missing from "Debian 11 or 12"',
    ]);
    expect(listProblems('Ubuntu 22.04, 24.04 or 26.04')).toEqual([]);
    expect(listProblems(NGINX_APT_SHORT)).toEqual([]);
    expect(listProblems('Debian 11 (bullseye) stays on 1.15.0')).toEqual([]);
    expect(listProblems('Debian 11 to 13 and Ubuntu 20.04 to 26.04')).toEqual([]);
  });

  it('pages list exactly the releases the signed apt repository serves', () => {
    const failures: string[] = [];
    for (const dir of SCAN_DIRS) {
      for (const file of walk(path.join(WEBSITE_ROOT, dir))) {
        const rel = path.relative(WEBSITE_ROOT, file).split(path.sep).join('/');
        if (SKIP.some((re) => re.test(rel))) continue;
        readFileSync(file, 'utf8')
          .split('\n')
          .forEach((line, i) => {
            for (const bad of listProblems(line)) failures.push(`${rel}:${i + 1}: ${bad}`);
          });
      }
    }
    expect(failures).toEqual([]);
  });
});
