// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * What the directive finder looks for, given the text in its field.
 *
 * The reference lists a directive by its bare name (`FileCachePath`). A
 * configuration file writes it with a prefix and usually a value:
 * `ModPagespeedFileCachePath /var/cache` (Apache) or
 * `pagespeed FileCachePath /var/cache;` (nginx). The rule:
 *
 *   1. A leading `ModPagespeed`, or a leading `pagespeed` followed by a
 *      space, is the prefix and is dropped.
 *   2. The first word of what is left is the name looked for (anything after
 *      it, such as a value or a `;`, is ignored). A directive matches when
 *      its name contains that word; case does not matter.
 *   3. When a prefix was dropped and nothing is left, or what is left is a
 *      module state (`on`, `off`, `standby`, `unplugged`), the text is the
 *      line that switches the module itself: only that entry matches.
 */

/** The reference's entry for the directive that switches the module. */
export const MODULE_ENTRY = 'pagespeed / ModPagespeed';

const MODULE_STATES = new Set(['on', 'off', 'standby', 'unplugged']);

export type DirectiveQuery =
  { kind: 'none' } | { kind: 'module' } | { kind: 'name'; needle: string };

const firstWord = (text: string) => text.split(/[\s;]+/).find((word) => word !== '') ?? '';

export function parseDirectiveQuery(typed: string): DirectiveQuery {
  const text = typed.trim().toLowerCase();
  if (!text) return { kind: 'none' };
  const prefixed = /^(?:modpagespeed|pagespeed(?=\s))(.*)$/.exec(text);
  if (prefixed) {
    const word = firstWord(prefixed[1]);
    if (!word || MODULE_STATES.has(word)) return { kind: 'module' };
    return { kind: 'name', needle: word };
  }
  const word = firstWord(text);
  return word ? { kind: 'name', needle: word } : { kind: 'none' };
}

/** Whether the entry called `name` in the reference matches `query`. */
export function matchesDirective(name: string, query: DirectiveQuery): boolean {
  if (query.kind === 'none') return true;
  if (query.kind === 'module') return name === MODULE_ENTRY;
  return name.toLowerCase().includes(query.needle);
}
