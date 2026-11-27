// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A deliberately small DOM stand-in for executing the async-CSS loader.
//
// It is NOT a browser. It models exactly the surface the loader touches, and it
// models it HONESTLY — which is the whole point. Two decisions matter:
//
//   * `querySelectorAll` really parses `tag[attr]` and really filters on
//     attribute presence. A stub that returned every link regardless of the
//     selector would make "an unmarked preload is left alone" pass vacuously,
//     and would hide a loader that widened its selector.
//   * `createElement` throws. The preload's consumer must be the SAME element
//     the loader flips; a loader that ever built a second <link> would double
//     the download, and here it fails loudly instead.
//
// `rel` and `media` are plain properties, so setting them has no side effects.
// Real rel-reflection (setting rel un-applies / re-applies the sheet) was
// verified separately in the pinned Playwright Chromium; this harness is for
// the loader's own control flow, which is what the C++ tests cannot see.

import vm from 'node:vm';

class FakeLink {
  constructor({ href, rel = 'preload', as = 'style', media = '', attrs = {} }) {
    this.tagName = 'link';
    this.href = href;
    this.rel = rel;
    this.as = as;
    this.media = media;
    this._attrs = { ...attrs };
    this._handlers = new Map();
    // Every addEventListener call, so a test can assert the loader registered
    // what it claims to (and did not, say, drop the error path).
    this.listenerLog = [];
  }

  getAttribute(name) {
    return Object.hasOwn(this._attrs, name) ? this._attrs[name] : null;
  }

  hasAttribute(name) {
    return Object.hasOwn(this._attrs, name);
  }

  addEventListener(type, fn, options) {
    this.listenerLog.push({ type, once: Boolean(options && options.once) });
    if (!this._handlers.has(type)) this._handlers.set(type, []);
    this._handlers.get(type).push({ fn, once: Boolean(options && options.once) });
  }

  /** Fire an event the way the browser would, honouring `{once:true}`. */
  dispatch(type) {
    const entries = this._handlers.get(type) || [];
    this._handlers.set(
      type,
      entries.filter((e) => !e.once),
    );
    for (const e of entries) e.fn();
  }
}

// Minimal `tag[attr]` / `tag` selector support — enough for the loader, and
// strict enough that an unrecognised selector is an error rather than a
// silently-everything match.
function matchSelector(selector, link) {
  const m = /^([a-z]+)(?:\[([a-z0-9-]+)\])?$/i.exec(selector);
  if (!m) throw new Error(`dom-stub: unsupported selector ${selector}`);
  const [, tag, attr] = m;
  if (link.tagName !== tag) return false;
  return attr === undefined ? true : link.hasAttribute(attr);
}

/**
 * Run the loader against a set of links.
 *
 * Returns the links plus the window-load handlers the loader registered, so a
 * test can drive each trigger independently.
 */
export function runLoader(loaderJs, links, { resourceTimingHrefs = [] } = {}) {
  const windowLoadHandlers = [];
  const selectorsSeen = [];

  const documentStub = {
    querySelectorAll(selector) {
      selectorsSeen.push(selector);
      return links.filter((l) => matchSelector(selector, l));
    },
    createElement(tag) {
      throw new Error(
        `dom-stub: the loader must not create elements (tried <${tag}>) — ` +
          'the preload it flips has to BE the consumer, or the browser ' +
          'downloads the stylesheet twice.',
      );
    },
  };

  const performanceStub = {
    getEntriesByName(name) {
      return resourceTimingHrefs.includes(name) ? [{ name }] : [];
    },
  };

  const windowStub = {
    performance: performanceStub,
    addEventListener(type, fn) {
      if (type === 'load') windowLoadHandlers.push(fn);
    },
  };

  const sandbox = {
    document: documentStub,
    performance: performanceStub,
    window: windowStub,
    // The loader may call bare addEventListener; route it to the window.
    addEventListener: (type, fn) => windowStub.addEventListener(type, fn),
  };
  sandbox.window.document = documentStub;

  vm.runInNewContext(loaderJs, vm.createContext(sandbox), {
    filename: 'async_css_loader.js',
  });

  return {
    links,
    selectorsSeen,
    fireWindowLoad() {
      for (const fn of windowLoadHandlers) fn();
    },
    windowLoadHandlerCount: windowLoadHandlers.length,
  };
}

/** A deferred stylesheet as the transform emits it. */
export function deferredLink(href, recordedMedia = 'screen') {
  return new FakeLink({
    href,
    rel: 'preload',
    as: 'style',
    media: '',
    attrs: { 'data-pagespeed-async': '', 'data-pagespeed-media': recordedMedia },
  });
}

export { FakeLink };
