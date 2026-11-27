// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The release-window tolerance of the live-state guards: which runs count as a
// release window (scripts/release-window.sh), how the version-drift guard
// behaves in and out of one (scripts/check-version-drift.sh), and the helper
// the live-state unit tests use (test/lib/release-window.ts). The shell
// scripts run against fake `git` and `gh` binaries, so nothing here needs the
// network.

import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { afterAll, describe, expect, it, vi } from 'vitest';
import { expectOrWarn, inReleaseWindow } from './lib/release-window';

const SCRIPTS = new URL('../scripts/', import.meta.url).pathname;
const tmp = mkdtempSync(path.join(tmpdir(), 'release-window-'));
afterAll(() => rmSync(tmp, { recursive: true, force: true }));

/**
 * A bin dir with fake `git` and `gh` (see below). `git ls-remote` prints TAGS
 * for mod_pagespeed, and OPTIMIZER_TAGS (default: none) for this repository.
 */
function fakeBin(
  name: string,
  opts: { tags?: string[]; optimizerTags?: string[]; gh?: string },
): string {
  const bin = path.join(tmp, name, 'bin');
  mkdirSync(bin, { recursive: true });
  const list = (tags: string[] = []) => tags.map((t) => `0000\trefs/tags/${t}`).join('\n');
  writeFileSync(
    path.join(bin, 'git'),
    `#!/bin/sh\ncase "$*" in\n*pagespeed-optimizer*) cat <<'EOT'\n${list(opts.optimizerTags)}\nEOT\n;;\n*) cat <<'EOT'\n${list(opts.tags)}\nEOT\n;;\nesac\n`,
  );
  writeFileSync(path.join(bin, 'gh'), `#!/bin/sh\n${opts.gh ?? 'exit 1'}\n`);
  chmodSync(path.join(bin, 'git'), 0o755);
  chmodSync(path.join(bin, 'gh'), 0o755);
  return bin;
}

function run(script: string, env: Record<string, string>, bin: string, cwd = tmp) {
  const r = spawnSync('bash', [path.join(SCRIPTS, script)], {
    cwd,
    encoding: 'utf8',
    env: { PATH: `${bin}:${process.env.PATH}`, HOME: tmp, ...env },
  });
  return { code: r.status, out: `${r.stdout}${r.stderr}` };
}

describe('release-window.sh', () => {
  const bin = fakeBin('rw', {
    gh: `case "$*" in *pulls/7*) echo "release/helm-0.3.14 We-Amp/pagespeed-optimizer";; *pulls/8*) echo "feature/x We-Amp/pagespeed-optimizer";; *pulls/9*) echo "release/y someone/fork";; *) exit 1;; esac`,
  });
  const base = { GITHUB_REPOSITORY: 'We-Amp/pagespeed-optimizer' };
  const win = (env: Record<string, string>) =>
    run('release-window.sh', { ...base, ...env }, bin).out.includes('release_window=true');

  it('is a window for the sync pull request and release/* pull requests from this repository', () => {
    const same = { EVENT_NAME: 'pull_request', HEAD_REPO: base.GITHUB_REPOSITORY };
    expect(win({ ...same, HEAD_REF: 'auto/sync-release-manifests' })).toBe(true);
    expect(win({ ...same, HEAD_REF: 'release/helm-0.3.14' })).toBe(true);
  });

  it('is not a window for ordinary branches, lookalikes, forks or pushes', () => {
    const same = { EVENT_NAME: 'pull_request', HEAD_REPO: base.GITHUB_REPOSITORY };
    expect(win({ ...same, HEAD_REF: 'docs/blog-2-8-release' })).toBe(false);
    expect(win({ ...same, HEAD_REF: 'releases/x' })).toBe(false);
    expect(win({ ...same, HEAD_REF: 'auto/sync-release-manifests-2' })).toBe(false);
    expect(
      win({ EVENT_NAME: 'pull_request', HEAD_REF: 'release/x', HEAD_REPO: 'someone/fork' }),
    ).toBe(false);
    expect(win({ EVENT_NAME: 'pull_request', HEAD_REF: 'release/x' })).toBe(false);
    // A push to main, or a push to a branch that merely has a release name.
    expect(
      win({ EVENT_NAME: 'push', HEAD_REF: 'release/x', HEAD_REPO: base.GITHUB_REPOSITORY }),
    ).toBe(false);
  });

  it('resolves a merge queue entry through its pull request, and fails closed', () => {
    const q = (n: number) => ({
      EVENT_NAME: 'merge_group',
      REF_NAME: `gh-readonly-queue/main/pr-${n}-abc123`,
    });
    expect(win(q(7))).toBe(true);
    expect(win(q(8))).toBe(false);
    expect(win(q(9))).toBe(false); // fork head
    const failed = run('release-window.sh', { ...base, ...q(10) }, bin);
    expect(failed.out).toContain('release_window=false'); // lookup fails
    expect(failed.out).toContain('::warning::release window: could not resolve');
    expect(win({ EVENT_NAME: 'merge_group', REF_NAME: 'something-else' })).toBe(false);
  });

  it('writes RELEASE_WINDOW to GITHUB_ENV', () => {
    const f = path.join(tmp, 'github_env');
    writeFileSync(f, '');
    run(
      'release-window.sh',
      {
        ...base,
        EVENT_NAME: 'pull_request',
        HEAD_REF: 'release/a',
        HEAD_REPO: base.GITHUB_REPOSITORY,
        GITHUB_ENV: f,
      },
      bin,
    );
    run('release-window.sh', { ...base, EVENT_NAME: 'push', GITHUB_ENV: f }, bin);
    run(
      'release-window.sh',
      {
        ...base,
        EVENT_NAME: 'pull_request',
        HEAD_REF: 'auto/sync-release-manifests',
        HEAD_REPO: base.GITHUB_REPOSITORY,
        GITHUB_ENV: f,
      },
      bin,
    );
    expect(readFileSync(f, 'utf8')).toBe(
      'RELEASE_WINDOW=1\nRELEASE_SYNC=0\nRELEASE_WINDOW=0\nRELEASE_SYNC=0\nRELEASE_WINDOW=1\nRELEASE_SYNC=1\n',
    );
  });
});

describe('check-version-drift.sh', () => {
  /** A minimal tree: the manifest and one docs page holding an image literal. */
  function tree(name: string, semver: string, docTag: string, modulePackages?: string): string {
    const root = path.join(tmp, name, 'tree');
    mkdirSync(path.join(root, 'website/src/content/releases-2.1'), { recursive: true });
    mkdirSync(path.join(root, 'website/src/content/docs'), { recursive: true });
    for (const d of ['pages', 'components', 'layouts'])
      mkdirSync(path.join(root, 'website/src', d), { recursive: true });
    writeFileSync(
      path.join(root, 'website/src/content/releases-2.1/release.yaml'),
      `release:\n  semver: "${semver}"\n`,
    );
    writeFileSync(
      path.join(root, 'website/src/content/docs/install.md'),
      `docker pull ghcr.io/we-amp/pagespeed-worker:${docTag}\n`,
    );
    if (modulePackages) {
      mkdirSync(path.join(root, 'website/src/data'), { recursive: true });
      writeFileSync(
        path.join(root, 'website/src/data/product-facts.mjs'),
        `export const MODULE_PACKAGE_VERSION = '${modulePackages}';\n`,
      );
    }
    return root;
  }
  const old = new Date(Date.now() - 48 * 3600e3).toISOString().replace(/\.\d+Z$/, 'Z');
  const ghTagged = `case "$*" in *git/refs/tags*) echo '{"object":{"type":"commit","sha":"abc"}}';; *git/commits*) echo '${old}';; *) exit 1;; esac`;
  const bin = fakeBin('drift', { tags: ['v2.7.0', 'v2.8.0'], gh: ghTagged });
  const go = (root: string, window: string, sync = '0') =>
    run('check-version-drift.sh', { ROOT: root, RELEASE_WINDOW: window, RELEASE_SYNC: sync }, bin);

  it('passes when everything agrees', () => {
    const r = go(tree('ok', '2.8.0', '2.8.0'), '0');
    expect(r.code).toBe(0);
    expect(r.out).toContain('version-drift guard OK');
  });

  it('fails a stale image tag everywhere except the sync pull request, which only warns', () => {
    const root = tree('stale', '2.8.0', '2.7.0');
    const strict = go(root, '0');
    expect(strict.code).toBe(1);
    expect(strict.out).toContain('::error::stale image tag(s)');
    // A release/* window is not enough: committed files, not live state.
    const releaseBranch = go(root, '1', '0');
    expect(releaseBranch.code).toBe(1);
    expect(releaseBranch.out).toContain('::error::stale image tag(s)');
    const tolerant = go(root, '1', '1');
    expect(tolerant.code).toBe(0);
    expect(tolerant.out).toContain('::warning::stale image tag(s)');
    expect(tolerant.out).toContain('release window');
    expect(tolerant.out).not.toContain('::error::');
  });

  it('fails a manifest that lags an old tag outside a release window, warns inside one', () => {
    const root = tree('lag', '2.7.0', '2.7.0');
    expect(go(root, '0').code).toBe(1);
    const tolerant = go(root, '1');
    expect(tolerant.code).toBe(0);
    expect(tolerant.out).toContain('::warning::the 2.1 manifest (2.7.0) lags');
  });

  it('keeps the existing grace window: a fresh tag warns even outside a release window', () => {
    const fresh = new Date(Date.now() - 3600e3).toISOString().replace(/\.\d+Z$/, 'Z');
    const b = fakeBin('fresh', {
      tags: ['v2.8.0'],
      gh: `case "$*" in *git/refs/tags*) echo '{"object":{"type":"commit","sha":"abc"}}';; *git/commits*) echo '${fresh}';; *) exit 1;; esac`,
    });
    const r = run(
      'check-version-drift.sh',
      { ROOT: tree('grace', '2.7.0', '2.7.0'), RELEASE_WINDOW: '0' },
      b,
    );
    expect(r.code).toBe(0);
    expect(r.out).toContain('within the 6h grace window');
  });

  describe('a module-only release', () => {
    // mod_pagespeed tagged v2.8.2; this repository has no v2.8.2 tag, so no
    // new images, chart or NuGet packages: the manifest stays 2.8.1.
    const modBin = fakeBin('module-only', {
      tags: ['v2.8.0', 'v2.8.1', 'v2.8.2'],
      optimizerTags: ['v2.8.1'],
      gh: ghTagged,
    });
    const check = (root: string, b = modBin) =>
      run('check-version-drift.sh', { ROOT: root, RELEASE_WINDOW: '0' }, b);

    it('passes when MODULE_PACKAGE_VERSION names the newer tag', () => {
      const r = check(tree('mo-ok', '2.8.1', '2.8.1', '2.8.2'));
      expect(r.code).toBe(0);
      expect(r.out).toContain('module-only release: v2.8.2');
      expect(r.out).toContain('version-drift guard OK');
      expect(r.out).not.toContain('::error::');
    });

    it('still fails an image literal at the module version', () => {
      const r = check(tree('mo-image', '2.8.1', '2.8.2', '2.8.2'));
      expect(r.code).toBe(1);
      expect(r.out).toContain('::error::stale image tag(s)');
      expect(r.out).toContain('must be :2.8.1');
    });

    it('fails when the site has not moved MODULE_PACKAGE_VERSION', () => {
      const r = check(tree('mo-stale', '2.8.1', '2.8.1', '2.8.1'));
      expect(r.code).toBe(1);
      expect(r.out).toContain('::error::the 2.1 manifest (2.8.1) lags');
      expect(r.out).toContain('MODULE_PACKAGE_VERSION');
    });

    it('is not a module-only release when this repository released the version', () => {
      const released = fakeBin('mo-released', {
        tags: ['v2.8.1', 'v2.8.2'],
        optimizerTags: ['v2.8.1', 'v2.8.2'],
        gh: ghTagged,
      });
      const r = check(tree('mo-released', '2.8.1', '2.8.1', '2.8.2'), released);
      expect(r.code).toBe(1);
      expect(r.out).toContain('::error::the 2.1 manifest (2.8.1) lags');
    });

    it('is not a module-only release on another line', () => {
      const minor = fakeBin('mo-minor', {
        tags: ['v2.8.1', 'v2.9.0'],
        optimizerTags: ['v2.8.1'],
        gh: ghTagged,
      });
      const r = check(tree('mo-minor', '2.8.1', '2.8.1', '2.9.0'), minor);
      expect(r.code).toBe(1);
      expect(r.out).toContain('lags the latest stable tag v2.9.0');
    });
  });

  it('still fails what is not release state, even inside a release window', () => {
    // No stable tag to compare with, or no readable manifest: these are not
    // release-state findings, so the window does not soften them.
    const noTags = fakeBin('notags', { tags: [], gh: 'exit 1' });
    const r = run(
      'check-version-drift.sh',
      { ROOT: tree('notags', '2.8.0', '2.8.0'), RELEASE_WINDOW: '1' },
      noTags,
    );
    expect(r.code).toBe(1);
    expect(r.out).toContain('no stable v2.* tag');
    const empty = path.join(tmp, 'empty-tree');
    mkdirSync(path.join(empty, 'website/src/content/releases-2.1'), { recursive: true });
    writeFileSync(
      path.join(empty, 'website/src/content/releases-2.1/release.yaml'),
      'release: {}\n',
    );
    expect(go(empty, '1').code).toBe(1);
  });
});

describe('expectOrWarn', () => {
  it('fails outside a release window and warns inside one', () => {
    expect(() => expectOrWarn('x', () => expect(1).toBe(2), false)).toThrow();
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    expect(() => expectOrWarn('x', () => expect(1).toBe(2), true)).not.toThrow();
    expect(warn).toHaveBeenCalledWith(expect.stringContaining('::warning::x'));
    warn.mockRestore();
  });

  it('reads the window from RELEASE_WINDOW=1 only', () => {
    expect(inReleaseWindow({ RELEASE_WINDOW: '1' })).toBe(true);
    expect(inReleaseWindow({ RELEASE_WINDOW: '0' })).toBe(false);
    expect(inReleaseWindow({})).toBe(false);
  });
});
