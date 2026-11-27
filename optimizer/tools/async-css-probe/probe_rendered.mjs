// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Rendered lane (Lane B): does the fold look finished before the deferred
// stylesheet applies?
//
// The markup lane proves the transform emitted the right bytes. It cannot prove
// the page LOOKS right while the deferred stylesheet is still in flight, which
// is the failure this whole effort exists to prevent. This lane renders the
// served page in a real browser and measures it.
//
// METHOD, per viewport:
//   1. Load the served page normally and let it finish. The loader has run, the
//      full stylesheet applies. Screenshot: this is the reference, and the
//      committed golden.
//   2. Put the deferred <link>s back to the non-applying primitive — undoing
//      exactly what the loader did — and screenshot again. The DOM is identical,
//      every subresource is in the same state, and the only difference is that
//      the deferred stylesheet does not apply. That is the CSS state of the
//      flash window.
//   3. Diff. A page whose critical CSS really covers the fold looks the same in
//      both; a difference is the flash.
//
// WHY NOT HOLD THE RESPONSE. The obvious method — intercept the stylesheet and
// hold it, so the browser genuinely sits in the flash window — was implemented
// and does not work here: enabling route interception at all changes the request
// the front end sees (Vary: Accept-Encoding, User-Agent), so the intercepted
// navigation is answered by a different cached variant — the origin-bytes one —
// and never converges on the deferred page no matter how long it is warmed.
// Everything measured under interception was therefore measured on the wrong
// document.
//
// Step 2 reconstructs the flash window's CSS state instead of observing it. The
// reconstruction is exact in the dimension being measured and free of the
// variant divergence that makes the observed version unusable.
//
// FONTS AND FOLD IMAGES ARE PRESENT, and that is on purpose. The fixture
// capture carries the three web fonts and the two logos the fold renders with
// (fixtures/modpagespeed-com/CAPTURE.md), so the deferred stylesheet's
// @font-face rules resolve against real files and the reconstructed flash
// window shows what a visitor would see: text falling back to system metrics,
// not text that was never webfont-styled in either screenshot. This lane is the
// ONLY place that class of flash is ever covered — the in-product validation
// gate blocks every subresource in its render as an SSRF defence, so it is
// blind to it permanently and by design.
//
// Two things are pinned below because those assets brought their own
// nondeterminism, not because the lane wanted more emulation: `reducedMotion`
// (the logo SVG animates a blinking cursor, and honours the preference itself)
// and a wait on `document.fonts.ready` before each screenshot (a shot taken
// mid-swap is a fallback-font render that reads as a flash).
//
// NAMED LIMITATIONS. What a green run here does NOT establish:
//
//   * Only the stylesheet's absence is reconstructed. A flash caused by ordering,
//     or by anything that happens only in the real timing window, is out of
//     scope by construction. In particular the fonts are already in the browser
//     cache when the flash state is reconstructed, so this measures the fold
//     losing its @font-face DECLARATIONS, not a font still on the wire.
//   * No @import in the fixture. The captured stylesheet has none, so a flash
//     caused by a nested import chain is not exercised here.
//   * Layout established by JS is not covered. The page's islands do not run
//     against the fixture (their bundles are deliberately not captured), so
//     anything the fold's geometry owes to script is absent from both
//     screenshots equally.
//   * The measured configuration is not one that ships (see WORKER_ARGV below):
//     deferral is forced past the sufficiency gate so that there is deferred
//     markup to render at all.
//
// The comparison mirrors VisualRegressionGate::CompareScreenshots
// (src/browser/visual_regression_gate.{h,cc}) exactly: a pixel differs when any
// channel differs by more than kChannelTolerance = 2, and the verdict is the
// differing-pixel ratio against kDefaultThreshold = 0.005. Lane and product
// agree by construction; when PR-C settles on a measured threshold, THRESHOLD
// below moves with it.
//
// Node rather than Python because the pinned Playwright image ships Node and its
// browsers but no pip — see README.md.
//
// ADVISORY: this never gates a merge. A cross-Chrome-version pixel diff on a
// required check is a flake generator. Its output is an artifact plus a findings
// file the workflow turns into one tracking issue.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { chromium } from "playwright";
import { PNG } from "pngjs";

const BASE_URL = process.env.PROBE_BASE_URL ?? "http://nginx:8080";
const ORIGIN_URL = process.env.PROBE_ORIGIN_URL ?? "http://origin:8081";
const GOLDEN_DIR = process.env.PROBE_GOLDEN_DIR ?? "goldens";
const OUT_DIR = process.env.PROBE_OUT_DIR ?? "/tmp/async-css-probe-out";
const FINDINGS = process.env.PROBE_FINDINGS ?? join(OUT_DIR, "findings.md");
const REGENERATE = process.env.ASYNC_CSS_PROBE_REGENERATE_GOLDENS === "1";
// Recorded verbatim in the findings file so the auto-filed issue says which
// worker configuration produced the numbers. This lane deliberately measures a
// configuration that never ships.
const WORKER_ARGV =
  process.env.PROBE_WORKER_ARGV ??
  "--unsafe-force-async-css --async-css-min-coverage 0.95";

// The deferral primitive, shared with the markup lane. PR-E edits that one file
// and both lanes record the swap.
const PRIMITIVE_PATH =
  process.env.PROBE_PRIMITIVE ?? join(GOLDEN_DIR, "..", "primitive.json");
const PRIMITIVE = JSON.parse(readFileSync(PRIMITIVE_PATH, "utf-8"));
const DEFERRED_LINK_SELECTOR = `link[${PRIMITIVE.deferred_link_marker}]`;
const PRIMITIVE_ATTR = PRIMITIVE.primitive_attr;
const PRIMITIVE_VALUE = PRIMITIVE.primitive_value;
const SHEET_PATH = PRIMITIVE.fixture_stylesheet_path;

// src/browser/browser_analysis_manager.h — AnalysisContext::kViewportWidths /
// kViewportHeights. The profile is stored per viewport, so the fold has to be
// checked at each one.
//
// Each viewport is also a DEVICE CLASS, and the front end serves each class
// its own variant with its own critical block: the class comes from the
// User-Agent (ParseViewport in lib/classify/capability_mask.cc). With one
// desktop UA for all three, every viewport was served the desktop block and
// the mobile and tablet blocks were never measured. So:
//
//   mobile   Chrome on an Android phone: "Android" + "Mobile"  -> kMobile
//   tablet   Chrome on an Android tablet: "Android", no
//            "Mobile"                                          -> kTablet
//   desktop  Playwright's own (desktop Linux) UA, unchanged     -> kDesktop
//
// iPadOS Safari is not used for the tablet: it sends a desktop "Macintosh"
// UA, which lands in kDesktop. Since User-Agent emulation the product's renders
// send
// the same phone and tablet UAs (Emulation.setUserAgentOverride), and both
// lanes read them from device_emulation.json's `user_agent_overrides`: a
// mobile viewport at most `phone_max_width` px wide is the phone, a wider one
// the tablet. applyDeviceOverrides repeats the product's exact call (UA
// string, navigator.platform and client hints) on a CDP session of the page,
// and assertDeviceParity checks what the served page sees.
//
// Layout mode is the product's own: every analysis render sets its device
// emulation from src/browser/device_emulation.json (through
// src/browser/device_emulation.h, which a unit test holds equal to the
// file), and so does this lane, from the same file. A viewport at most
// `mobile_max_width` px wide renders as a mobile device (`isMobile`), wider
// as a desktop window. With `isMobile`, Chromium honours the page's meta
// viewport and grows the layout viewport to the document's width when
// content overflows, so an overflow anywhere in the document shifts the fold
// and moves position:fixed elements, as on a phone or a tablet in portrait.
// Without it the overflow only adds a scrollbar outside the screenshot:
// the case was an unsized icon 13,000 px down widening the flash-state document
// to 443 px, invisible in a desktop-mode render at 375 px. Since the
// tablet-as-mobile change
// the tablet (768 px) renders as a mobile device too, in the product and
// here.
//
// A mobile device is a touch device when `mobile_has_touch`:
// `hasTouch` here, Emulation.setTouchEmulationEnabled in the product, so
// `(hover: none)` and `(pointer: coarse)` match on the phone and the tablet
// and `(hover: hover)` / `(pointer: fine)` do not, as on a visitor's phone.
// Playwright's hasTouch sends that call without a touch-point count (one
// point); the product sends `max_touch_points`, so applyDeviceOverrides repeats
// the product's exact call on a CDP session of the page and the two renders
// report the same navigator.maxTouchPoints, and assertDeviceParity checks the
// served page sees it. The desktop has no touch.
//
// The file is read from the checkout (src/browser/ next to tools/), or from
// PROBE_DEVICE_EMULATION where only this directory is mounted (the nightly).
const DEVICE_EMULATION_PATH =
  process.env.PROBE_DEVICE_EMULATION ??
  join(GOLDEN_DIR, "..", "..", "..", "src", "browser", "device_emulation.json");
function readDeviceEmulation(path) {
  try {
    return JSON.parse(readFileSync(path, "utf-8"));
  } catch (err) {
    throw new Error(
      `cannot read the device emulation file ${path} (${err.message}). ` +
        "Outside a checkout (e.g. with PROBE_GOLDEN_DIR pointing into a " +
        "container mount) set PROBE_DEVICE_EMULATION to the mounted " +
        "src/browser/device_emulation.json.",
    );
  }
}
const DEVICE_EMULATION = readDeviceEmulation(DEVICE_EMULATION_PATH);
for (const key of [
  "mobile_max_width",
  "mobile_has_touch",
  "max_touch_points",
  "phone_max_width",
  "user_agent_overrides",
]) {
  if (!(key in DEVICE_EMULATION)) {
    throw new Error(
      `${DEVICE_EMULATION_PATH} has no "${key}": the probe reads the same ` +
        "fields device_emulation_test holds the product to; a renamed or " +
        "missing field must fail here, not emulate something else.",
    );
  }
}
// src/browser/device_emulation.h UserAgentOverrideParams: the phone's or the
// tablet's Emulation.setUserAgentOverride params, null for a desktop window.
function userAgentOverride(width) {
  if (width > DEVICE_EMULATION.mobile_max_width) return null;
  const overrides = DEVICE_EMULATION.user_agent_overrides;
  const params =
    width <= DEVICE_EMULATION.phone_max_width
      ? overrides.phone
      : overrides.tablet;
  if (!params || !params.userAgent) {
    throw new Error(
      `${DEVICE_EMULATION_PATH}: no user agent override for a ${width} px ` +
        "mobile viewport.",
    );
  }
  return params;
}
// Playwright's options for the device at `width`.
function emulate(width) {
  const isMobile = width <= DEVICE_EMULATION.mobile_max_width;
  const device = {
    isMobile,
    hasTouch: isMobile && DEVICE_EMULATION.mobile_has_touch,
  };
  const ua = userAgentOverride(width);
  if (ua) device.userAgent = ua.userAgent;
  return device;
}
// The product's touch and user agent calls, verbatim, on a page Playwright
// made with hasTouch / userAgent (src/browser/device_emulation.h
// TouchEmulationParams, UserAgentOverrideParams): same `enabled`, same
// `maxTouchPoints`, and the same UA string, navigator.platform and client
// hints (Playwright's userAgent sets only the string). A no-op for the
// desktop. The session is left attached for the page's life on purpose:
// Chromium reverts a session's emulation overrides when it detaches, and that
// took the touch state of the whole page with it (hover: hover,
// maxTouchPoints 0; Chromium 145). The page's close() ends it.
async function applyDeviceOverrides(page, vp) {
  if (!vp.device.hasTouch && !vp.uaOverride) return;
  const cdp = await page.context().newCDPSession(page);
  if (vp.device.hasTouch) {
    await cdp.send("Emulation.setTouchEmulationEnabled", {
      enabled: true,
      maxTouchPoints: DEVICE_EMULATION.max_touch_points,
    });
  }
  if (vp.uaOverride) {
    await cdp.send("Emulation.setUserAgentOverride", vp.uaOverride);
  }
}
// What the served page sees must be what the product's renders see for this
// class, or the lane measures a fold the product never validated: a touch
// viewport reports (hover: none), (pointer: coarse), their any-* forms (the
// critical-CSS extractor decides on all four), the product's touch points
// and `ontouchstart` (the page is navigated to, so its window postdates the
// emulation, as the product's fresh windows do since User-Agent emulation); the
// desktop
// reports none of them. The phone and the tablet report the product's UA
// string and navigator.platform, and, where the page is a secure context
// (navigator.userAgentData exists only there; the nightly serves plain
// http), its client hints' `mobile` and `platform`. The desktop keeps a
// desktop UA. Read after the navigation, on the document the fold is
// measured in.
async function assertDeviceParity(page, vp) {
  const seen = await page.evaluate(() => ({
    hoverNone: matchMedia("(hover: none)").matches,
    pointerCoarse: matchMedia("(pointer: coarse)").matches,
    anyHoverNone: matchMedia("(any-hover: none)").matches,
    anyPointerCoarse: matchMedia("(any-pointer: coarse)").matches,
    maxTouchPoints: navigator.maxTouchPoints,
    ontouchstart: "ontouchstart" in window,
    userAgent: navigator.userAgent,
    platform: navigator.platform,
    uaDataMobile: navigator.userAgentData
      ? navigator.userAgentData.mobile
      : undefined,
    uaDataPlatform: navigator.userAgentData
      ? navigator.userAgentData.platform
      : undefined,
  }));
  const want = {
    hoverNone: vp.device.hasTouch,
    pointerCoarse: vp.device.hasTouch,
    anyHoverNone: vp.device.hasTouch,
    anyPointerCoarse: vp.device.hasTouch,
    maxTouchPoints: vp.device.hasTouch ? DEVICE_EMULATION.max_touch_points : 0,
    ontouchstart: vp.device.hasTouch,
  };
  const ua = vp.uaOverride;
  if (ua) {
    want.userAgent = ua.userAgent;
    want.platform = ua.platform;
    if (seen.uaDataMobile !== undefined) {
      want.uaDataMobile = ua.userAgentMetadata.mobile;
      want.uaDataPlatform = ua.userAgentMetadata.platform;
    }
  } else if (/Android|Mobile|iPhone|iPad|Tablet/i.test(seen.userAgent)) {
    throw new InfraError(
      `${vp.name}: the page reports the user agent ${seen.userAgent}, which ` +
        "the front end does not classify as a desktop; the product's desktop " +
        "render sends no override.",
    );
  }
  for (const key of Object.keys(want)) {
    if (seen[key] !== want[key]) {
      throw new InfraError(
        `${vp.name}: the page reports ${key}=${seen[key]}, the product's ` +
          `render has ${want[key]} (device_emulation.json): the device ` +
          "emulation of this lane and the product differ.",
      );
    }
  }
}
function viewport(name, width, height) {
  return {
    name,
    width,
    height,
    device: emulate(width),
    uaOverride: userAgentOverride(width),
  };
}
const VIEWPORTS = [
  viewport("mobile", 375, 667),
  viewport("tablet", 768, 1024),
  viewport("desktop", 1440, 900),
];

// src/browser/visual_regression_gate.h.
const CHANNEL_TOLERANCE = 2;
const THRESHOLD = 0.005;

// Cold-stack convergence was measured at ~60s on an idle machine, and the
// nightly always starts cold. 180s is the same margin the workflows give
// readiness — generous on purpose, because the alternative is a false finding.
const WARM_BUDGET_MS = Number(process.env.PROBE_WARM_BUDGET_MS ?? 180_000);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Raised for anything that means "this lane did not measure the fold". Kept
// distinct from a product finding all the way to the findings file: an
// infrastructure failure is not evidence about the fold, and reporting it as one
// sends the reader hunting a rendering bug that was never measured.
class InfraError extends Error {}

function compare(goldenPath, candidatePath) {
  const a = PNG.sync.read(readFileSync(goldenPath));
  const b = PNG.sync.read(readFileSync(candidatePath));
  const w = Math.min(a.width, b.width);
  const h = Math.min(a.height, b.height);
  // A zero-size image means a screenshot or a golden failed to capture. Ratio 0
  // would read as a perfect match and quietly clear the lane.
  if (w === 0 || h === 0) {
    throw new InfraError(
      `refusing to compare a zero-size image: ${goldenPath} is ` +
        `${a.width}x${a.height}, ${candidatePath} is ${b.width}x${b.height}`,
    );
  }
  let diff = 0;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const ia = (y * a.width + x) * 4;
      const ib = (y * b.width + x) * 4;
      for (let c = 0; c < 4; c++) {
        if (Math.abs(a.data[ia + c] - b.data[ib + c]) > CHANNEL_TOLERANCE) {
          diff++;
          break;
        }
      }
    }
  }
  return { ratio: diff / (w * h), diff, total: w * h };
}

// Warm ONCE, before any viewport is measured. Without it the FIRST viewport
// spends its whole budget warming and reports "no deferred stylesheet" — on the
// cold stack the nightly always produces, a false finding every night.
async function prewarm(browser) {
  const deadline = Date.now() + WARM_BUDGET_MS;

  // Warm over plain HTTP, in the markup lane's order and for its reasons
  // (conftest.py): the stylesheet must be cached before the page is first
  // optimized, and "X-PageSpeed: HIT" is not "the optimizer has run" — the
  // front end serves the origin's bytes from cache while the worker optimizes
  // out of band, so wait for output that is not the origin's bytes.
  //
  // Plain HTTP rather than the browser, which is the whole finding here: the
  // optimizer produces one variant per capability mask, and on a cold stack the
  // browser's own repeated navigation does NOT converge — measured flat at
  // origin bytes for a full 180s. Warm this way first and the browser is served
  // the deferred page on its FIRST navigation, in under two seconds. Warming
  // through the browser alone would file a false finding every night.
  //
  // Once per device class: each viewport's UA is its own variant (see
  // VIEWPORTS), so warming one class warms nothing the others are served. The
  // desktop entry sends node's own UA over plain HTTP, which classifies as
  // desktop like the browser's.
  const originHtml = Buffer.from(
    await (await fetch(`${ORIGIN_URL}/`)).arrayBuffer(),
  );
  for (const vp of VIEWPORTS) {
    const headers = vp.device.userAgent
      ? { "user-agent": vp.device.userAgent }
      : {};
    let sheetHit = false;
    while (Date.now() < deadline) {
      const r = await fetch(`${BASE_URL}${SHEET_PATH}`, { headers });
      await r.arrayBuffer();
      if (r.headers.get("x-pagespeed") === "HIT") {
        sheetHit = true;
        break;
      }
      await sleep(250);
    }
    if (!sheetHit) {
      throw new InfraError(
        `${vp.name}: the stylesheet was never served from cache within ` +
          `${WARM_BUDGET_MS / 1000}s.`,
      );
    }

    let served = null;
    while (Date.now() < deadline) {
      const r = await fetch(`${BASE_URL}/`, { headers });
      const body = Buffer.from(await r.arrayBuffer());
      if (r.headers.get("x-pagespeed") === "HIT" && !body.equals(originHtml)) {
        served = body;
        break;
      }
      await sleep(250);
    }
    if (served === null) {
      throw new InfraError(
        `${vp.name}: the page was never served as optimizer output within ` +
          `${WARM_BUDGET_MS / 1000}s.`,
      );
    }
    if (!served.includes(PRIMITIVE.deferred_link_marker)) {
      throw new InfraError(
        `${vp.name}: the optimized page carries no deferred stylesheet. The ` +
          "stack is not in the forced mode (README.md).",
      );
    }
  }

  // Confirm through the browser before any measurement: the mask the browser
  // is served must be the deferred one too, for every device class.
  for (const vp of VIEWPORTS) {
    const page = await browser.newPage({
      viewport: { width: vp.width, height: vp.height },
      ...vp.device,
    });
    await applyDeviceOverrides(page, vp);
    let confirmed = false;
    try {
      while (Date.now() < deadline) {
        await page.goto(`${BASE_URL}/`, { waitUntil: "load" });
        const deferred = await page.evaluate(
          (sel) => document.querySelectorAll(sel).length,
          DEFERRED_LINK_SELECTOR,
        );
        if (deferred > 0) {
          confirmed = true;
          break;
        }
        await sleep(1000);
      }
    } finally {
      await page.close();
    }
    if (!confirmed) {
      throw new InfraError(
        `${vp.name}: the front end serves a deferred page over plain HTTP but ` +
          `not to the browser within ${WARM_BUDGET_MS / 1000}s — a ` +
          "capability-mask variant the optimizer never produced.",
      );
    }
  }
}

// Now that the fixture serves real fonts, a screenshot can land mid-swap: the
// fold drawn in fallback metrics, one frame before the webfont applies. Against
// a golden taken after the swap that is a large diff and reads as exactly the
// flash this lane hunts — a false finding manufactured by timing. Waiting on
// document.fonts.ready removes the race in the only dimension it exists.
//
// Bounded and best-effort. document.fonts.ready settles on its own in every
// state this lane produces, including the reconstructed one where the
// @font-face rules went away with the sheet — but page.evaluate carries no
// timeout of its own, and a lane that can hang forever is worse than one that
// screenshots a frame early. The cap is the whole reason this is a helper and
// not an inline await.
const FONT_SETTLE_CAP_MS = 5_000;

async function settleFonts(page) {
  let timer;
  try {
    await Promise.race([
      page
        .evaluate(() => document.fonts.ready.then(() => undefined))
        .catch(() => {}),
      // Cleared below rather than left to fire: an un-cleared timer holds the
      // event loop open, and this runs six times per lane.
      new Promise((r) => {
        timer = setTimeout(r, FONT_SETTLE_CAP_MS);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function runViewport(browser, vp) {
  const golden = join(GOLDEN_DIR, `fold-${vp.name}.png`);
  const during = join(OUT_DIR, `critical-only-${vp.name}.png`);
  const after = join(OUT_DIR, `full-css-${vp.name}.png`);

  const page = await browser.newPage({
    viewport: { width: vp.width, height: vp.height },
    // The device class this viewport stands for (see VIEWPORTS): its UA picks
    // the variant, and isMobile + hasTouch (phone and tablet, from
    // device_emulation.json) lay it out, and evaluate hover/pointer media
    // queries, the way those devices do.
    ...vp.device,
    deviceScaleFactor: 1,
    // The capture is a dark-mode-only site; pin the preference so a runner
    // default can never move the fold's colours out from under the golden.
    colorScheme: "dark",
    // The captured logo SVG animates a blinking cursor (1.2s, steps(1)) and
    // runs it inside <img>. Left free it lands on or off per screenshot, so
    // every diff here carries a few dozen pixels of coin-flip. The SVG honours
    // prefers-reduced-motion itself — animation: none, opacity fixed — so
    // pinning the preference makes the fold deterministic rather than
    // approximately deterministic. Both screenshots and the golden are taken
    // under it, so nothing about the comparison is one-sided.
    reducedMotion: "reduce",
  });
  await applyDeviceOverrides(page, vp);

  try {
    // Count on the measured navigation only. A preload whose `as` does not match
    // its consumer downloads the sheet twice; that is the risk PR-E takes on,
    // and this is where it would show.
    let sheetRequests = 0;
    page.on("request", (r) => {
      if (r.url().includes(SHEET_PATH)) sheetRequests++;
    });
    await page.goto(`${BASE_URL}/`, { waitUntil: "load" });
    await page.waitForTimeout(500);
    await assertDeviceParity(page, vp);
    await settleFonts(page);

    const deferred = await page.evaluate(
      (sel) => document.querySelectorAll(sel).length,
      DEFERRED_LINK_SELECTOR,
    );
    if (deferred === 0) {
      throw new InfraError(
        `${vp.name}: the served page carried no deferred stylesheet even after ` +
          "the pre-warm succeeded — the variant served here differs.",
      );
    }

    // Reference: the loader has run and the full stylesheet applies.
    if (REGENERATE) {
      mkdirSync(GOLDEN_DIR, { recursive: true });
      await page.screenshot({ path: golden });
      return { name: vp.name, regenerated: golden };
    }
    await page.screenshot({ path: after });

    // Candidate: the same document with the deferred sheet not applying.
    // The return value is the self-check: this reconstruction only means
    // anything if setting the primitive actually DETACHES the sheet. Should a
    // future primitive stop doing that, the mutation becomes a no-op and this
    // lane silently degrades into comparing the golden against an identical
    // render — permanently, quietly green. Count the applied sheets either
    // side of the mutation and refuse to report on a no-op.
    const sheetsBefore = await page.evaluate(() => document.styleSheets.length);
    await page.evaluate(
      ({ sel, attr, value }) => {
        for (const l of document.querySelectorAll(sel)) l.setAttribute(attr, value);
      },
      { sel: DEFERRED_LINK_SELECTOR, attr: PRIMITIVE_ATTR, value: PRIMITIVE_VALUE },
    );
    const sheetsAfter = await page.evaluate(() => document.styleSheets.length);
    if (sheetsAfter >= sheetsBefore) {
      throw new InfraError(
        `${vp.name}: setting ${PRIMITIVE_ATTR}="${PRIMITIVE_VALUE}" did not ` +
          `detach any stylesheet (${sheetsBefore} -> ${sheetsAfter}). The ` +
          'un-applying reconstruction is a no-op, so "during" would be the ' +
          'same render as "after" and this lane would pass vacuously. Update ' +
          'primitive.json / this reconstruction for the current primitive.',
      );
    }
    await page.waitForTimeout(300);
    await settleFonts(page);
    await page.screenshot({ path: during });

    if (!existsSync(golden)) {
      throw new InfraError(
        `${vp.name}: missing golden ${golden} — regenerate inside the pinned ` +
          'image (README.md, "Golden screenshots").',
      );
    }

    const findings = [];
    const full = compare(golden, after);
    const crit = compare(golden, during);

    // Checked first: a stale golden makes the flash number meaningless, and
    // reporting it as a flash sends someone hunting a product bug that is
    // really a fixture refresh.
    if (full.ratio > THRESHOLD) {
      findings.push(
        `**${vp.name}**: the fully-styled render diverges from the golden ` +
          `(${full.ratio.toFixed(5)} > ${THRESHOLD}) — the golden is stale, or ` +
          "the page changed. Fix that before reading the flash result.",
      );
    } else if (crit.ratio > THRESHOLD) {
      findings.push(
        `**${vp.name}**: flash of unstyled content — ${crit.diff}/${crit.total} ` +
          `pixels (${crit.ratio.toFixed(5)}) differ from the fully-styled fold ` +
          `while the deferred stylesheet has not applied (threshold ${THRESHOLD}).`,
      );
    }

    if (sheetRequests !== 1) {
      findings.push(
        `**${vp.name}**: the deferred stylesheet was requested ${sheetRequests} ` +
          "times (expected exactly 1).",
      );
    }

    return { name: vp.name, full, crit, sheetRequests, findings };
  } finally {
    await page.close();
  }
}

function renderFindings(product, infra) {
  if (!product.length && !infra.length) return "";
  let out = "";
  if (infra.length) {
    out +=
      "### Rendered probe did not reach a verdict (infrastructure)\n\n" +
      infra.map((f) => `- ${f}`).join("\n") +
      "\n\n";
  }
  if (product.length) {
    out +=
      "### Async-CSS rendered probe findings\n\n" +
      `Measured against a worker started with \`${WORKER_ARGV}\` — a ` +
      "configuration that never ships. Deferral is forced past the sufficiency " +
      "gate so that there is deferred markup to render at all. The fixture " +
      "origin serves the fold's web fonts and logos, so a flash caused by the " +
      "deferred sheet taking its @font-face rules with it DOES show up in " +
      "these numbers — read them with that in mind rather than as a pure " +
      "layout diff.\n\n" +
      product.map((f) => `- ${f}`).join("\n") +
      "\n";
  }
  return out;
}

async function main() {
  mkdirSync(OUT_DIR, { recursive: true });
  const product = [];
  const infra = [];
  const results = [];

  const browser = await chromium.launch({ args: ["--force-color-profile=srgb"] });
  try {
    await prewarm(browser);
    for (const vp of VIEWPORTS) {
      try {
        results.push(await runViewport(browser, vp));
      } catch (e) {
        if (!(e instanceof InfraError)) throw e;
        infra.push(e.message);
      }
    }
  } catch (e) {
    if (!(e instanceof InfraError)) throw e;
    infra.push(e.message);
  } finally {
    await browser.close();
  }

  if (REGENERATE) {
    for (const r of results) console.log(`regenerated ${r.regenerated}`);
    for (const f of infra) console.error(`INFRA: ${f}`);
    process.exitCode = infra.length ? 1 : 0;
    return;
  }

  for (const r of results) {
    if (!r.crit) continue;
    console.log(
      `${r.name}: critical-only=${r.crit.ratio.toFixed(5)} ` +
        `full-css=${r.full.ratio.toFixed(5)} sheet-requests=${r.sheetRequests} ` +
        `(threshold ${THRESHOLD})`,
    );
    product.push(...r.findings);
  }
  for (const f of infra) console.error(`INFRA: ${f}`);

  writeFileSync(FINDINGS, renderFindings(product, infra));
  if (product.length || infra.length) {
    console.error(
      `\n${product.length} finding(s), ${infra.length} infrastructure ` +
        `failure(s) written to ${FINDINGS}`,
    );
    // Non-zero so a local run is obvious. The nightly workflow captures this
    // rather than failing on it: the tracking issue is the signal.
    process.exitCode = 1;
  }
}

await main();
