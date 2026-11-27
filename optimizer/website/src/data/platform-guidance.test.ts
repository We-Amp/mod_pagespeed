// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { PLATFORM_GUIDANCE, guidanceFor } from './platform-guidance';

describe('platform guidance', () => {
  const entries = Object.entries(PLATFORM_GUIDANCE);

  it('has at least one entry, keyed by a lowercase platform id', () => {
    expect(entries.length).toBeGreaterThan(0);
    for (const [id] of entries) expect(id).toMatch(/^[a-z0-9-]+$/);
  });

  it.each(entries)(
    '%s: one to three actions, each with a source on the vendor domain',
    (_id, g) => {
      expect(g.checked).toMatch(/^\d{4}-\d{2}-\d{2}$/);
      expect(g.actions.length).toBeGreaterThanOrEqual(1);
      expect(g.actions.length).toBeLessThanOrEqual(3);
      for (const action of g.actions) {
        expect(action.text.trim().length).toBeGreaterThan(10);
        expect(action.text.endsWith('.')).toBe(true);
        expect(action.source.startsWith('https://')).toBe(true);
        const host = new URL(action.source).hostname;
        expect(host === g.vendorDomain || host.endsWith('.' + g.vendorDomain)).toBe(true);
        expect(action.sourceLabel.trim()).not.toBe('');
      }
      // Link texts differ, so each link says where it goes.
      expect(new Set(g.actions.map((a) => a.sourceLabel)).size).toBe(g.actions.length);
    },
  );

  it('has no entry for an unknown id', () => {
    for (const id of ['unknown', '', 'toString', 'constructor', '__proto__', null, undefined, 3]) {
      expect(guidanceFor(id)).toBeNull();
    }
    expect(Object.keys(PLATFORM_GUIDANCE)).not.toContain('unknown');
    expect(guidanceFor('wix')).toBe(PLATFORM_GUIDANCE.wix);
  });
});
