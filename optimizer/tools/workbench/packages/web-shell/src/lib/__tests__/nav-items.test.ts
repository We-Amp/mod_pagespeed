// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { navItems } from '../nav-items';

describe('navItems (left-nav config)', () => {
  it('does not expose nav entries for incomplete console views', () => {
    // These three console views exist as routes but are intentionally
    // hidden from the left nav until their UX is finalized. If you are
    // re-introducing one, remove it from this guard.
    const hiddenLabels = ['Waterfall', 'Visual Diff', 'Logs'];
    const hiddenHrefs = ['/waterfall', '/diff', '/logs'];

    for (const label of hiddenLabels) {
      expect(
        navItems.find((item) => item.label === label),
        `nav entry "${label}" must not appear in the left nav`,
      ).toBeUndefined();
    }
    for (const href of hiddenHrefs) {
      expect(
        navItems.find((item) => item.href === href),
        `nav entry with href "${href}" must not appear in the left nav`,
      ).toBeUndefined();
    }
  });

  it('does not expose a License entry', () => {
    // The console has no license management view: the product is Apache-2.0 licensed
    // and the worker exposes no license endpoints. Guard against a stale
    // nav entry pointing at a route that no longer exists.
    expect(navItems.find((item) => item.label === 'License')).toBeUndefined();
    expect(navItems.find((item) => item.href === '/license')).toBeUndefined();
  });

  it('still exposes the core console entries', () => {
    const labels = navItems.map((item) => item.label);
    expect(labels).toContain('Dashboard');
    expect(labels).toContain('Configuration');
    expect(labels).toContain('URLs');
    expect(labels).toContain('Metrics');
    expect(labels).toContain('About');
  });
});
