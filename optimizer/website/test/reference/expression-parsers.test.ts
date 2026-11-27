// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// expression-parsers.test.ts — the default-value expressions the reference
// generator evaluates from the module's option registrations. A form the
// evaluator cannot read renders as "Default: —", so each form a registration
// uses has a case here.

import { describe, it, expect } from 'vitest';
import { evalExpr, resolveDefault } from '../../scripts/lib/reference-parsers.mjs';

const noLookup = () => null;

describe('evalExpr', () => {
  it('evaluates left shifts with C precedence', () => {
    expect(evalExpr('2 << 20', noLookup)).toBe(2097152);
    expect(evalExpr('4 << 20', noLookup)).toBe(4194304);
    expect(evalExpr('1 << 20', noLookup)).toBe(1048576);
    expect(evalExpr('(1 << 3) + 1', noLookup)).toBe(9);
    // `+` binds tighter than `<<` in C.
    expect(evalExpr('1 << 2 + 1', noLookup)).toBe(8);
  });

  it('evaluates shifts past 32 bits exactly', () => {
    expect(evalExpr('1 << 40', noLookup)).toBe(1099511627776);
  });

  it('refuses shifts it cannot represent exactly or would approximate', () => {
    expect(evalExpr('1 << 60', noLookup)).toBeNull();
    expect(evalExpr('2 << 20 / 2', noLookup)).toBeNull();
    expect(evalExpr('1 < 2', noLookup)).toBeNull();
    expect(evalExpr('1 <<< 2', noLookup)).toBeNull();
  });

  it('evaluates brace-initialized integer casts', () => {
    expect(evalExpr('int64{5} * 1024 * 1024', noLookup)).toBe(5242880);
    expect(evalExpr('int64{3}', noLookup)).toBe(3);
    expect(evalExpr('uint32_t{1} << 10', noLookup)).toBe(1024);
  });

  it('keeps the forms it already read', () => {
    expect(evalExpr('static_cast<const int64>(48 * 1024)', noLookup)).toBe(49152);
    expect(evalExpr('10 * 1000L', noLookup)).toBe(10000);
  });
});

describe('resolveDefault', () => {
  const constants = new Map([
    ['kDefaultCssParseMaxBytes', { kind: 'expr', value: '2 << 20' }],
    ['kDefaultSized', { kind: 'expr', value: 'int64{2} * kDefaultCssParseMaxBytes' }],
  ]);
  const enums = new Map([['kDaemonHtmlCacheOff', 'off']]);

  it('resolves a constant defined by a shift to an int', () => {
    expect(resolveDefault('kDefaultCssParseMaxBytes', constants, enums)).toEqual({
      value: '2097152',
      type: 'int',
    });
    expect(resolveDefault('kDefaultSized', constants, enums)).toEqual({
      value: '4194304',
      type: 'int',
    });
  });

  it('resolves an inline brace-cast expression to an int', () => {
    expect(resolveDefault('int64{5} * 1024 * 1024', constants, enums)).toEqual({
      value: '5242880',
      type: 'int',
    });
  });

  it('resolves an enumerator through the enum map', () => {
    expect(resolveDefault('kDaemonHtmlCacheOff', constants, enums)).toEqual({
      value: 'off',
      type: 'enum',
    });
  });
});
