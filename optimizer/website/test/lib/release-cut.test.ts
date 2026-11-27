// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { releaseCutInFlight } from './release-cut';

const base = {
  version: '2.9.0',
  rowRelease: '2.8.2',
  rowHelmChart: '0.3.15',
  chartVersion: '0.3.16',
  chartAppVersion: '2.9.0',
};

describe('releaseCutInFlight', () => {
  it('is true when the chart and VERSION.txt are ahead of the site row', () => {
    expect(releaseCutInFlight(base)).toBe(true);
  });
  it('is false when the chart appVersion disagrees with VERSION.txt', () => {
    expect(releaseCutInFlight({ ...base, chartAppVersion: '2.8.2' })).toBe(false);
    expect(releaseCutInFlight({ ...base, chartAppVersion: '2.9.1' })).toBe(false);
  });
  it('is false when the row is at VERSION.txt (no cut in flight)', () => {
    expect(releaseCutInFlight({ ...base, rowRelease: '2.9.0', chartAppVersion: '2.9.0' })).toBe(
      false,
    );
  });
  it('is false when the row is ahead of VERSION.txt', () => {
    expect(releaseCutInFlight({ ...base, rowRelease: '2.9.1' })).toBe(false);
  });
  it('is false when the chart version is behind the row', () => {
    expect(releaseCutInFlight({ ...base, chartVersion: '0.3.14' })).toBe(false);
  });
  it('is false when the chart version equals the row (stale chart)', () => {
    expect(releaseCutInFlight({ ...base, chartVersion: '0.3.15' })).toBe(false);
  });
  it('is false when the row has no chart (cannot be judged)', () => {
    expect(releaseCutInFlight({ ...base, rowHelmChart: null })).toBe(false);
  });
  it('compares numerically, not as strings', () => {
    expect(releaseCutInFlight({ ...base, version: '2.10.0', chartAppVersion: '2.10.0' })).toBe(
      true,
    );
    expect(releaseCutInFlight({ ...base, chartVersion: '0.3.9', rowHelmChart: '0.3.10' })).toBe(
      false,
    );
  });
});
