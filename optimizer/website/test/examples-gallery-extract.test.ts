// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the subresource extraction used by the /examples/ gallery
// generator (tools/examples-gallery/generate.mjs). The generator counts these
// resources for the "before" and "after" side of every example, so a resource
// it fails to see shows up on the page as a request and bytes "saved".

import { describe, expect, it } from 'vitest';
import {
  extractResourceRefs,
  extractResources,
} from '../../tools/examples-gallery/extract-resources.mjs';

const PAGE = 'https://demo.test/mod_pagespeed_example/page.html';

describe('examples gallery: attribute quoting', () => {
  it('reads a double-quoted value', () => {
    expect(extractResourceRefs('<img src="images/a.png" alt="">')).toEqual(['images/a.png']);
  });

  it('reads a single-quoted value', () => {
    expect(extractResourceRefs("<img src='images/a.png' alt=''>")).toEqual(['images/a.png']);
  });

  it('reads an unquoted value', () => {
    expect(extractResourceRefs('<img src=images/a.png alt="">')).toEqual(['images/a.png']);
  });

  it('reads an unquoted value that ends the tag', () => {
    expect(extractResourceRefs('<img alt="" src=images/a.png>')).toEqual(['images/a.png']);
  });

  it('allows whitespace around "="', () => {
    expect(extractResourceRefs('<img src = "a.png">')).toEqual(['a.png']);
    expect(extractResourceRefs('<img src =a.png>')).toEqual(['a.png']);
  });

  it('keeps a ">" inside a quoted value', () => {
    expect(extractResourceRefs('<img alt="a > b" src=a.png>')).toEqual(['a.png']);
  });

  it('reads link href and script src in every quoting style', () => {
    const html =
      '<link rel=stylesheet href=a.css>' +
      "<link rel='stylesheet' href='b.css'>" +
      '<script src="c.js"></script>' +
      '<script src=d.js></script>';
    expect(extractResourceRefs(html)).toEqual(['a.css', 'b.css', 'c.js', 'd.js']);
  });
});

describe('examples gallery: what counts as a resource', () => {
  it('matches attribute names exactly', () => {
    expect(extractResourceRefs('<img data-src="lazy.png" src="blank.gif">')).toEqual(['blank.gif']);
    expect(extractResourceRefs('<img data-src=lazy.png>')).toEqual([]);
  });

  it('uses the first of a repeated attribute', () => {
    expect(extractResourceRefs('<img src=a.png src=b.png>')).toEqual(['a.png']);
  });

  it('ignores markup inside a script block', () => {
    const html = '<script>document.write("<img src=x.png>")</script><img src=y.png>';
    expect(extractResourceRefs(html)).toEqual(['y.png']);
  });

  it('ignores tags it does not measure', () => {
    expect(extractResourceRefs('<a href=next.html>next</a><iframe src=f.html></iframe>')).toEqual(
      [],
    );
  });

  it('is case-insensitive for tag and attribute names', () => {
    expect(extractResourceRefs('<IMG SRC=a.png>')).toEqual(['a.png']);
  });
});

describe('examples gallery: extractResources', () => {
  it('resolves, de-duplicates and drops data: and javascript: URLs', () => {
    const html =
      '<img src=images/a.png><img src="images/a.png">' +
      '<img src="data:image/gif;base64,R0lGOD">' +
      '<script src="javascript:void(0)"></script>';
    expect(extractResources(html, PAGE)).toEqual([
      'https://demo.test/mod_pagespeed_example/images/a.png',
    ]);
  });

  it('sees the same image before and after quotes are removed', () => {
    const before =
      '<img src="images/BikeCrashIcn.png" align=\'right\' alt="" border="2" width=\'70\' height=\'70\'>';
    const after =
      '<img src=images/BikeCrashIcn.png align=right alt="" border=2 width=70 height=70>';
    expect(extractResources(after, PAGE)).toEqual(extractResources(before, PAGE));
    expect(extractResources(after, PAGE)).toHaveLength(1);
  });
});

describe('examples gallery: connection hints', () => {
  it('does not count a preconnect or dns-prefetch link as a resource', () => {
    const html =
      '<link rel="preconnect" href="http://www.gstatic.com">' +
      '<link rel="dns-prefetch" href="//ajax.googleapis.com">' +
      '<link rel="stylesheet" href="a.css">';
    expect(extractResourceRefs(html)).toEqual(['a.css']);
  });

  it('matches the rel token, not a substring', () => {
    expect(extractResourceRefs('<link rel="preconnect-ish" href="b.css">')).toEqual(['b.css']);
    expect(extractResourceRefs('<link rel="Preconnect" href="https://x.test">')).toEqual([]);
  });
});
