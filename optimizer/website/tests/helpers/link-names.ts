// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In-page audit of image alternatives and link names, run through
// page.evaluate. The name rules follow the accessible name computation closely
// enough for links: aria-labelledby, then aria-label, then the content (text,
// an image's alt, an svg's label or <title>), then title. Hidden content
// (aria-hidden, display:none) contributes nothing.

export interface LinkAudit {
  /** Images with no alt attribute at all (alt="" is a valid decorative mark). */
  imagesWithoutAlt: string[];
  /** Exposed links whose accessible name is empty. */
  unnamedLinks: string[];
  /** Exposed links named only with a generic phrase such as "read more". */
  genericLinks: string[];
  /** Names used by exposed links that go to more than one page. */
  sameNameDifferentTargets: { name: string; targets: string[] }[];
}

export function auditLinks(): LinkAudit {
  const GENERIC = new Set([
    'here',
    'click here',
    'read more',
    'more',
    'learn more',
    'link',
    'this',
    'this page',
    'details',
    'continue',
  ]);
  const hidden = (el: Element) =>
    el.closest('[aria-hidden="true"], [inert]') !== null ||
    (el instanceof HTMLElement && el.hidden);

  const nameFrom = (node: Node): string => {
    if (node.nodeType === Node.TEXT_NODE) return node.textContent ?? '';
    if (!(node instanceof Element)) return '';
    if (node.getAttribute('aria-hidden') === 'true') return '';
    const style = getComputedStyle(node);
    if (style.display === 'none' || style.visibility === 'hidden') return '';
    const label = node.getAttribute('aria-label')?.trim();
    if (node.localName === 'img') return label || node.getAttribute('alt') || '';
    if (node.localName === 'svg') {
      return label || node.querySelector(':scope > title')?.textContent || '';
    }
    if (label && node.getAttribute('role') === 'img') return label;
    return Array.from(node.childNodes).map(nameFrom).join(' ');
  };

  const accessibleName = (a: Element): string => {
    const ids = a.getAttribute('aria-labelledby');
    let name = '';
    if (ids) {
      name = ids
        .split(/\s+/)
        .map((id) => document.getElementById(id)?.textContent ?? '')
        .join(' ');
    }
    if (!name.trim()) name = a.getAttribute('aria-label') ?? '';
    if (!name.trim()) name = Array.from(a.childNodes).map(nameFrom).join(' ');
    if (!name.trim()) name = a.getAttribute('title') ?? '';
    return name.replace(/\s+/g, ' ').trim();
  };

  const describe = (el: Element) => {
    const html = el.outerHTML.replace(/\s+/g, ' ');
    return html.length > 160 ? `${html.slice(0, 160)}…` : html;
  };

  const imagesWithoutAlt = Array.from(document.querySelectorAll('img'))
    .filter((img) => !img.hasAttribute('alt'))
    .map(describe);

  const unnamedLinks: string[] = [];
  const genericLinks: string[] = [];
  const byName = new Map<string, Set<string>>();
  for (const a of Array.from(document.querySelectorAll('a[href]'))) {
    // Only links a screen reader is offered: not hidden from the
    // accessibility tree and actually rendered (a visually hidden link with a
    // box still counts).
    if (hidden(a) || !a.checkVisibility()) continue;
    const name = accessibleName(a);
    if (!name) {
      unnamedLinks.push(describe(a));
      continue;
    }
    // Compared without case or a trailing arrow or full stop.
    const key = name
      .toLowerCase()
      .replace(/[\s.…:→›»↗]+$/u, '')
      .trim();
    if (GENERIC.has(key)) genericLinks.push(`"${name}"`);
    // Two fragments of one page are one destination.
    const target = (a as HTMLAnchorElement).href.replace(/#.*$/, '');
    if (!byName.has(key)) byName.set(key, new Set());
    byName.get(key)!.add(target);
  }
  const sameNameDifferentTargets = Array.from(byName.entries())
    .filter(([, targets]) => targets.size > 1)
    .map(([name, targets]) => ({
      name,
      targets: Array.from(targets).map((t) => t.replace(location.origin, '')),
    }));

  return { imagesWithoutAlt, unnamedLinks, genericLinks, sameNameDifferentTargets };
}
