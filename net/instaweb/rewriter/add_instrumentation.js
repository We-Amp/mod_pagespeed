/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @fileoverview Real-user-monitoring collector for the
 * AddInstrumentationFilter.  Gathers Core Web Vitals (LCP, session-window
 * CLS, INP) through PerformanceObserver plus Navigation Timing Level 2
 * data, and reports them in a single navigator.sendBeacon POST when the
 * page is hidden or unloaded.  Web APIs are accessed with bracket notation
 * throughout so the Closure Compiler's ADVANCED mode cannot rename them.
 */

// Exporting functions using quoted attributes to prevent js compiler from
// renaming them.
// See https://developers.google.com/closure/compiler/docs/api-tutorial3#dangers
window['pagespeed'] = window['pagespeed'] || {};
var pagespeed = window['pagespeed'];

/**
 * @constructor
 * @param {string} beaconUrlPrefix The prefix portion of the beacon url.
 * @param {string} extraParams Additional parameters to be added to the beacon.
 * @param {string} htmlUrl Url of the page the beacon is being inserted on.
 */
pagespeed.AddInstrumentation = function(beaconUrlPrefix, extraParams,
                                        htmlUrl) {
  this.beaconUrlPrefix_ = beaconUrlPrefix;
  this.extraParams_ = extraParams;
  this.htmlUrl_ = htmlUrl;
  /** @private {boolean} Whether the beacon has been sent (exactly-once). */
  this.sent_ = false;
  /** @private {number} Latest LCP candidate in ms, -1 if none observed. */
  this.lcpMs_ = -1;
  /** @private {!Object} Current CLS session window. */
  this.clsSession_ = {sum: 0, firstTime: 0, lastTime: 0};
  /** @private {number} Max session-window CLS observed so far. */
  this.cls_ = 0;
  /** @private {boolean} Whether the layout-shift observer is installed. */
  this.clsObserved_ = false;
  /** @private {number} Worst interaction duration in ms, -1 if none. */
  this.inpMs_ = -1;
  /** @private {!Array<!Object>} Installed observers plus their handlers. */
  this.observers_ = [];
  this.initObservers_();
};

/**
 * Registers a buffered PerformanceObserver for a single entry type.
 * Buffering is essential: this script runs at the end of the body, after
 * the earliest paint and layout-shift entries were dispatched.  Failures
 * are contained per type so support degrades per-metric on browsers that
 * lack an entry type (older Safari throws on unsupported types).
 * @param {string} type The performance entry type to observe.
 * @param {function(!Array<!Object>)} handler Receives observed entries.
 * @param {Object=} opt_extra Extra options for the observe() call.
 * @return {boolean} Whether the observer was installed.
 * @private
 */
pagespeed.AddInstrumentation.prototype.observe_ = function(type, handler,
                                                           opt_extra) {
  var PO = window['PerformanceObserver'];
  if (!PO) {
    return false;
  }
  // Gate on supportedEntryTypes: some browsers accept the observe() options
  // dict but silently never deliver entries for unsupported types (e.g.
  // layout-shift), which would otherwise make the metric read as a
  // legitimate zero instead of "not measured".
  if (!PO['supportedEntryTypes'] ||
      PO['supportedEntryTypes'].indexOf(type) == -1) {
    return false;
  }
  try {
    var observer = new PO(function(list) { handler(list['getEntries']()); });
    var opts = opt_extra || {};
    opts['type'] = type;
    opts['buffered'] = true;
    observer['observe'](opts);
    this.observers_.push({observer: observer, handler: handler});
    return true;
  } catch (e) {
    return false;
  }
};

/**
 * Installs the Core Web Vitals observers.
 * @private
 */
pagespeed.AddInstrumentation.prototype.initObservers_ = function() {
  var that = this;

  // Largest Contentful Paint: the last entry emitted wins.
  this.observe_('largest-contentful-paint', function(entries) {
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      that.lcpMs_ =
          Math.round(e['renderTime'] || e['loadTime'] || e['startTime']);
    }
  });

  // Cumulative Layout Shift, session-window variant: shifts within a
  // window of at most 5s, with gaps of at most 1s, accumulate; the value
  // reported is the worst window.  Shifts right after user input are
  // excluded, as in the CLS definition.
  this.clsObserved_ = this.observe_('layout-shift', function(entries) {
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      if (e['hadRecentInput']) {
        continue;
      }
      var s = that.clsSession_;
      if (s.sum > 0 && e['startTime'] - s.lastTime < 1000 &&
          e['startTime'] - s.firstTime < 5000) {
        s.sum += e['value'];
        s.lastTime = e['startTime'];
      } else {
        s.sum = e['value'];
        s.firstTime = s.lastTime = e['startTime'];
      }
      if (s.sum > that.cls_) {
        that.cls_ = s.sum;
      }
    }
  });

  // Interaction latency: the worst qualifying interaction duration.  This
  // deliberately reports the maximum rather than the 98th-percentile
  // estimator the INP metric specifies; for typical page views (fewer than
  // 50 interactions) the two are identical.
  var inpHandler = function(entries) {
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      // 'event' entries without an interactionId are not discrete
      // interactions (e.g. plain mousemoves); skip them.  'first-input'
      // entries always qualify.
      if (e['entryType'] == 'event' && !e['interactionId']) {
        continue;
      }
      var duration = Math.round(e['duration']);
      if (duration > that.inpMs_) {
        that.inpMs_ = duration;
      }
    }
  };
  this.observe_('event', inpHandler, {'durationThreshold': 40});
  // Fallback so a single early interaction is still captured on browsers
  // without event-timing support; merged through the same max.
  this.observe_('first-input', inpHandler);
};

/**
 * Re-arms measurement after a back/forward-cache restore.  Hiding the page
 * latched the exactly-once guard and disconnected the observers, so without
 * this a restored visit would beacon nothing and contribute no LCP/CLS/INP.
 * Per web-vitals guidance the restore is a new page view: reset the
 * accumulation state, re-install the observers, and let the next hide beacon
 * again.  Degrades exactly like the constructor: without PerformanceObserver
 * no observers come back, and the restored visit still re-beacons its
 * Navigation Timing subset.
 *
 * Note: the re-installed observers are created with buffered:true (see
 * observe_), so Chromium immediately re-delivers pre-freeze entries into the
 * fresh state.  The restored visit's LCP therefore floors at the frozen
 * visit's value (semantically fine: the content is identical and no new LCP
 * can fire after the first hide), and its CLS/INP start from the pre-freeze
 * values rather than zero -- per-visit beacon approximation we accept, since
 * entry timestamps cannot distinguish cache epochs.
 * @private
 */
pagespeed.AddInstrumentation.prototype.rearm_ = function() {
  // Normally send_ already disconnected everything when the page hid; close
  // any observer that is somehow still live so entries are never delivered
  // twice into the fresh accumulation state below.
  for (var i = 0; i < this.observers_.length; i++) {
    try {
      this.observers_[i].observer['disconnect']();
    } catch (e) {
    }
  }
  this.observers_ = [];
  this.sent_ = false;
  this.lcpMs_ = -1;
  this.clsSession_ = {sum: 0, firstTime: 0, lastTime: 0};
  this.cls_ = 0;
  this.inpMs_ = -1;
  this.initObservers_();
};

/**
 * Assembles the beacon POST body as a query-parameter string.  Always
 * returns a non-empty string: some server frontends reject a POST with an
 * empty body.
 * @return {string} The beacon body.
 * @private
 */
pagespeed.AddInstrumentation.prototype.buildBody_ = function() {
  var parts = [];
  var perf = window['performance'];
  var navEntry = null;
  if (perf && perf['getEntriesByType']) {
    var navEntries = perf['getEntriesByType']('navigation');
    if (navEntries && navEntries.length > 0) {
      navEntry = navEntries[0];
    }
  }
  if (navEntry) {
    var loadMs = Math.round(navEntry['loadEventStart']);
    if (loadMs > 0) {
      // Page load time.  Omitted when the page is hidden before onload
      // fires; the server treats it as optional.
      parts.push('ets=load:' + loadMs);
      parts.push('rload=' + loadMs);
    }
    parts.push('nav=' + Math.round(navEntry['fetchStart']));
    parts.push('dns=' + Math.round(navEntry['domainLookupEnd'] -
                                   navEntry['domainLookupStart']));
    parts.push('connect=' + Math.round(navEntry['connectEnd'] -
                                       navEntry['connectStart']));
    parts.push('req_start=' + Math.round(navEntry['requestStart']));
    // c_ttfb (client TTFB): absolute responseStart from navigation start.
    // Deliberately a new name: the legacy ttfb param carried
    // responseStart - requestStart and the server ignores it.
    parts.push('c_ttfb=' + Math.round(navEntry['responseStart']));
    parts.push('dwld=' + Math.round(navEntry['responseEnd'] -
                                    navEntry['responseStart']));
    parts.push('dom_c=' + Math.round(navEntry['domContentLoadedEventStart']));
    if (navEntry['type']) {
      parts.push('nt=' + navEntry['type']);
    }
  } else if (window['mod_pagespeed_start']) {
    // No Navigation Timing support at all: report the elapsed time since
    // the timing script at the top of the head ran.  Note this is measured
    // at hide-time, not at the load event, so it overstates load time by
    // however long the page stayed open.
    parts.push('ets=load:' +
               (Number(new Date()) - window['mod_pagespeed_start']));
  }
  if (this.lcpMs_ >= 0) {
    parts.push('lcp=' + this.lcpMs_);
  }
  if (this.clsObserved_) {
    // Fixed-point milli-units keep the value an integer end-to-end: a CLS
    // of 0.1 is reported as cls=100.  Zero is meaningful, so the param is
    // sent whenever the observer was installed.
    parts.push('cls=' + Math.round(this.cls_ * 1000));
  }
  if (this.inpMs_ >= 0) {
    parts.push('inp=' + this.inpMs_);
  }
  // Collect devicePixelRatios to find common values.
  // Note: This may append =undefined for old browsers; it also guarantees
  // the body is never empty.
  parts.push('dpr=' + window.devicePixelRatio);
  var body = parts.join('&');
  if (this.extraParams_ != '') {
    body += this.extraParams_;
  }
  if (pagespeed['getResourceTimingData']) {
    body += pagespeed['getResourceTimingData']();
  }
  if (document.referrer) {
    body += '&ref=' + encodeURIComponent(document.referrer);
  }
  return body;
};

/**
 * Flushes the observers and sends the beacon, exactly once.  Prefers
 * navigator.sendBeacon (survives page dismissal); falls back to the legacy
 * image GET with the body appended to the query string.
 * @private
 */
pagespeed.AddInstrumentation.prototype.send_ = function() {
  if (this.sent_) {
    return;
  }
  this.sent_ = true;
  // Flush entries queued but not yet delivered to observer callbacks, then
  // stop observing.
  for (var i = 0; i < this.observers_.length; i++) {
    var o = this.observers_[i];
    try {
      o.handler(o.observer['takeRecords']());
      o.observer['disconnect']();
    } catch (e) {
    }
  }
  var body = this.buildBody_();
  var url = this.beaconUrlPrefix_;
  // Handle a beacon url that already has query params.  The page URL stays
  // in the beacon URL's query string (not the body) so it shows up in
  // access logs and load-balancer routing.
  url += (url.indexOf('?') == -1) ? '?' : '&';
  url += 'url=' + encodeURIComponent(this.htmlUrl_);
  var nav = window.navigator;
  if (nav && nav['sendBeacon']) {
    try {
      if (nav['sendBeacon'](url, body)) {
        return;
      }
    } catch (e) {
    }
  }
  new Image().src = url + '&' + body;
};

/**
 * Initialize instrumentation beacon.
 * @param {string} beaconUrl Url of beacon.
 * @param {string} extraParams Additional parameters to be added to the beacon.
 * @param {string} htmlUrl Url of the page the beacon is being inserted on.
 */
pagespeed.addInstrumentationInit = function(beaconUrl, extraParams, htmlUrl) {
  if (window.parent != window) {
    // Only top-level page views are reported; never run in iframes.
    return;
  }
  var collector = new pagespeed.AddInstrumentation(beaconUrl, extraParams,
                                                   htmlUrl);
  // Send when the page becomes hidden or is being unloaded.  Both fire on
  // navigation; the exactly-once guard collapses them to a single beacon.
  // Deliberately no 'beforeunload'/'unload' listeners: those disable the
  // back/forward cache.
  document.addEventListener('visibilitychange', function() {
    if (document['visibilityState'] == 'hidden') {
      collector.send_();
    }
  });
  window.addEventListener('pagehide', function() { collector.send_(); });
  window.addEventListener('pageshow', function(event) {
    // pageshow with persisted=true is a back/forward-cache restore: the
    // frozen page (with this collector, already sent and disconnected)
    // becomes the current visit again.  Re-arm so the restored visit is
    // measured and beaconed on its own hide.  Non-persisted pageshows
    // (initial load, ordinary navigations) must not reset the exactly-once
    // guard.
    if (event['persisted']) {
      collector.rearm_();
    }
  });
};

pagespeed['addInstrumentationInit'] = pagespeed.addInstrumentationInit;
