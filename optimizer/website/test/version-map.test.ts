// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The version map (src/data/version-map.mjs) is hand-kept, one row per
// release. Its first row is the current release, so it must agree with the
// sources that already carry that release: the release manifest, the Helm
// chart and the module package version. A release that forgets to add its row
// fails here.

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { parse as parseYaml } from 'yaml';
import { MODULE_PACKAGE_VERSION, SIDECAR_VERSION } from '../src/data/product-facts.mjs';
import { VERSION_MAP, versionMapMarkdown } from '../src/data/version-map.mjs';
import { releaseCutInFlight } from './lib/release-cut';
import { releaseManifest } from './lib/release-manifest';
import { expectOrWarn, inReleaseWindow } from './lib/release-window';
import { artifactVersionProblems } from './lib/version-rules';

const chart = parseYaml(
  readFileSync(new URL('../../deploy/helm/pagespeed/Chart.yaml', import.meta.url), 'utf8'),
);

const VERSION = readFileSync(new URL('../../VERSION.txt', import.meta.url), 'utf8').trim();

describe('version map', () => {
  const current = VERSION_MAP[0];

  it('starts with the release in the manifest', () => {
    expect(current.release).toBe(releaseManifest.release.semver);
    expect(current.date).toBe(String(releaseManifest.release.released_on));
  });

  it('gives the current release the chart and package versions that ship it', () => {
    // The chart is bumped by the release lane's own pull request
    // (release/helm-*) before the site's version map follows, so in a release
    // window a mismatch here is a warning (test/lib/release-window.ts).
    // The appVersion stays on the release in a module-only release too.
    // A release cut in flight (chart appVersion = VERSION.txt, row still on
    // the previous release) warns as well: the site's row follows with the
    // release's site PR (test/lib/release-cut.ts). Committed files only.
    const cutInFlight = releaseCutInFlight({
      version: VERSION,
      rowRelease: current.release,
      rowHelmChart: current.helmChart,
      chartVersion: String(chart.version),
      chartAppVersion: String(chart.appVersion),
    });
    if (cutInFlight) {
      console.warn(
        `::warning::release cut ${VERSION} in flight: the site's row follows with the release's site PR`,
      );
    }
    expectOrWarn(
      'version map row vs Helm chart',
      () => {
        expect(current.helmChart).toBe(String(chart.version));
        expect(
          artifactVersionProblems({ ...current, chartAppVersion: String(chart.appVersion) }),
        ).toEqual([]);
      },
      inReleaseWindow() || cutInFlight,
    );
    // Two committed files, not live state: strict in a window too.
    expect(current.packages).toBe(MODULE_PACKAGE_VERSION);
  });

  it('lists releases newest first, each once, with ISO dates', () => {
    const releases = VERSION_MAP.map((r) => r.release);
    expect(new Set(releases).size).toBe(releases.length);
    const dates = VERSION_MAP.map((r) => r.date);
    for (const d of dates) expect(d).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    expect([...dates].sort().reverse()).toEqual(dates);
  });

  it('carries one number on every artifact from 2.5.0 on, except the chart', () => {
    // A module-only release (2.8.2) publishes new module packages and the IIS
    // installer without new images, chart or NuGet packages, so the packages
    // may lead the release on its line; they never trail it, and everything
    // else stays on it (test/lib/version-rules.ts).
    for (const r of VERSION_MAP.filter((row) => /^2\.(?:[5-9]|\d{2,})\./.test(row.release))) {
      expect(artifactVersionProblems(r), r.release).toEqual([]);
      expect(r.iisInstaller, r.release).toBe(true);
    }
  });

  it('keeps the images and the NuGet packages of the current release on the release', () => {
    expect(current.images).toBe(releaseManifest.release.semver);
    expect(current.nuget).toBe(releaseManifest.release.semver);
    expect(SIDECAR_VERSION).toBe(releaseManifest.release.semver);
    expect(artifactVersionProblems({ ...current, sidecar: SIDECAR_VERSION })).toEqual([]);
  });

  it('renders a Markdown table with one row per release', () => {
    const md = versionMapMarkdown();
    const rows = md.split('\n').filter((l) => l.startsWith('| '));
    expect(rows).toHaveLength(VERSION_MAP.length + 2);
    expect(md).toContain(`pagespeed-iis-${current.packages}-win-x64.msi`);
  });
});

describe('module-only release rules', () => {
  const release = { release: '2.8.1', packages: '2.8.1', images: '2.8.1', nuget: '2.8.1' };
  const full = { ...release, chartAppVersion: '2.8.1', sidecar: '2.8.1' };

  it('accepts one number everywhere, and module packages a patch or more ahead', () => {
    expect(artifactVersionProblems(full)).toEqual([]);
    expect(artifactVersionProblems({ ...full, packages: '2.8.2' })).toEqual([]);
    expect(artifactVersionProblems({ ...full, packages: '2.8.3' })).toEqual([]);
  });

  it('rejects module packages that trail the release', () => {
    expect(artifactVersionProblems({ ...full, packages: '2.8.0' })).toEqual([
      '2.8.1: packages 2.8.0 must not trail the release',
    ]);
  });

  it('rejects module packages on another line', () => {
    expect(artifactVersionProblems({ ...full, packages: '2.9.0' })).toEqual([
      '2.8.1: packages 2.9.0 must stay on the 2.8 line',
    ]);
    expect(artifactVersionProblems({ ...full, packages: '3.8.1' })).toEqual([
      '2.8.1: packages 3.8.1 must stay on the 2.8 line',
    ]);
    expect(artifactVersionProblems({ ...full, packages: '2.8' })).toEqual([
      '2.8.1: packages 2.8 is not x.y.z',
    ]);
  });

  it('rejects images, NuGet, the Sidecar or the chart at the module version', () => {
    const m = { ...full, packages: '2.8.2' };
    expect(artifactVersionProblems({ ...m, images: '2.8.2' })).toEqual([
      '2.8.1: images 2.8.2 must equal the release',
    ]);
    expect(artifactVersionProblems({ ...m, nuget: '2.8.2' })).toEqual([
      '2.8.1: nuget 2.8.2 must equal the release',
    ]);
    expect(artifactVersionProblems({ ...m, sidecar: '2.8.2' })).toEqual([
      '2.8.1: sidecar 2.8.2 must equal the release',
    ]);
    expect(artifactVersionProblems({ ...m, chartAppVersion: '2.8.2' })).toEqual([
      '2.8.1: chart appVersion 2.8.2 must equal the release',
    ]);
  });
});

describe('optimizer-only release rules', () => {
  // A paired release 2.9.0 and an optimizer release 2.9.1 that moves only the
  // optimizer host packages.
  const paired = {
    release: '2.9.0',
    packages: '2.9.0',
    images: '2.9.0',
    nuget: '2.9.0',
    chartAppVersion: '2.9.0',
    sidecar: '2.9.0',
  };
  const optOnly = { ...paired, optimizer: '2.9.1' };

  it('accepts an optimizer a patch or more ahead when everything else is on the paired release', () => {
    expect(artifactVersionProblems(optOnly)).toEqual([]);
    expect(artifactVersionProblems({ ...paired, optimizer: '2.9.4' })).toEqual([]);
    expect(artifactVersionProblems({ ...paired, optimizer: '2.9.0' })).toEqual([]);
  });

  it('leaves rows without an optimizer to the existing rules', () => {
    expect(artifactVersionProblems(paired)).toEqual([]);
    expect(artifactVersionProblems({ ...paired, packages: '2.9.1' })).toEqual([]);
  });

  it('rejects an optimizer that trails the release', () => {
    expect(artifactVersionProblems({ ...paired, optimizer: '2.8.9' })).toEqual([
      '2.9.0: optimizer 2.8.9 must stay on the 2.9 line',
    ]);
    expect(
      artifactVersionProblems({
        ...paired,
        release: '2.9.1',
        packages: '2.9.1',
        images: '2.9.1',
        nuget: '2.9.1',
        chartAppVersion: '2.9.1',
        sidecar: '2.9.1',
        optimizer: '2.9.0',
      }),
    ).toEqual(['2.9.1: optimizer 2.9.0 must not trail the release']);
  });

  it('rejects an optimizer on another line or not in x.y.z form', () => {
    expect(artifactVersionProblems({ ...paired, optimizer: '2.10.0' })).toEqual([
      '2.9.0: optimizer 2.10.0 must stay on the 2.9 line',
    ]);
    expect(artifactVersionProblems({ ...paired, optimizer: '3.9.1' })).toEqual([
      '2.9.0: optimizer 3.9.1 must stay on the 2.9 line',
    ]);
    expect(artifactVersionProblems({ ...paired, optimizer: '2.9' })).toEqual([
      '2.9.0: optimizer 2.9 is not x.y.z',
    ]);
  });

  it('applies only while the module, images, NuGet, Sidecar and chart all equal the paired release', () => {
    expect(artifactVersionProblems({ ...optOnly, packages: '2.9.1' })).toEqual([
      '2.9.0: packages 2.9.1 must equal the release when the optimizer leads',
    ]);
    expect(artifactVersionProblems({ ...optOnly, images: '2.9.1' })).toEqual([
      '2.9.0: images 2.9.1 must equal the release',
    ]);
    expect(artifactVersionProblems({ ...optOnly, nuget: '2.9.1' })).toEqual([
      '2.9.0: nuget 2.9.1 must equal the release',
    ]);
    expect(artifactVersionProblems({ ...optOnly, sidecar: '2.9.1' })).toEqual([
      '2.9.0: sidecar 2.9.1 must equal the release',
    ]);
    expect(artifactVersionProblems({ ...optOnly, chartAppVersion: '2.9.1' })).toEqual([
      '2.9.0: chart appVersion 2.9.1 must equal the release',
    ]);
  });

  it('does not let the module and the optimizer both lead', () => {
    expect(artifactVersionProblems({ ...optOnly, packages: '2.9.2' })).toEqual([
      '2.9.0: packages 2.9.2 must equal the release when the optimizer leads',
    ]);
  });
});
