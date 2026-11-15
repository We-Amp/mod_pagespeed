// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import {
  absolutizeLinks,
  artifactUrl,
  dockerTag,
  docMarkdownUrl,
  docUrl,
  groupDocs,
  nugetCommand,
  renderDocMarkdown,
  substituteRelease,
} from '../../src/lib/docs-markdown.mjs';

// Hand-made manifests in the shape of src/content/releases-2.1/release.yaml.
// The real manifest is deliberately not read, so a release bump cannot break
// these tests.
type ReleaseFields = { semver?: string; revision?: number | null; tag?: string };

const release = (fields: ReleaseFields = {}) => ({
  product: { line: '2.1' },
  release: { semver: '2.8.1', revision: 7, tag: 'v2.8.1', ...fields },
  artifacts: {
    docker: { image: 'ghcr.io/we-amp/pagespeed-nginx', tag_template: '%V' },
    deb: {
      amd64: 'pool/main/p/pagespeed/pagespeed_%V_r%R_amd64.deb',
      arm64: 'pool/main/p/pagespeed/pagespeed_%T_arm64.deb',
    },
    msi: {
      win_x64:
        'https://github.com/We-Amp/mod_pagespeed/releases/download/module-v%V/pagespeed-iis-%V-win-x64.msi',
    },
  },
  urls: { archive_base: 'https://packages.modpagespeed.com/' },
});

/** Manifests whose three lines carry distinguishable NuGet values. */
const nugetManifests = (pkg: string, flags: string) => {
  const line = (p: string, f: string) => ({
    ...release(),
    artifacts: { nuget: { package: p } },
    display: { nuget_install_flags: f },
  });
  return {
    '1.1': line(`${pkg}.old`, `${flags} 1.1`),
    '2.0': line(pkg, flags),
    '2.1': line(`${pkg}.new`, `${flags} 2.1`),
  };
};

describe('substituteRelease', () => {
  it('substitutes %V, %R and %T', () => {
    expect(substituteRelease('pagespeed_%V_r%R_%T', release())).toBe('pagespeed_2.8.1_r7_v2.8.1');
  });

  it('renders a missing revision as 0', () => {
    expect(substituteRelease('%R', release({ revision: null }))).toBe('0');
    expect(substituteRelease('%R', release({ revision: undefined }))).toBe('0');
  });

  it('returns a template with no placeholders unchanged', () => {
    expect(substituteRelease('pagespeed-legacy.deb', release())).toBe('pagespeed-legacy.deb');
  });
});

describe('dockerTag', () => {
  it('substitutes the docker tag template', () => {
    expect(dockerTag(release())).toBe('2.8.1');
  });

  it('renders a compound template', () => {
    const rel = release();
    rel.artifacts.docker.tag_template = '%T-slim';
    expect(dockerTag(rel)).toBe('v2.8.1-slim');
  });

  it('falls back to the bare version without a docker block', () => {
    const rel = release();
    expect(dockerTag({ ...rel, artifacts: { ...rel.artifacts, docker: undefined } })).toBe('2.8.1');
  });
});

describe('artifactUrl', () => {
  it('substitutes %V and %R into a relative template and joins it to archive_base', () => {
    expect(artifactUrl(release(), 'deb', 'amd64')).toBe(
      'https://packages.modpagespeed.com/pool/main/p/pagespeed/pagespeed_2.8.1_r7_amd64.deb',
    );
  });

  it('substitutes %T', () => {
    expect(artifactUrl(release(), 'deb', 'arm64')).toBe(
      'https://packages.modpagespeed.com/pool/main/p/pagespeed/pagespeed_v2.8.1_arm64.deb',
    );
  });

  it('renders a missing revision as 0', () => {
    expect(artifactUrl(release({ revision: null }), 'deb', 'amd64')).toBe(
      'https://packages.modpagespeed.com/pool/main/p/pagespeed/pagespeed_2.8.1_r0_amd64.deb',
    );
  });

  it('returns an absolute https template as-is, without archive_base', () => {
    expect(artifactUrl(release(), 'msi', 'win_x64')).toBe(
      'https://github.com/We-Amp/mod_pagespeed/releases/download/module-v2.8.1/pagespeed-iis-2.8.1-win-x64.msi',
    );
  });

  it('substitutes an explicit version for %V (a module-only update)', () => {
    expect(artifactUrl(release(), 'msi', 'win_x64', '2.8.2')).toBe(
      'https://github.com/We-Amp/mod_pagespeed/releases/download/module-v2.8.2/pagespeed-iis-2.8.2-win-x64.msi',
    );
  });

  it('throws for a channel the manifest does not carry', () => {
    expect(() => artifactUrl(release(), 'rpm', 'amd64')).toThrowError(
      'No artifact template for channel=rpm arch=amd64 in the 2.1 manifest',
    );
  });

  it('throws for an arch the channel does not carry', () => {
    expect(() => artifactUrl(release(), 'deb', 'i386')).toThrowError(
      'No artifact template for channel=deb arch=i386 in the 2.1 manifest',
    );
  });
});

describe('nugetCommand', () => {
  it('reads the package and the install flags from the 2.0 manifest', () => {
    expect(nugetCommand(nugetManifests('WeAmp.PageSpeed.AspNetCore', '--prerelease'))).toBe(
      'dotnet add package WeAmp.PageSpeed.AspNetCore --prerelease',
    );
  });

  it('falls back to the shared package name and drops blank flags', () => {
    const m = {
      '1.1': release(),
      '2.0': { ...release(), artifacts: {}, display: { nuget_install_flags: '   ' } },
      '2.1': release(),
    };
    expect(nugetCommand(m)).toBe('dotnet add package WeAmp.PageSpeed.AspNetCore');
  });
});

describe('docUrl', () => {
  it('builds the canonical page URL', () => {
    expect(docUrl('install')).toBe('https://modpagespeed.com/docs/install/');
  });
});

describe('docMarkdownUrl', () => {
  it('builds the markdown route URL', () => {
    expect(docMarkdownUrl('install')).toBe('https://modpagespeed.com/docs/install.md');
  });
});

describe('absolutizeLinks', () => {
  const pageUrl = 'https://modpagespeed.com/docs/configuration/';

  it('rewrites a root-relative target to the site', () => {
    expect(absolutizeLinks('See [the guide](/docs/guide/).', pageUrl)).toBe(
      'See [the guide](https://modpagespeed.com/docs/guide/).',
    );
  });

  it('rewrites a fragment-only target to the page URL', () => {
    expect(absolutizeLinks('Jump to [the flags](#flags).', pageUrl)).toBe(
      'Jump to [the flags](https://modpagespeed.com/docs/configuration/#flags).',
    );
  });

  it('rewrites both kinds on one line', () => {
    expect(absolutizeLinks('From [here](/start/) see [the end](#end).', pageUrl)).toBe(
      'From [here](https://modpagespeed.com/start/) see [the end](https://modpagespeed.com/docs/configuration/#end).',
    );
  });

  it('leaves an absolute URL alone', () => {
    const line = 'See [a](https://a.example) and [b](https://b.example) for details.';
    expect(absolutizeLinks(line, pageUrl)).toBe(line);
  });

  it('leaves a relative target without a leading slash alone', () => {
    expect(absolutizeLinks('Read [the next page](next-page/).', pageUrl)).toBe(
      'Read [the next page](next-page/).',
    );
  });

  it('leaves text that only looks like a link alone', () => {
    const line = 'Paths appear as (/docs/guide/) and /docs/guide/ in prose.';
    expect(absolutizeLinks(line, pageUrl)).toBe(line);
  });
});

describe('groupDocs', () => {
  const doc = (slug: string, group?: string) => ({
    slug,
    file: `${slug}.md`,
    ext: 'md' as const,
    data: { title: slug, description: '', order: 0, ...(group ? { group } : {}) },
    body: '',
  });

  it('merges consecutive pages of the same group', () => {
    const a = doc('install', 'Get started');
    const b = doc('quickstart', 'Get started');
    const c = doc('directives', 'Reference');
    expect(groupDocs([a, b, c])).toEqual([
      { label: 'Get started', docs: [a, b] },
      { label: 'Reference', docs: [c] },
    ]);
  });

  it('labels pages without a group Other', () => {
    const rows = groupDocs([doc('about'), doc('install', 'Get started'), doc('faq')]);
    expect(rows.map((g) => g.label)).toEqual(['Other', 'Get started', 'Other']);
  });

  it('starts a new group when the same label comes back after another group', () => {
    const a = doc('one', 'Get started');
    const b = doc('two', 'Reference');
    const c = doc('three', 'Get started');
    expect(groupDocs([a, b, c])).toEqual([
      { label: 'Get started', docs: [a] },
      { label: 'Reference', docs: [b] },
      { label: 'Get started', docs: [c] },
    ]);
  });

  it('returns no groups for an empty collection', () => {
    expect(groupDocs([])).toEqual([]);
  });
});

describe('renderDocMarkdown callouts', () => {
  const render = (body: string, ext: 'md' | 'mdx' = 'md') =>
    renderDocMarkdown(
      {
        slug: 'page',
        file: 'page.md',
        ext,
        body,
        data: { title: 'Page', description: 'About.', order: 1 },
      },
      { '1.1': release(), '2.0': release(), '2.1': release() },
    );

  it('renders a next-release marker as a titled blockquote, without its after', () => {
    for (const ext of ['md', 'mdx'] as const) {
      const md = render(
        ['Before.', '', ':::next-release{after="2.8.2"}', 'New.', ':::', '', 'After.'].join('\n'),
        ext,
      );
      expect(md).toContain('Before.\n\n> **From the next release**\n>\n> New.\n\nAfter.');
      expect(md).not.toContain('after=');
    }
  });

  it('takes a [label] over the next-release title', () => {
    expect(render(':::next-release[Coming]{after="2.8.2"}\nNew.\n:::')).toContain('> **Coming**');
  });
});
