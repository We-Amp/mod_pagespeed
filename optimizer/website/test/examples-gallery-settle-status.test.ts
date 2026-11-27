// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the status the /examples/ gallery generator records per
// example (tools/examples-gallery/settle-status.mjs). Fixtures are shaped on
// what the demo host serves; no network.

import { describe, expect, it } from 'vitest';
import {
  STATUSES,
  classifySettle,
  hasBeaconInstrumentation,
  hintLinks,
  isBeaconOnly,
  legacyStatus,
  materialSignature,
  stripBeaconInstrumentation,
} from '../../tools/examples-gallery/settle-status.mjs';

const BEFORE = `<html>
  <head>
    <title>example</title>
  </head>
  <body>
    <img src="images/Puzzle.jpg" width="256" height="192"/>
  </body>
</html>
`;

const BEACON_SCRIPT = `<script data-pagespeed-no-defer type="text/javascript">//<![CDATA[
(()=>{var h=function(t,i,e){}})();
pagespeed.CriticalImages.Run('/mod_pagespeed_beacon','https://demo.test/page.html','nonce1',true,false,'abc');
//]]></script>`;

const ONLOAD =
  ' data-pagespeed-url-hash="1227742713" onload="if(window.pagespeed){if(pagespeed.CriticalImages){pagespeed.CriticalImages.checkImageForCriticality(this);}}"';

// Beacon script injected, image tag re-serialized with the per-image check.
const BEACON_ONLY = `<html>
  <head>
    <title>example</title>
  </head>
  <body>
    ${BEACON_SCRIPT}<img src="images/Puzzle.jpg" width="256" height="192"${ONLOAD}>
  </body>
</html>
`;

// Beacon instrumentation plus a real change (lazy loading).
const BEACON_AND_LAZY = BEACON_ONLY.replace(`${ONLOAD}>`, `${ONLOAD} loading="lazy">`);

const REWRITTEN = BEFORE.replace('images/Puzzle.jpg', 'images/Puzzle.jpg.pagespeed.ce.HASH.jpg');

const HINT =
  '</ex/styles/all.css>; rel=preload; as=style; nopush, </ex/inline_javascript.js>; rel=preload; as=script; nopush';

function poll(over: Record<string, unknown> = {}) {
  return {
    userAgent: 'default',
    settled: false,
    everDiffered: false,
    body: BEFORE,
    headers: { 'x-mod-pagespeed': '2.8.1' } as Record<string, string>,
    attempts: 12,
    windowMs: 60000,
    ...over,
  };
}

const before = { body: BEFORE, headers: {} };

describe('examples gallery: beacon instrumentation', () => {
  it('detects the beacon script', () => {
    expect(hasBeaconInstrumentation(BEACON_ONLY)).toBe(true);
    expect(hasBeaconInstrumentation(BEFORE)).toBe(false);
  });

  it('strips the script and the per-image attributes, nothing else', () => {
    const stripped = stripBeaconInstrumentation(BEACON_AND_LAZY);
    expect(stripped).not.toContain('mod_pagespeed_beacon');
    expect(stripped).not.toContain('onload=');
    expect(stripped).not.toContain('data-pagespeed-url-hash');
    expect(stripped).toContain('loading="lazy"');
  });

  it('keeps scripts that are not the beacon', () => {
    const page = BEFORE.replace('</body>', '<script>var a = 1;</script></body>');
    expect(stripBeaconInstrumentation(page)).toBe(page);
  });

  it('calls a beacon-only change beacon-only, and a real change not', () => {
    expect(isBeaconOnly(BEFORE, BEACON_ONLY)).toBe(true);
    expect(isBeaconOnly(BEFORE, BEACON_AND_LAZY)).toBe(false);
    expect(isBeaconOnly(BEFORE, REWRITTEN)).toBe(false);
    expect(isBeaconOnly(BEFORE, BEFORE)).toBe(false);
  });
});

describe('examples gallery: hint headers', () => {
  it('splits a Link header into its hint entries', () => {
    expect(hintLinks({ link: HINT })).toEqual([
      '</ex/styles/all.css>; rel=preload; as=style; nopush',
      '</ex/inline_javascript.js>; rel=preload; as=script; nopush',
    ]);
  });

  it('ignores Link entries that are not resource hints', () => {
    expect(hintLinks({ link: '<https://demo.test/a>; rel=canonical' })).toEqual([]);
    expect(hintLinks({})).toEqual([]);
    expect(hintLinks(undefined)).toEqual([]);
  });
});

describe('examples gallery: settle status', () => {
  it('only uses the documented statuses', () => {
    expect(STATUSES).toEqual(['settled', 'header', 'beacon', 'not-applied', 'unsettled']);
  });

  it('settled: the body changed and held', () => {
    const v = classifySettle(before, [
      poll({ settled: true, everDiffered: true, body: REWRITTEN, attempts: 2 }),
    ]);
    expect(v.status).toBe('settled');
    expect(v.settled).toBe(true);
    expect(v.poll).toBe(0);
    expect(v.evidence).toMatchObject({ userAgent: 'default', requests: 2, bodyChanged: true });
  });

  it('settled with the browser user agent when only that one applies the filter', () => {
    const v = classifySettle(before, [
      poll(),
      poll({
        userAgent: 'browser',
        settled: true,
        everDiffered: true,
        body: BEACON_AND_LAZY,
        attempts: 2,
        windowMs: 1000,
      }),
    ]);
    expect(v.status).toBe('settled');
    expect(v.poll).toBe(1);
    expect(v.evidence).toMatchObject({
      userAgent: 'browser',
      userAgentsTried: ['default', 'browser'],
      requests: 14,
      windowSeconds: 61,
      beaconInstrumented: true,
    });
  });

  it('beacon: the only change is the beacon instrumentation', () => {
    const v = classifySettle(before, [
      poll(),
      poll({
        userAgent: 'browser',
        settled: true,
        everDiffered: true,
        body: BEACON_ONLY,
        attempts: 2,
      }),
    ]);
    expect(v.status).toBe('beacon');
    expect(v.settled).toBe(false);
    expect(v.poll).toBe(1);
    expect(v.evidence.beaconInstrumented).toBe(true);
  });

  it('beacon: instrumented on some polls only, and the capture is the instrumented body', () => {
    const v = classifySettle(before, [
      poll(),
      poll({
        userAgent: 'browser',
        everDiffered: true,
        body: BEFORE,
        changedBody: BEACON_ONLY,
        attempts: 6,
      }),
    ]);
    expect(v.status).toBe('beacon');
    expect(v.body).toBe(BEACON_ONLY);
    expect(v.evidence.beaconInstrumented).toBe(true);
  });

  it('a real change seen once but not held stays unsettled', () => {
    const v = classifySettle(before, [poll({ everDiffered: true, changedBody: REWRITTEN })]);
    expect(v.status).toBe('unsettled');
  });

  it('header: no body change, but a hint header the original lacks', () => {
    const v = classifySettle(before, [
      poll({ headers: { 'x-mod-pagespeed': '2.8.1', link: HINT } }),
    ]);
    expect(v.status).toBe('header');
    expect(v.settled).toBe(false);
    expect(v.evidence.hintHeader).toContain('rel=preload');
  });

  it('a hint header the original already has is not the filter', () => {
    const v = classifySettle({ body: BEFORE, headers: { link: HINT } }, [
      poll({ headers: { link: HINT } }),
    ]);
    expect(v.status).toBe('not-applied');
  });

  it('not-applied: nothing changed for any user agent, with the evidence', () => {
    const v = classifySettle(before, [
      poll(),
      poll({ userAgent: 'browser', attempts: 6, windowMs: 13000 }),
    ]);
    expect(v.status).toBe('not-applied');
    expect(v.settled).toBe(false);
    expect(v.evidence).toEqual({
      userAgent: 'browser',
      userAgentsTried: ['default', 'browser'],
      requests: 18,
      windowSeconds: 73,
      moduleHeader: '2.8.1',
      bodyChanged: false,
      beaconInstrumented: false,
    });
  });

  it('unsettled: the body changed at some point but never held', () => {
    const v = classifySettle(before, [
      poll({ everDiffered: true }),
      poll({ userAgent: 'browser' }),
    ]);
    expect(v.status).toBe('unsettled');
    expect(v.evidence.bodyChanged).toBe(true);
  });

  it('needs a poll', () => {
    expect(() => classifySettle(before, [])).toThrow();
  });
});

describe('examples gallery: status of entries written before the field', () => {
  it('derives it from the stored fields', () => {
    expect(
      legacyStatus({ settled: true, before: { html: BEFORE }, after: { html: REWRITTEN } }),
    ).toBe('settled');
    expect(
      legacyStatus({ settled: true, before: { html: BEFORE }, after: { html: BEACON_ONLY } }),
    ).toBe('beacon');
    expect(
      legacyStatus({ settled: false, before: { html: BEFORE }, after: { html: BEFORE } }),
    ).toBe('unsettled');
    expect(legacyStatus({ status: 'header', settled: false })).toBe('header');
    expect(legacyStatus({ error: 'before HTTP 500' })).toBeUndefined();
  });
});

describe('examples gallery: add_instrumentation is the filter, not a wait for a browser', () => {
  // add_instrumentation's own output is a start-time script plus a script that
  // reports to the beacon endpoint; the start-time script is a real change.
  const INSTRUMENTED = BEFORE.replace(
    '</head>',
    '<script>window.mod_pagespeed_start = Number(new Date());</script></head>',
  ).replace(
    '</body>',
    `<script data-pagespeed-no-defer type="text/javascript">//<![CDATA[
(()=>{window.pagespeed=window.pagespeed||{};})();
pagespeed.addInstrumentationInit('/mod_pagespeed_beacon', 'load', '', 'https://demo.test/page.html');
//]]></script></body>`,
  );

  it('settles', () => {
    expect(isBeaconOnly(BEFORE, INSTRUMENTED)).toBe(false);
    const v = classifySettle(before, [
      poll({ settled: true, everDiffered: true, body: INSTRUMENTED, attempts: 2 }),
    ]);
    expect(v.status).toBe('settled');
  });
});

describe('examples gallery: material change signature', () => {
  const beaconEntry = (nonce: string) => ({
    status: 'beacon',
    settled: false,
    savings: null,
    diff: [
      { t: ' ', s: '<body>' },
      {
        t: '+',
        s: `pagespeed.CriticalImages.Run('/mod_pagespeed_beacon','https://demo.test/page.html','rTdnZNYuK5',true,false,'${nonce}');`,
      },
    ],
  });
  const settledEntry = (src: string) => ({
    status: 'settled',
    settled: true,
    savings: { requests: 1 },
    diff: [{ t: '+', s: `<img src="${src}">` }],
  });

  it('ignores the per-response nonce of a beacon capture', () => {
    expect(materialSignature({ a: beaconEntry('aOLkVr7YRIU') })).toBe(
      materialSignature({ a: beaconEntry('7POttjPL4S8') }),
    );
  });

  it('still sees a status change of a beacon entry', () => {
    expect(materialSignature({ a: beaconEntry('x') })).not.toBe(
      materialSignature({ a: { ...beaconEntry('x'), status: 'not-applied' } }),
    );
  });

  it('sees a flip between header and not-applied, which share an empty diff', () => {
    const entry = (status: string) => ({ status, settled: false, savings: null, diff: [] });
    expect(materialSignature({ a: entry('header') })).not.toBe(
      materialSignature({ a: entry('not-applied') }),
    );
  });

  it('sees a changed diff of a settled entry, and ignores key order', () => {
    expect(materialSignature({ a: settledEntry('a.png') })).not.toBe(
      materialSignature({ a: settledEntry('b.png') }),
    );
    expect(materialSignature({ a: settledEntry('a.png'), b: beaconEntry('x') })).toBe(
      materialSignature({ b: beaconEntry('y'), a: settledEntry('a.png') }),
    );
  });
});
