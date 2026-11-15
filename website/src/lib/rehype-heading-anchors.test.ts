// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import type { Element, ElementContent, Root } from 'hast';
import rehypeHeadingAnchors from './rehype-heading-anchors.mjs';

const t = (value: string) => ({ type: 'text' as const, value });
const h = (
  tagName: string,
  properties: Element['properties'],
  children: ElementContent[],
): Element => ({
  type: 'element',
  tagName,
  properties,
  children,
});

function run(children: ElementContent[], path = '/site/src/content/docs/page.md'): Root {
  const tree: Root = { type: 'root', children };
  (rehypeHeadingAnchors() as (tree: Root, file: { path: string }) => void)(tree, { path });
  return tree;
}

function textOf(node: Root | Root['children'][number]): string {
  if (node.type === 'text') return node.value;
  return 'children' in node ? node.children.map(textOf).join('') : '';
}

const anchors = (el: Element) =>
  el.children.filter((c): c is Element => c.type === 'element' && c.tagName === 'a');

describe('rehypeHeadingAnchors', () => {
  it('adds a text-less permalink and names the heading by its own text', () => {
    const heading = h('h2', { id: 'roll-back' }, [t('Roll back')]);
    run([heading]);
    expect(heading.properties.ariaLabelledBy).toEqual(['roll-back--text']);
    expect(heading.children[0]).toEqual(h('span', { id: 'roll-back--text' }, [t('Roll back')]));
    expect(anchors(heading)).toEqual([
      h(
        'a',
        { className: ['heading-anchor'], href: '#roll-back', ariaLabel: 'Link to “Roll back”' },
        [],
      ),
    ]);
  });

  it('leaves the heading text exactly as written: no "#" in it', () => {
    const heading = h('h3', { id: 'x' }, [t('Use '), h('code', {}, [t('pagespeed on')])]);
    run([heading]);
    expect(textOf(heading)).toBe('Use pagespeed on');
    expect(anchors(heading)[0].properties.ariaLabel).toBe('Link to “Use pagespeed on”');
  });

  it('covers h2 to h4 with an id, nested anywhere, and nothing else', () => {
    const list = [
      h('h1', { id: 'a' }, [t('A')]),
      h('h2', { id: 'b' }, [t('B')]),
      h('div', {}, [h('h3', { id: 'c' }, [t('C')])]),
      h('h4', { id: 'd' }, [t('D')]),
      h('h5', { id: 'e' }, [t('E')]),
      h('h2', {}, [t('No id')]),
    ];
    const tree = run(list);
    const count = JSON.stringify(tree).match(/heading-anchor/g)?.length;
    expect(count).toBe(3);
  });

  it('runs only on docs and how-it-works pages', () => {
    const how = h('h2', { id: 'b' }, [t('B')]);
    run([how], '/site/src/content/how-it-works/p.md');
    expect(anchors(how)).toHaveLength(1);
    const blog = h('h2', { id: 'b' }, [t('B')]);
    run([blog], '/site/src/content/blog/p.md');
    expect(blog.children).toEqual([t('B')]);
  });

  it('keeps an author-set accessible name and never adds a second permalink', () => {
    const heading = h('h2', { id: 'b', ariaLabel: 'Bee' }, [t('B')]);
    run([heading]);
    run([heading]);
    expect(heading.properties.ariaLabelledBy).toBeUndefined();
    expect(anchors(heading)).toHaveLength(1);
  });
});
