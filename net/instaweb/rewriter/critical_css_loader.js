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
 * @fileoverview Turns deferred stylesheets on. The CriticalSelectorFilter
 * replaces a stylesheet link by its critical rules plus a preload link that
 * carries the data-pagespeed-deferred-css attribute, in the place where the
 * stylesheet link stood. This script makes each of those preloads the
 * stylesheet again, in that same place, as soon as the file has arrived.
 */

goog.provide('pagespeed.CriticalCssLoader');

goog.require('pagespeedutils');


/**
 * The attribute that marks a preload link as a deferred stylesheet.
 * @private @const {string}
 */
pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_ = 'data-pagespeed-deferred-css';


/** @private {boolean} */
pagespeed.CriticalCssLoader.started_ = false;


/**
 * Makes one deferred stylesheet a stylesheet again. The element keeps its
 * position, so the order in which the page's rules apply is the page's own.
 * @param {!Element} link A link carrying the deferred attribute.
 * @private
 */
pagespeed.CriticalCssLoader.apply_ = function(link) {
  if (link.getAttribute('rel') == 'preload') {
    link.setAttribute('rel', 'stylesheet');
  }
};


/**
 * Handles the load or the failure of any element in the document; acts on
 * deferred stylesheets only.
 * @param {!Event} event
 * @private
 */
pagespeed.CriticalCssLoader.onLinkEvent_ = function(event) {
  var target = /** @type {Element} */ (event.target);
  if (target && target.nodeName == 'LINK' &&
      target.hasAttribute(pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_)) {
    pagespeed.CriticalCssLoader.apply_(target);
  }
};


/**
 * Makes every deferred stylesheet still waiting a stylesheet again. Covers
 * the ones the browser did not fetch early, for example because their media
 * does not match.
 */
pagespeed.CriticalCssLoader.applyAll = function() {
  var links = document.querySelectorAll(
      'link[' + pagespeed.CriticalCssLoader.DEFERRED_ATTRIBUTE_ + ']');
  for (var i = 0; i < links.length; ++i) {
    pagespeed.CriticalCssLoader.apply_(links[i]);
  }
};


/**
 * Starts watching for deferred stylesheets. The load and error events of a
 * link do not bubble, but they do pass the document in the capture phase, so
 * one listener serves every deferred stylesheet, including those the parser
 * has not reached yet.
 *
 * A deferred stylesheet is never left as a preload:
 *  - when the file has arrived, it becomes the stylesheet at once;
 *  - when the request failed, it becomes an ordinary stylesheet link at
 *    once, which is what the page would have had without this filter;
 *  - in a browser that does not preload, where neither event will come, it
 *    becomes the stylesheet when the document has been parsed;
 *  - whatever is still waiting when the page has loaded (for example a
 *    stylesheet whose media does not match) becomes the stylesheet then;
 *  - whatever is already in the document when this script starts becomes
 *    the stylesheet at once.
 * @export
 */
pagespeed.CriticalCssLoader.Run = function() {
  if (pagespeed.CriticalCssLoader.started_) {
    return;
  }
  pagespeed.CriticalCssLoader.started_ = true;
  document.addEventListener(
      'load', pagespeed.CriticalCssLoader.onLinkEvent_, true);
  document.addEventListener(
      'error', pagespeed.CriticalCssLoader.onLinkEvent_, true);
  var probe = document.createElement('link');
  var supportsPreload = !!(probe.relList && probe.relList.supports &&
                           probe.relList.supports('preload'));
  if (!supportsPreload) {
    pagespeedutils.addHandler(
        document, 'DOMContentLoaded', pagespeed.CriticalCssLoader.applyAll);
  }
  pagespeedutils.addHandler(
      window, 'load', pagespeed.CriticalCssLoader.applyAll);
  // Normally this script runs before the parser has reached the first
  // deferred stylesheet and there is nothing to do yet. If something delayed
  // it, the events above may already have passed: turn on whatever is
  // already in the document, whether its file has arrived, failed, or is
  // still on its way.
  pagespeed.CriticalCssLoader.applyAll();
};
