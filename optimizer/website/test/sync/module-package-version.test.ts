// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// module-package-version.test.ts — MODULE_PACKAGE_VERSION in
// src/data/product-facts.mjs (the version the homepage quick-start shows in
// the module's response header) must equal the newest module packages in the
// published package index. The live comparison needs the network; without it
// the comparison is skipped and only the parser checks run.

import { describe, it, expect } from 'vitest';
import {
  MODULE_PACKAGE_VERSION,
  NGINX_APT_DISTROS,
  PKG_APACHE_MODULE,
  PKG_NGINX_MODULE,
} from '../../src/data/product-facts.mjs';
import { expectOrWarn } from '../lib/release-window';

const INDEX = 'https://packages.modpagespeed.com/apt';

/** The upstream part of a Debian version ("1.17.0-r1~noble" -> "1.17.0"),
 *  or null for a pre-release ("1.15.0~beta.1-r0~jammy") or another shape. */
function upstreamVersion(debVersion: string): string | null {
  const m = /^(\d+)\.(\d+)\.(\d+)-/.exec(debVersion);
  return m ? `${m[1]}.${m[2]}.${m[3]}` : null;
}

function compare(a: string, b: string): number {
  const pa = a.split('.').map(Number);
  const pb = b.split('.').map(Number);
  for (let i = 0; i < 3; i++) if (pa[i] !== pb[i]) return pa[i] - pb[i];
  return 0;
}

/** The newest upstream version of `pkg` in an apt Packages file. */
function newestInPackages(packagesText: string, pkg: string): string | null {
  let newest: string | null = null;
  for (const stanza of packagesText.split(/\n\s*\n/)) {
    const name = /^Package: (.+)$/m.exec(stanza)?.[1].trim();
    const version = /^Version: (.+)$/m.exec(stanza)?.[1].trim();
    if (name !== pkg || !version) continue;
    const upstream = upstreamVersion(version);
    if (upstream && (!newest || compare(upstream, newest) > 0)) newest = upstream;
  }
  return newest;
}

describe('package index parsing', () => {
  it('takes the upstream version and skips pre-releases', () => {
    expect(upstreamVersion('1.17.0-r1')).toBe('1.17.0');
    expect(upstreamVersion('1.17.0-r1~noble')).toBe('1.17.0');
    expect(upstreamVersion('1.15.0~beta.1-r0~jammy')).toBeNull();
  });

  it('picks the numerically newest version of the named package', () => {
    const text = [
      'Package: mod-pagespeed\nVersion: 1.9.0-r1',
      'Package: mod-pagespeed\nVersion: 1.17.0-r1',
      'Package: mod-pagespeed\nVersion: 1.16.0-r3',
      'Package: other\nVersion: 9.0.0-r1',
    ].join('\n\n');
    expect(newestInPackages(text, 'mod-pagespeed')).toBe('1.17.0');
    expect(newestInPackages(text, 'missing')).toBeNull();
  });
});

describe('MODULE_PACKAGE_VERSION', () => {
  it('is a plain x.y.z version', () => {
    expect(MODULE_PACKAGE_VERSION).toMatch(/^\d+\.\d+\.\d+$/);
  });

  it('equals the newest module packages in the live package index', async (ctx) => {
    // Suites as apt names them: the lowercase codename in the matrix label.
    const suites = NGINX_APT_DISTROS.map((d: { distro: string }) =>
      d.distro.split(' ').pop()!.toLowerCase(),
    );
    const texts: string[] = [];
    try {
      for (const suite of suites) {
        const res = await fetch(`${INDEX}/dists/${suite}/main/binary-amd64/Packages`, {
          signal: AbortSignal.timeout(10000),
        });
        if (!res.ok) throw new Error(`${suite}: HTTP ${res.status}`);
        texts.push(await res.text());
      }
    } catch {
      ctx.skip(); // no network: nothing to compare against
      return;
    }
    for (const pkg of [PKG_APACHE_MODULE, PKG_NGINX_MODULE]) {
      const newest = texts
        .map((t) => newestInPackages(t, pkg))
        .filter((v): v is string => v !== null)
        .sort(compare)
        .pop();
      // A release publishes the packages before the sync pull request moves
      // MODULE_PACKAGE_VERSION, so in a release window the index being ahead
      // is a warning (test/lib/release-window.ts); elsewhere it fails.
      expectOrWarn(`${pkg} in ${suites.join(', ')}`, () => {
        expect(newest, `${pkg} in ${suites.join(', ')}`).toBe(MODULE_PACKAGE_VERSION);
      });
    }
  }, 60000);
});
