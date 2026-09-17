/*
 * Copyright 2013 Google Inc.
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
 * @fileoverview Code for detecting and sending to the server the critical CSS
 * selectors (selectors applying to any DOM elements on the page) on the client
 * side. To use, this script should be injected right before </body> with a call
 * to pagespeed.criticalCssBeaconInit(...) appended to it.
 *
 * @author jud@google.com (Jud Porter)
 */

goog.require('pagespeedutils');

// Exporting functions using quoted attributes to prevent js compiler from
// renaming them.
// See https://developers.google.com/closure/compiler/docs/api-tutorial3#dangers
window['pagespeed'] = window['pagespeed'] || {};
var pagespeed = window['pagespeed'];



/**
 * @constructor
 * @param {string} beaconUrl The URL on the server to send the beacon to.
 * @param {string} htmlUrl Url of the page the beacon is being inserted on.
 * @param {string} optionsHash The hash of the rewrite options. This is required
 *     to perform the property cache lookup when the beacon is handled by the
 *     sever.
 * @param {string} nonce The nonce sent by the server.
 * @param {Array.<string>} selectors List of the selectors on the page.
 */
pagespeed.CriticalCssBeacon = function(beaconUrl, htmlUrl, optionsHash,
                                       nonce, selectors) {
  /**
   * We divide up the main loop of checkCssSelectors into multiple calls with
   * window.setTimeout to minimize the delay in processing other events on the
   * page. This constant sets the number of candidate selectors we check in a
   * single call to checkCssSelectors.
   *
   * This value is choosen so that each iteration takes around ~100ms on a
   * modern mobile phone. On a nexus 4, each document.querySelector call was
   * measured to take around 400us on a complex page.
   *
   * @const
   * @private
   */
  this.MAXITERS_ = 250;

  /**
   * Cap on the number of matched elements whose geometry we measure for a
   * single selector when deciding above-the-fold-ness. Past the cap the
   * selector is kept (conservative).
   * @const
   * @private
   */
  this.MAXMEASURES_ = 10;

  this.beaconUrl_ = beaconUrl;
  this.htmlUrl_ = htmlUrl;
  this.optionsHash_ = optionsHash;
  this.nonce_ = nonce;
  this.selectors_ = selectors;
  this.criticalSelectors_ = [];
  this.idx_ = 0;
};


/**
 * Send the selectors that have been collected into the criticalSelectors_
 * member var back to the server.
 * @private
 */
pagespeed.CriticalCssBeacon.prototype.sendBeacon_ = function() {
  var data = 'oh=' + this.optionsHash_ + '&n=' + this.nonce_;
  data += '&cs=';
  // The candidate list is sorted, so starting at index 0 on every beacon
  // would starve the same tail selectors whenever the payload overflows.
  // Rotate the starting offset instead: support then accumulates across
  // beacons for every selector, just more slowly under overflow.
  var numSelectors = this.criticalSelectors_.length;
  var start = Math.floor(Math.random() * numSelectors);
  // Reserve room for the overflow flag appended below.
  var maxDataLen = pagespeedutils.MAX_POST_SIZE - '&of=1'.length;
  var truncated = false;
  for (var i = 0; i < numSelectors; ++i) {
    var tmp = (i > 0) ? ',' : '';
    tmp += encodeURIComponent(
        this.criticalSelectors_[(start + i) % numSelectors]);
    if ((data.length + tmp.length) > maxDataLen) {
      truncated = true;
      break;
    }
    data += tmp;
  }
  if (truncated) {
    // Tell the server we dropped data so the loss is observable there.
    data += '&of=1';
  }
  // Export the URL for testing purposes.
  pagespeed['criticalCssBeaconData'] = data;
  // TODO(jud): This beacon should coordinate with the add_instrumentation JS
  // so that only one beacon request is sent if both filters are enabled.
  pagespeedutils.sendBeacon(this.beaconUrl_, this.htmlUrl_, data);
};


/**
 * Decide whether a selector should count as critical: it must match a DOM
 * element, and at least one match must intersect the initial viewport ---
 * otherwise the "critical" set converges on "all CSS used anywhere on the
 * page", which for long pages defeats the purpose of inlining. The bias is
 * conservative: whenever above-the-fold-ness can't be determined (viewport
 * height unknown, element not currently rendered, more matches than we are
 * willing to measure), the selector is kept.
 * @param {string} selector The selector to check.
 * @return {boolean} True if the selector should gain critical support.
 * @private
 */
pagespeed.CriticalCssBeacon.prototype.isSelectorCritical_ = function(
    selector) {
  var elements = document.querySelectorAll(selector);
  if (elements.length == 0) {
    return false;
  }
  var viewportHeight =
      window.innerHeight || document.documentElement.clientHeight;
  if (!viewportHeight || !elements[0].getBoundingClientRect) {
    // Can't measure: keep the selector.
    return true;
  }
  // getBoundingClientRect is relative to the current scroll position, which
  // at onload need not be the top (anchors, restored scroll); adding the
  // scroll offset gives the document-absolute position the initial viewport
  // is compared against.
  var scrollY = window.pageYOffset || 0;
  var numToMeasure = Math.min(elements.length, this.MAXMEASURES_);
  for (var i = 0; i < numToMeasure; ++i) {
    var rect = elements[i].getBoundingClientRect();
    if (rect.width == 0 && rect.height == 0) {
      // Not rendered right now (e.g. display:none that script may toggle):
      // indeterminate, keep the selector.
      return true;
    }
    if (rect.top + scrollY < viewportHeight) {
      // Above the fold.
      return true;
    }
  }
  // Every measured match is below the fold; only matches past the measuring
  // cap remain unknown, and then we keep the selector.
  return elements.length > numToMeasure;
};


/**
 * Check if CSS selectors apply to DOM elements that are visible on initial page
 * load.
 * @param {Function} callback Function to call when calculating the selectors
 *     has finished.
 * @private
 */
pagespeed.CriticalCssBeacon.prototype.checkCssSelectors_ = function(callback) {
  for (var i = 0; i < this.MAXITERS_ && this.idx_ < this.selectors_.length;
       ++i, ++this.idx_) {
    try {
      if (this.isSelectorCritical_(this.selectors_[this.idx_])) {
        this.criticalSelectors_.push(this.selectors_[this.idx_]);
      }
    } catch (e) {
      // SYNTAX_ERR is thrown if the browser can't parse a selector (eg, CSS3 in
      // a CSS2.1 browser). Ignore these exceptions.
      // TODO(jud): Consider if continue is the right thing to do here. It may
      // be safer to mark this selector as critical if the browser didn't
      // understand it.
      continue;
    }
  }

  if (this.idx_ < this.selectors_.length) {
    window.setTimeout(this.checkCssSelectors_.bind(this), 0, callback);
  } else {
    callback();
  }
};


/**
 * Initialize.
 * @param {string} beaconUrl The URL on the server to send the beacon to.
 * @param {string} htmlUrl Url of the page the beacon is being inserted on.
 * @param {string} optionsHash The hash of the rewrite options.
 * @param {string} nonce The nonce sent by the server.
 * @param {Array.<string>} selectors List of the selectors on the page.
 */
pagespeed.criticalCssBeaconInit = function(beaconUrl, htmlUrl, optionsHash,
                                           nonce, selectors) {
  // Verify that the browser supports the APIs we need and bail out early if we
  // don't.
  if (!document.querySelector || !document.querySelectorAll ||
      !Function.prototype.bind) {
    return;
  }

  var temp = new pagespeed.CriticalCssBeacon(beaconUrl, htmlUrl, optionsHash,
                                             nonce, selectors);
  // Add event to the onload handler to scan selectors and beacon back which
  // apply to critical elements.
  var beacon_onload = function() {
    // Attempt not to block other onload events on the page by wrapping in
    // setTimeout().
    window.setTimeout(function() {
      temp.checkCssSelectors_(function() {
        temp.sendBeacon_();
      });
    }, 0);
  };
  pagespeedutils.addHandler(window, 'load', beacon_onload);
};

pagespeed['criticalCssBeaconInit'] = pagespeed.criticalCssBeaconInit;
