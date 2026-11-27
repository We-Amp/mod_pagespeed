// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// directive-match.test.ts — the rule that turns the text typed in the
// directive finder into a name to look for (src/lib/directive-match.ts),
// checked against the names the configuration reference really lists.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import { MODULE_ENTRY, matchesDirective, parseDirectiveQuery } from '../src/lib/directive-match';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const { directives } = JSON.parse(
  readFileSync(resolve(WEBSITE_ROOT, 'src/data/reference/module-directives.json'), 'utf8'),
) as { directives: { name: string }[] };
// The names as the page shows them (and as each block carries its name).
const entries = directives.map((d) => (d.name === 'ModPagespeed' ? MODULE_ENTRY : d.name));
const found = (typed: string) => {
  const query = parseDirectiveQuery(typed);
  return entries.filter((name) => matchesDirective(name, query));
};

describe('parseDirectiveQuery', () => {
  it('is no filter for empty text', () => {
    for (const typed of ['', '   ', ' ; ']) {
      expect(parseDirectiveQuery(typed)).toEqual({ kind: 'none' });
    }
  });

  it('takes the first word, lower-cased, as the name', () => {
    expect(parseDirectiveQuery('FileCache')).toEqual({ kind: 'name', needle: 'filecache' });
    expect(parseDirectiveQuery('  FileCachePath /var/cache')).toEqual({
      kind: 'name',
      needle: 'filecachepath',
    });
  });

  it('drops the Apache and nginx prefixes and the value', () => {
    for (const typed of [
      'ModPagespeedFileCachePath',
      'modpagespeedfilecachepath "/var/cache"',
      'pagespeed FileCachePath /var/cache;',
      'pagespeed   FileCachePath;',
      'PAGESPEED FILECACHEPATH',
    ]) {
      expect(parseDirectiveQuery(typed), typed).toEqual({ kind: 'name', needle: 'filecachepath' });
    }
  });

  it('reads the line that switches the module as the module entry', () => {
    for (const typed of [
      'pagespeed on;',
      'pagespeed on',
      'pagespeed off;',
      'pagespeed standby;',
      'pagespeed unplugged;',
      'ModPagespeed on',
      'ModPagespeed off',
      'ModPagespeed',
      'modpagespeed ',
      'pagespeed ;',
    ]) {
      expect(parseDirectiveQuery(typed), typed).toEqual({ kind: 'module' });
    }
  });

  it('treats "pagespeed" without anything after it as a name', () => {
    expect(parseDirectiveQuery('pagespeed')).toEqual({ kind: 'name', needle: 'pagespeed' });
    // Still typing the prefix: a name, not yet a prefix.
    expect(parseDirectiveQuery('modpagesp')).toEqual({ kind: 'name', needle: 'modpagesp' });
  });

  it('keeps a state word typed on its own as a name', () => {
    expect(parseDirectiveQuery('on')).toEqual({ kind: 'name', needle: 'on' });
  });
});

describe('matching against the reference', () => {
  it('finds exactly the module entry for the lines that switch the module', () => {
    for (const typed of ['pagespeed on;', 'pagespeed on', 'ModPagespeed on', 'ModPagespeed off']) {
      expect(found(typed), typed).toEqual([MODULE_ENTRY]);
    }
  });

  it('finds the module entry for either prefix typed alone', () => {
    expect(found('ModPagespeed')).toEqual([MODULE_ENTRY]);
    expect(found('pagespeed')).toContain(MODULE_ENTRY);
  });

  it('finds one directive for its name in any written form', () => {
    for (const typed of [
      'FileCachePath',
      'filecachepath',
      'ModPagespeedFileCachePath /var/cache/pagespeed',
      'pagespeed FileCachePath /var/cache/pagespeed;',
    ]) {
      expect(found(typed), typed).toEqual(['FileCachePath']);
    }
  });

  it('finds every directive whose name contains the word', () => {
    const hits = found('FileCache');
    expect(hits.length).toBeGreaterThan(1);
    expect(hits.every((name) => name.toLowerCase().includes('filecache'))).toBe(true);
    expect(found('pagespeed EnableFilters rewrite_images;')).toEqual(found('EnableFilters'));
  });

  it('finds nothing for a name the reference does not have', () => {
    expect(found('NoSuchDirective')).toEqual([]);
    expect(found('pagespeed NoSuchDirective on;')).toEqual([]);
  });

  it('shows everything when there is no filter', () => {
    expect(found('').length).toBe(entries.length);
  });
});
