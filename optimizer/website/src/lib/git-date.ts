// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Build-time "last updated" dates from git history.
 *
 * `gitLastModified(file)` returns the committer date (YYYY-MM-DD) of the last
 * commit that touched `file`, or null when git cannot answer honestly: no git
 * binary, the file is not inside a work tree (an exported tarball), or the
 * clone is shallow (a depth-1 checkout reports the HEAD date for every file,
 * which would be a wrong answer presented as a right one). Callers fall back
 * to the frontmatter `lastUpdated` in that case.
 *
 * Server-side only: this runs inside `astro build` / `astro dev`, never in the
 * browser. Results are cached per file for the lifetime of the process.
 */
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';

const dateCache = new Map<string, string | null>();
let historyUsable: boolean | null = null;

function git(args: string[], cwd: string): string {
  return execFileSync('git', args, {
    cwd,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'ignore'],
    timeout: 10_000,
    // Room for a whole data file's content (`gitFileHistory`).
    maxBuffer: 64 * 1024 * 1024,
  }).trim();
}

function canUseHistory(cwd: string): boolean {
  if (historyUsable !== null) return historyUsable;
  try {
    historyUsable =
      git(['rev-parse', '--is-inside-work-tree'], cwd) === 'true' &&
      git(['rev-parse', '--is-shallow-repository'], cwd) !== 'true';
  } catch {
    historyUsable = false;
  }
  return historyUsable;
}

/**
 * @param filePath Path to a tracked file, absolute or relative to the
 *   process working directory (the site root during a build).
 * @returns The last commit date as YYYY-MM-DD, or null when unavailable.
 */
export function gitLastModified(filePath: string): string | null {
  const abs = resolve(process.cwd(), filePath);
  const cached = dateCache.get(abs);
  if (cached !== undefined) return cached;

  let date: string | null = null;
  if (existsSync(abs) && canUseHistory(dirname(abs))) {
    try {
      const out = git(['log', '-1', '--format=%cs', '--', abs], dirname(abs));
      if (/^\d{4}-\d{2}-\d{2}$/.test(out)) date = out;
    } catch {
      date = null;
    }
  }
  dateCache.set(abs, date);
  return date;
}

/**
 * Every committed version of `filePath`, newest first, as `{ date, text }`
 * (committer date YYYY-MM-DD and the file's content at that commit). Null
 * under the same conditions as `gitLastModified` (no git, not a work tree, a
 * shallow clone), or when git fails. Renames are not followed.
 *
 * @param filePath Path to a tracked file, absolute or relative to the
 *   process working directory.
 */
export function gitFileHistory(filePath: string): Array<{ date: string; text: string }> | null {
  const abs = resolve(process.cwd(), filePath);
  const cwd = dirname(abs);
  if (!existsSync(abs) || !canUseHistory(cwd)) return null;
  try {
    const rel = git(['ls-files', '--full-name', '--', abs], cwd);
    if (!rel) return null;
    const log = git(['log', '--format=%H %cs', '--', abs], cwd);
    return log
      .split('\n')
      .filter(Boolean)
      .map((line) => {
        const [sha, date] = line.split(' ');
        return { date, text: git(['show', `${sha}:${rel}`], cwd) };
      });
  } catch {
    return null;
  }
}
