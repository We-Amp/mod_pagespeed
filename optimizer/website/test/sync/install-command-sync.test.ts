// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// install-command-sync.test.ts — drift guard for the install commands the
// download page shows under its heading. The page repeats commands that other
// pages already carry, so a visitor who copies one from any of them runs the
// same thing:
//
//   1. src/pages/download.astro            — REPOSITORY_INSTALL_COMMAND (the
//                                            Apache and nginx commands) and
//                                            DOCKER_QUICK_TRY_COMMAND
//   2. src/components/home/Install.astro   — the nginx and Apache install
//                                            commands on the home page: the
//                                            repository line, then package
//                                            lines that must each appear
//                                            verbatim on the apt/yum page
//   3. src/pages/download/apt-yum.astro    — the one-line install panel
//   4. src/content/docs/installation-docker.md and getting-started.md
//                                          — the one-container quick try
//
// This guard reads the sources as text. When a command changes, change it in
// every file listed here in the same commit.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';
import * as facts from '../../src/data/product-facts.mjs';

function read(rel: string): string {
  return readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8');
}

function extract(source: string, re: RegExp, label: string): string {
  const m = source.match(re);
  if (!m) {
    throw new Error(
      `install-command-sync: could not find ${label}. ` +
        `If the file moved or the literal changed shape, update this guard.`,
    );
  }
  return m[1];
}

const download = read('../../src/pages/download.astro');
const home = read('../../src/components/home/Install.astro');
// The apt/yum page writes its distro lists from product-facts.mjs
// (`${MODULE_APT_SHORT}` and the like); expand those so the page reads as rendered.
const aptYum = read('../../src/pages/download/apt-yum.astro').replace(
  /\$\{(\w+)\}/g,
  (whole, name: string) => {
    const value = (facts as Record<string, unknown>)[name];
    return typeof value === 'string' ? value : whole;
  },
);

const repositoryCommand = extract(
  download,
  /const REPOSITORY_INSTALL_COMMAND =\s*'([^']+)';/,
  'REPOSITORY_INSTALL_COMMAND in download.astro',
);
const dockerCommand = extract(
  download,
  /const DOCKER_QUICK_TRY_COMMAND = String\.raw`([^`]+)`;/,
  'DOCKER_QUICK_TRY_COMMAND in download.astro',
);

// The first fenced bash block after a heading in a docs page.
function firstBashBlockAfter(rel: string, heading: string): string {
  const source = read(rel);
  const at = source.indexOf(heading);
  if (at < 0) throw new Error(`install-command-sync: no "${heading}" in ${rel}`);
  return extract(source.slice(at), /```bash\n([\s\S]*?)\n```/, `a bash block in ${rel}`);
}

describe('install commands on the download page', () => {
  it('names a platform for each command, Apache and nginx sharing the repository command', () => {
    const platform = (id: string) =>
      extract(
        download,
        new RegExp(`id: '${id}',\\s*label: '[^']+',\\s*command: (\\w+),`),
        `the ${id} install platform in download.astro`,
      );
    expect(platform('apache')).toBe('REPOSITORY_INSTALL_COMMAND');
    expect(platform('nginx')).toBe('REPOSITORY_INSTALL_COMMAND');
    expect(platform('docker')).toBe('DOCKER_QUICK_TRY_COMMAND');
  });

  it('repository command equals the home page install commands', () => {
    const line = extract(
      home,
      /const REPOSITORY_INSTALL_COMMAND =\s*'([^']+)';/,
      'REPOSITORY_INSTALL_COMMAND in components/home/Install.astro',
    );
    expect(repositoryCommand).toBe(line);
    for (const id of ['nginx', 'apache']) {
      expect(
        extract(
          home,
          new RegExp(`id: '${id}',\\s*label: '[^']+',\\s*command: \`\\$\\{(\\w+)\\}\\\\n`),
          `the ${id} install platform in components/home/Install.astro`,
        ),
      ).toBe('REPOSITORY_INSTALL_COMMAND');
    }
  });

  for (const id of ['nginx', 'apache']) {
    it(`the home ${id} package lines match an apt/yum page block, in order`, () => {
      const rest = extract(
        home,
        new RegExp(
          `id: '${id}',\\s*label: '[^']+',\\s*command: \`\\$\\{\\w+\\}((?:\\\\n[^\`\\\\]+)+)\``,
        ),
        `the ${id} package lines in components/home/Install.astro`,
      );
      const lines = rest.split('\\n').filter((line) => line !== '');
      expect(lines.length).toBeGreaterThan(0);
      const aptYumLines = aptYum.split('\n').map((line) => line.trim());
      for (const line of lines) {
        expect(aptYumLines, `"${line}" on the apt/yum page`).toContain(line);
      }
      // Same order and count: the home lines are one whole Debian/Ubuntu block
      // of the apt/yum page (the lines after a "# Debian" comment, up to the
      // blank line), so a swapped or dropped line fails too.
      const debianBlocks: string[][] = [];
      aptYumLines.forEach((line, i) => {
        if (!line.includes('# Debian')) return;
        const block: string[] = [];
        for (let j = i + 1; j < aptYumLines.length && aptYumLines[j] !== ''; j++) {
          block.push(aptYumLines[j]);
        }
        debianBlocks.push(block);
      });
      expect(debianBlocks, 'a Debian/Ubuntu block on the apt/yum page').toContainEqual(lines);
    });
  }

  it('repository command equals the apt/yum page one-liner', () => {
    const line = extract(
      aptYum,
      /text: '([^']+)',\s*tone: 'strong',\s*event: 'cta_download_apt_yum_oneliner'/,
      'the one-line install panel in download/apt-yum.astro',
    );
    expect(repositoryCommand).toBe(line);
  });

  it('Docker command equals the quick try in the Docker guide', () => {
    expect(dockerCommand).toBe(
      firstBashBlockAfter(
        '../../src/content/docs/installation-docker.md',
        '## Quick try (one container)',
      ),
    );
  });

  it('Docker command equals the Docker quick try in the getting-started guide', () => {
    const gettingStarted = read('../../src/content/docs/getting-started.md');
    expect(gettingStarted).toContain('```bash\n' + dockerCommand + '\n```');
  });
});
