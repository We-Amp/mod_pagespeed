// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import {
  CHECKING,
  NOT_MEASURED,
  aireadStatus,
  healthLine,
  riskStatus,
  speedStatus,
} from './status';

describe('speedStatus', () => {
  it('buckets on the mobile score', () => {
    expect(speedStatus(90, 40)).toMatchObject({ state: 'good', word: 'Good' });
    expect(speedStatus(89, 99).state).toBe('needs-work');
    expect(speedStatus(50, 99).state).toBe('needs-work');
    expect(speedStatus(49, 99)).toMatchObject({ state: 'poor', word: 'Poor' });
  });
  it('falls back to desktop and reports what it has', () => {
    expect(speedStatus(null, 95)).toMatchObject({ state: 'good', value: 'Mobile — · Desktop 95' });
    expect(speedStatus(72, 91).value).toBe('Mobile 72 · Desktop 91');
    expect(speedStatus(null, 40)).toMatchObject({ state: 'poor', value: 'Mobile — · Desktop 40' });
    expect(speedStatus(undefined, 50).state).toBe('needs-work');
    expect(speedStatus(null, null)).toEqual(NOT_MEASURED);
  });
});

describe('aireadStatus', () => {
  it('maps grades', () => {
    expect(aireadStatus({ grade: 'B', score: 81 })).toMatchObject({
      state: 'good',
      value: 'Grade B · 81/100',
    });
    expect(aireadStatus({ grade: 'C', score: 64 }).state).toBe('needs-work');
    expect(aireadStatus({ grade: 'F', score: 10 }).state).toBe('poor');
    expect(aireadStatus(null)).toEqual(NOT_MEASURED);
  });
});

describe('riskStatus', () => {
  it('counts flagged lenses among the measured ones', () => {
    const r = riskStatus({
      preConsentLeak: { status: 'ok', verdict: 'leaks' },
      scriptInventory: { status: 'ok', verdict: 'clean' },
      seoDefects: { status: 'ok', verdict: 'unknown' },
      responseExposure: { status: 'blocked' },
    });
    expect(r).toMatchObject({
      state: 'flagged',
      word: '1 flagged',
      value: '1 check needs attention',
    });
  });
  it('is clean when nothing is flagged and not measured when nothing is measured', () => {
    expect(riskStatus({ scriptInventory: { status: 'ok', verdict: 'clean' } }).state).toBe('clean');
    expect(riskStatus({ scriptInventory: { status: 'error' } })).toEqual(NOT_MEASURED);
  });
});

describe('riskStatus: k and n', () => {
  const ok = (verdict: string) => ({ status: 'ok', verdict });
  it('shrinks k when a lens is disabled or missing', () => {
    const r = riskStatus({
      preConsentLeak: ok('clean'),
      scriptInventory: { status: 'disabled', verdict: 'unknown' },
      seoDefects: ok('attention'),
    });
    expect(r.value).toBe('1 check needs attention');
  });
  it('excludes a verdict of unknown, a missing verdict and non-ok statuses from k', () => {
    const r = riskStatus({
      preConsentLeak: ok('unknown'),
      scriptInventory: { status: 'ok' },
      seoDefects: { status: 'blocked', verdict: 'attention' },
      responseExposure: ok('attention'),
    });
    expect(r).toMatchObject({
      state: 'flagged',
      word: '1 flagged',
      value: '1 check needs attention',
    });
  });
  it('is not measured when every lens is blocked, errored, disabled or unknown', () => {
    expect(
      riskStatus({
        preConsentLeak: { status: 'blocked', verdict: 'unknown' },
        scriptInventory: { status: 'error', reason: 'x' },
        seoDefects: { status: 'disabled', verdict: 'unknown' },
        responseExposure: ok('unknown'),
      }),
    ).toEqual(NOT_MEASURED);
    expect(riskStatus({})).toEqual(NOT_MEASURED);
  });
  it('counts n across all four lenses', () => {
    const r = riskStatus({
      preConsentLeak: ok('leaks'),
      scriptInventory: ok('attention'),
      seoDefects: ok('attention'),
      responseExposure: ok('clean'),
    });
    expect(r).toMatchObject({ word: '3 flagged', value: '3 checks need attention' });
  });
  it('treats a leaks verdict only as flagged for the pre-consent lens', () => {
    expect(riskStatus({ scriptInventory: ok('leaks') }).state).toBe('clean');
  });
});

describe('healthLine', () => {
  const good = speedStatus(95, 95);
  const poor = speedStatus(20, 20);
  it('counts pending areas while checking', () => {
    expect(healthLine([CHECKING, good, CHECKING])).toBe('Checking 2 of 3 areas…');
  });
  it('summarises attention among measured areas', () => {
    expect(healthLine([good, good, good])).toBe('None of the 3 areas checked needs attention.');
    expect(healthLine([poor, good, NOT_MEASURED])).toBe('1 of 2 areas checked needs attention.');
    expect(healthLine([poor, NOT_MEASURED, NOT_MEASURED])).toBe(
      'The one area checked needs attention.',
    );
    expect(healthLine([poor, poor, good])).toBe('2 of 3 areas checked need attention.');
    expect(healthLine([poor, poor, NOT_MEASURED])).toBe('2 of 2 areas checked need attention.');
    expect(healthLine([poor, good, good])).toBe('1 of 3 areas checked needs attention.');
    expect(healthLine([poor, poor, poor])).toBe('3 of 3 areas checked need attention.');
    expect(healthLine([NOT_MEASURED, NOT_MEASURED, NOT_MEASURED])).toBe(
      'We could not check this page.',
    );
  });

  it('counts the next steps instead when the result has any', () => {
    expect(healthLine([good, good, good], 1)).toBe(
      '1 thing to improve. Start with the step below.',
    );
    expect(healthLine([poor, good, good], 3)).toBe(
      '3 things to improve. Start with the steps below.',
    );
    expect(healthLine([poor, poor, NOT_MEASURED], 5)).toBe(
      '5 things to improve. Start with the steps below.',
    );
    // More qualify than are listed: the line says which ones are shown.
    expect(healthLine([poor, good, good], 4, { shown: 3 })).toBe(
      '4 things to improve. The first three are below.',
    );
    expect(healthLine([poor, good, good], 3, { shown: 3 })).toBe(
      '3 things to improve. Start with the steps below.',
    );
    // A page the scan could read next to nothing of gets no count.
    expect(healthLine([poor, poor, NOT_MEASURED], 2, { shown: 2, unreadable: true })).toBe(
      'We could read almost nothing on this page.',
    );
    expect(healthLine([CHECKING, poor, poor], 2, { unreadable: true })).toBe(
      'Checking 1 of 3 areas…',
    );
    // Never "nothing needs attention" above a list of steps.
    expect(healthLine([good, good, good], 2)).not.toMatch(/needs attention|need attention/);
    // Without steps the sentence about the areas stays.
    expect(healthLine([good, good, good], 0)).toBe('None of the 3 areas checked needs attention.');
    // While a result is on its way the line still says so.
    expect(healthLine([CHECKING, good, good], 2)).toBe('Checking 1 of 3 areas…');
  });
});

describe('aireadStatus grades and gaps', () => {
  it.each([
    ['A', 'good', 'Good'],
    ['B', 'good', 'Good'],
    ['C', 'needs-work', 'Needs work'],
    ['D', 'poor', 'Poor'],
    ['F', 'poor', 'Poor'],
  ])('grade %s is %s', (grade, state, word) => {
    expect(aireadStatus({ grade, score: 50 })).toMatchObject({ state, word });
  });
  it('is not measured without a grade or a score', () => {
    expect(aireadStatus({ score: 50 })).toEqual(NOT_MEASURED);
    expect(aireadStatus({ grade: 'A' })).toEqual(NOT_MEASURED);
    expect(aireadStatus({})).toEqual(NOT_MEASURED);
  });
});
