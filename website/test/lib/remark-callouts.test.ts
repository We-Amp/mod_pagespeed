// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import remarkCallouts from '../../src/lib/remark-callouts.mjs';

interface NodeData {
  directiveLabel?: boolean;
  hName?: string;
  hProperties?: { className: string[]; 'data-released-after'?: string };
}

interface TestNode {
  type: string;
  name?: string;
  attributes?: Record<string, string>;
  depth?: number;
  value?: string;
  children?: TestNode[];
  data?: NodeData;
}

const text = (value: string): TestNode => ({ type: 'text', value });

const paragraph = (...children: TestNode[]): TestNode => ({
  type: 'paragraph',
  children: [...children],
});

/** What remark-directive parses a `[Label]` into: the first paragraph, so flagged. */
const labelParagraph = (...children: TestNode[]): TestNode => ({
  type: 'paragraph',
  data: { directiveLabel: true },
  children: [...children],
});

const directive = (name: string, children: TestNode[] = []): TestNode => ({
  type: 'containerDirective',
  name,
  children,
});

/** The title paragraph the plugin unshifts onto the body. */
const titleParagraph = (title: string): TestNode => ({
  type: 'paragraph',
  data: { hName: 'p', hProperties: { className: ['callout-title'] } },
  children: [text(title)],
});

// The plugin types its argument as an mdast Root, but the nodes it transforms
// are remark-directive's containerDirective nodes, which @types/mdast does not
// model; feed it hand-built equivalents.
const transform = (tree: TestNode): TestNode => {
  (remarkCallouts() as unknown as (node: TestNode) => unknown)(tree);
  return tree;
};

describe('remarkCallouts', () => {
  it('gives each known callout its default title and its callout classes', () => {
    const known: Array<[string, string]> = [
      ['note', 'Note'],
      ['tip', 'Tip'],
      ['caution', 'Caution'],
      ['warning', 'Warning'],
      ['danger', 'Danger'],
      ['important', 'Important'],
    ];
    const out = known.map(([name, title]) => {
      const node = transform(directive(name, [paragraph(text('Body.'))]));
      const first = node.children?.[0];
      return {
        name,
        aside: node.data?.hName,
        classes: node.data?.hProperties?.className,
        title: first?.children?.[0]?.value,
        titleClasses: first?.data?.hProperties?.className,
      };
    });
    expect(out).toEqual(
      known.map(([name, title]) => ({
        name,
        aside: 'aside',
        classes: ['callout', `callout-${name}`],
        title,
        titleClasses: ['callout-title'],
      })),
    );
  });

  it('lifts a [Label] into the title and drops the label paragraph from the body', () => {
    const node = transform(
      directive('caution', [labelParagraph(text('Experimental')), paragraph(text('Body.'))]),
    );
    expect(node.children).toEqual([titleParagraph('Experimental'), paragraph(text('Body.'))]);
    expect(node.children?.some((child) => child.data?.directiveLabel)).toBe(false);
  });

  it('takes the label text from the whole paragraph, not just its first child', () => {
    const node = transform(
      directive('note', [labelParagraph(text('Two '), text('parts')), paragraph(text('Body.'))]),
    );
    expect(node.children?.[0]).toEqual(titleParagraph('Two parts'));
  });

  it('keeps the default title for a blank label but still drops the label paragraph', () => {
    const node = transform(
      directive('tip', [labelParagraph(text('   ')), paragraph(text('Body.'))]),
    );
    expect(node.children).toEqual([titleParagraph('Tip'), paragraph(text('Body.'))]);
  });

  it('renders a next-release marker with its title, and keeps its after on the aside', () => {
    const node = transform({
      ...directive('next-release', [paragraph(text('Body.'))]),
      attributes: { after: '2.8.2' },
    });
    expect(node.data).toEqual({
      hName: 'aside',
      hProperties: {
        className: ['callout', 'callout-next-release'],
        'data-released-after': '2.8.2',
      },
    });
    expect(node.children).toEqual([
      titleParagraph('From the next release'),
      paragraph(text('Body.')),
    ]);
  });

  it('leaves a directive whose name it does not know untouched', () => {
    const node = transform(directive('spoiler', [paragraph(text('Body.'))]));
    expect(node).toEqual(directive('spoiler', [paragraph(text('Body.'))]));
  });

  it('touches nothing but container directives', () => {
    const tree: TestNode = {
      type: 'root',
      children: [
        { type: 'heading', depth: 2, children: [text('A heading')] },
        paragraph(text('Plain prose.')),
        { type: 'leafDirective', name: 'note' },
        directive('spoiler', [paragraph(text('Body.'))]),
      ],
    };
    const before = structuredClone(tree);
    transform(tree);
    expect(tree).toEqual(before);
  });

  it('transforms a callout nested inside another callout', () => {
    const outer = directive('note', [
      paragraph(text('Outer body.')),
      directive('danger', [labelParagraph(text('Inner label')), paragraph(text('Inner body.'))]),
    ]);
    transform(outer);
    expect(outer.data).toEqual({
      hName: 'aside',
      hProperties: { className: ['callout', 'callout-note'] },
    });
    expect(outer.children).toEqual([
      titleParagraph('Note'),
      paragraph(text('Outer body.')),
      {
        type: 'containerDirective',
        name: 'danger',
        data: { hName: 'aside', hProperties: { className: ['callout', 'callout-danger'] } },
        children: [titleParagraph('Inner label'), paragraph(text('Inner body.'))],
      },
    ]);
  });
});
