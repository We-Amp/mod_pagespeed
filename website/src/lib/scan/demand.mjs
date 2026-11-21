// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Gate predicates and the per-topic signal for the scanner results. Pure
// functions: data in, value out. The gates and the signal shapes follow the
// scanner's own demand module; keep them in sync with the scanner.

// Every gated topic needs a web server the visitor runs. When the scan says
// the owner has no control of the origin (a hosted site builder), no such
// topic applies. Any other reading, and no platform at all, changes nothing.
export function originTopicsApply(platform) {
  return !(platform && platform.originControl === 'none');
}

// Each gate takes the platform as an optional second argument and returns
// false when originTopicsApply(platform) is false. Called with one argument a
// gate behaves as it did before the platform was read.

export function tollboothCtaApplies(agentVerifiability, platform) {
  if (!originTopicsApply(platform)) return false;
  const d = agentVerifiability && agentVerifiability.detail;
  if (!d) return false;
  return (
    d.verifiable === false &&
    d.renderOk === true &&
    Array.isArray(d.aiCrawlersAllowed) &&
    d.aiCrawlersAllowed.length > 0 &&
    d.exposurePct >= 30
  );
}

export function agentPassCtaApplies(signedAgentVerification, platform) {
  const s = signedAgentVerification;
  return (
    originTopicsApply(platform) &&
    !!(
      s &&
      s.status === 'ok' &&
      (s.classification === 'verifying' || s.classification === 'signature-aware')
    )
  );
}

export function complianceFixCtaApplies(accessibility, platform) {
  const a = accessibility;
  return (
    originTopicsApply(platform) && !!(a && a.available && a.detail && a.detail.fixableInline > 0)
  );
}

export function consentEnforcementCtaApplies(preConsentLeak, platform) {
  const p = preConsentLeak;
  return (
    originTopicsApply(platform) &&
    !!(p && p.status === 'ok' && p.verdict === 'leaks' && p.trackerCount > 0)
  );
}

export function pageIntegrityCtaApplies(scriptInventory, platform) {
  const si = scriptInventory;
  return originTopicsApply(platform) && !!(si && si.status === 'ok' && si.verdict === 'attention');
}

export function thirdPartyFreezeCtaApplies(scriptInventory, platform) {
  const si = scriptInventory;
  return (
    originTopicsApply(platform) &&
    !!(
      si &&
      si.status === 'ok' &&
      si.verdict !== 'unknown' &&
      si.totals &&
      si.totals.thirdPartyExternal > si.totals.thirdPartyWithIntegrity
    )
  );
}

export function edgeSeoCtaApplies(sd, platform) {
  return (
    originTopicsApply(platform) &&
    !!(
      sd &&
      sd.status === 'ok' &&
      sd.verdict === 'attention' &&
      Array.isArray(sd.defects) &&
      sd.defects.length > 0
    )
  );
}

export function responseFirewallCtaApplies(rx, platform) {
  return (
    originTopicsApply(platform) &&
    !!(
      rx &&
      rx.status === 'ok' &&
      rx.verdict === 'attention' &&
      Array.isArray(rx.reasons) &&
      (rx.reasons.includes('debug-output') || rx.reasons.includes('end-of-life'))
    )
  );
}

export function markdownOriginCtaApplies(agentSignals, platform) {
  return (
    originTopicsApply(platform) &&
    !!(
      agentSignals &&
      agentSignals.status === 'ok' &&
      agentSignals.markdown &&
      agentSignals.markdown.negotiated === false
    )
  );
}

// The signal one topic carries: ids, counts and booleans from the lens behind
// the wedge, or null when the wedge has no lens. Pure.
export function lensSignalFor(wedge, report) {
  let lensSignal;
  if (wedge === 'tollbooth') {
    const d = report.agentVerifiability.detail;
    lensSignal = {
      verifiable: d.verifiable,
      exposurePct: d.exposurePct,
      aiCrawlersAllowed: d.aiCrawlersAllowed,
      aiCrawlersBlocked: d.aiCrawlersBlocked,
      botAuthHeaders: d.botAuthHeaders,
      renderOk: d.renderOk,
    };
  } else if (wedge === 'agentpass') {
    const s = report.signedAgentVerification;
    lensSignal = {
      classification: s.classification,
      evidence: s.evidence,
      probes: s.probes,
    };
  } else if (wedge === 'compliancefix') {
    const d = report.accessibility.detail;
    lensSignal = {
      total: d.total,
      fixableInline: d.fixableInline,
      byImpact: d.byImpact,
      topRules: d.topRules,
    };
  } else if (wedge === 'consent-enforcement') {
    const p = report.preConsentLeak;
    lensSignal = {
      verdict: p.verdict,
      trackerCount: p.trackerCount,
      thirdPartyCount: p.thirdPartyCount,
      topHosts: (p.thirdPartyHosts || [])
        .slice(0, 3)
        .map((h) => ({ host: h.host, requests: h.requests, category: h.category })),
      cmpDetected: p.cmpDetected,
    };
  } else if (wedge === 'page-integrity' || wedge === 'third-party-freeze') {
    const s = report.scriptInventory;
    lensSignal = {
      verdict: s.verdict,
      reasons: s.reasons,
      totals: {
        scripts: s.totals.scripts,
        inline: s.totals.inline,
        thirdPartyExternal: s.totals.thirdPartyExternal,
        thirdPartyHosts: s.totals.thirdPartyHosts,
        thirdPartyWithIntegrity: s.totals.thirdPartyWithIntegrity,
        // Present only when the scan named a hosted platform.
        ...(typeof s.totals.platformExternal === 'number'
          ? { platformExternal: s.totals.platformExternal }
          : {}),
      },
      topHosts: (s.scriptHosts || [])
        .filter((h) => h.party === 'third')
        .slice(0, 3)
        .map((h) => ({ host: h.host, scripts: h.scripts, category: h.category })),
      duplicates: (s.duplicates || []).slice(0, 3),
      csp: s.csp,
    };
  } else if (wedge === 'edge-seo') {
    const s = report.seoDefects;
    lensSignal = {
      verdict: s.verdict,
      defects: s.defects,
      counts: s.counts,
      staticCompared: s.staticCompared,
      notMeasured: s.notMeasured,
      robotsMismatch: !!(s.robots && s.robots.mismatch),
      jsonLdPresent: !!(s.jsonLd && s.jsonLd.static.blocks + s.jsonLd.rendered.blocks > 0),
      delivery: {
        cdn: s.delivery ? s.delivery.cdn : null,
        server: s.delivery ? s.delivery.server : null,
      },
    };
  } else if (wedge === 'response-firewall') {
    const s = report.responseExposure;
    lensSignal =
      s && s.status === 'ok'
        ? {
            verdict: s.verdict,
            reasons: s.reasons,
            leaks: (s.leaks || []).map((l) => ({ id: l.id, count: l.count })),
            endOfLife: (s.endOfLife || []).map((e) => ({
              product: e.product,
              line: e.line,
              basis: e.basis,
            })),
            tells: [...new Set((s.tells || []).map((t) => t.product))].slice(0, 10),
            missingHeaders: s.headers ? s.headers.missing : [],
            https: !!(s.headers && s.headers.https),
            pagespeedBucket: s.pagespeed ? s.pagespeed.bucket : null,
            delivery: {
              cdn: s.delivery ? s.delivery.cdn : null,
              server: s.delivery ? s.delivery.server : null,
            },
          }
        : null;
  } else if (wedge === 'markdown-origin') {
    const as = report.agentSignals || {};
    const m = as.markdown || {};
    lensSignal = {
      verdict: as.verdict,
      negotiated: m.negotiated,
      httpStatus: m.httpStatus ?? null,
      contentType: m.contentType ?? null,
      varyAccept: !!m.varyAccept,
    };
  } else {
    lensSignal = null;
  }
  return lensSignal;
}
