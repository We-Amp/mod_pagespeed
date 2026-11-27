// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import {
  SHORTCUTS,
  isMac,
  shortcutLabel,
  type ShortcutEntry,
} from '../keyboard-shortcuts';

describe('keyboard-shortcuts', () => {
  describe('SHORTCUTS', () => {
    it('has at least 3 shortcut entries', () => {
      expect(SHORTCUTS.length).toBeGreaterThanOrEqual(3);
    });

    it('includes Ctrl+K for URL focus', () => {
      const found = SHORTCUTS.find((s) => s.label === 'Ctrl+K');
      expect(found).toBeDefined();
      expect(found!.description).toContain('URL');
    });

    it('includes Escape for closing', () => {
      const found = SHORTCUTS.find((s) => s.label === 'Escape');
      expect(found).toBeDefined();
      expect(found!.description).toContain('Close');
    });

    it('includes ? for help', () => {
      const found = SHORTCUTS.find((s) => s.label === '?');
      expect(found).toBeDefined();
      expect(found!.description).toContain('shortcut');
    });

    it('every entry has both label and macLabel', () => {
      for (const s of SHORTCUTS) {
        expect(s.label).toBeTruthy();
        expect(s.macLabel).toBeTruthy();
        expect(s.description).toBeTruthy();
      }
    });
  });

  describe('isMac', () => {
    it('returns a boolean', () => {
      // In a test environment, navigator may not be defined.
      expect(typeof isMac()).toBe('boolean');
    });
  });

  describe('shortcutLabel', () => {
    it('returns the correct label based on platform', () => {
      const entry: ShortcutEntry = {
        label: 'Ctrl+K',
        macLabel: '\u2318K',
        description: 'Focus URL input',
      };
      const result = shortcutLabel(entry);
      // Should be one of the two labels.
      expect([entry.label, entry.macLabel]).toContain(result);
    });
  });
});
