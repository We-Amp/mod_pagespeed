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
 * @fileoverview Code for caching inlined resources in local storage on first
 * view and fetching them therefrom on repeat view.
 *
 * @author matterbury@google.com (Matt Atterbury)
 */

goog.require('pagespeedutils');

// Exporting functions using quoted attributes to prevent js compiler from
// renaming them.
// See https://developers.google.com/closure/compiler/docs/api-tutorial3#dangers
window['pagespeed'] = window['pagespeed'] || {};
var pagespeed = window['pagespeed'];



/**
 * @constructor
 */
pagespeed.LocalStorageCache = function() {
  /**
   * Flag indicating that the cookie needs to be regenerated.
   * @type {boolean}
   * @private
   */
  this.regenerate_cookie_ = true;
};


/**
 * @return {boolean} True if the given object has expired.
 * @param {*} obj is an object of the form '<expiry> <hash> <inline-data>'.
 */
pagespeed.LocalStorageCache.prototype.hasExpired = function(obj) {
  var expiry = parseInt(obj.substring(0, obj.indexOf(' ')), 10);
  return (!isNaN(expiry) && expiry <= pagespeedutils.now());
};

pagespeed.LocalStorageCache.prototype['hasExpired'] =
    pagespeed.LocalStorageCache.prototype.hasExpired;


/**
 * @return {string} The data part of the given object.
 * @param {*} obj is an object of the form '<expiry> <hash> <inline-data>'.
 */
pagespeed.LocalStorageCache.prototype.getData = function(obj) {
  var pos1 = obj.indexOf(' ');
  var pos2 = obj.indexOf(' ', pos1 + 1);
  return obj.substring(pos2 + 1);
};

pagespeed.LocalStorageCache.prototype['getData'] =
    pagespeed.LocalStorageCache.prototype.getData;


/**
 * Replaces the last script element in the DOM with the given element. The
 * script element was added by our C++ code, replacing the element that was
 * there, either an img or a style element. The script itself kicked off the
 * call to this method to replace itself with the original img/style element
 * using the inlined data from local storage.
 * @param {Element} newElement The element to replace with.
 */
pagespeed.LocalStorageCache.prototype.replaceLastScript = function(newElement) {
  var scripts = document.getElementsByTagName('script');
  var lastElement = scripts[scripts.length - 1];
  lastElement.parentNode.replaceChild(newElement, lastElement);
};

pagespeed.LocalStorageCache.prototype['replaceLastScript'] =
    pagespeed.LocalStorageCache.prototype.replaceLastScript;


/**
 * Inline the CSS with the given URL.
 * @param {string} url is the URL of the CSS to inline.
 */
pagespeed.LocalStorageCache.prototype.inlineCss = function(url) {
  // Storage access can throw (e.g. SecurityError when storage is disabled
  // by browser policy); fall back to the original resource so the element
  // this script replaced is never lost.
  var obj = null;
  try {
    obj = window.localStorage.getItem('pagespeed_lsc_url:' + url);
  } catch (e) {
    // Handled below: a null obj restores the resource from the network.
  }
  var newNode = document.createElement(obj ? 'style' : 'link');
  if (obj && !this.hasExpired(obj)) {
    newNode.type = 'text/css';
    newNode.appendChild(document.createTextNode(this.getData(obj)));
  } else {
    newNode.rel = 'stylesheet';
    newNode.href = url;
    this.regenerate_cookie_ = true;
  }
  this.replaceLastScript(newNode);
};

pagespeed.LocalStorageCache.prototype['inlineCss'] =
    pagespeed.LocalStorageCache.prototype.inlineCss;


/**
 * Inline the IMG with the given URL.
 * @param {string} url is the URL of the image to inline.
 * @param {string} hash is the hash of the image to inline.
 */
pagespeed.LocalStorageCache.prototype.inlineImg = function(url, hash) {
  // Storage access can throw (e.g. SecurityError when storage is disabled
  // by browser policy); fall back to the original resource so the element
  // this script replaced is never lost.
  var obj = null;
  try {
    obj = window.localStorage.getItem('pagespeed_lsc_url:' + url + ' ' +
                                      'pagespeed_lsc_hash:' + hash);
  } catch (e) {
    // Handled below: a null obj restores the resource from the network.
  }
  var newNode = document.createElement('img');
  if (obj && !this.hasExpired(obj)) {
    newNode.src = this.getData(obj);
  } else {
    newNode.src = url;
    this.regenerate_cookie_ = true;
  }
  // Copy over any other original attributes.
  for (var i = 2, n = arguments.length; i < n; ++i) {
    var pos = arguments[i].indexOf('=');
    newNode.setAttribute(arguments[i].substring(0, pos),
                         arguments[i].substring(pos + 1));
  }
  this.replaceLastScript(newNode);
};

pagespeed.LocalStorageCache.prototype['inlineImg'] =
    pagespeed.LocalStorageCache.prototype.inlineImg;


/**
 * For each element of the given tag name, check if it has
 * data-pagespeed-lsc-url and data-pagespeed-lsc-hash attributes and, if so,
 * save the element's hash, expiry, and data in local
 * storage. regenerate_cookie_ is set to true if any elements are saved, which
 * later triggers regeneration of the cookie.
 * @param {string} tagName Tag Name of elements to process.
 * @param {boolean} isHashInKey True iff the hash is part of the lookup key.
 * @param {(function ({Element})|function ({Image}))} dataFunc Function to get
 *     an element's data.
 * @private
 */
pagespeed.LocalStorageCache.prototype.processTags_ = function(tagName,
                                                              isHashInKey,
                                                              dataFunc) {
  var elements = document.getElementsByTagName(tagName);
  for (var i = 0, n = elements.length; i < n; ++i) {
    var element = elements[i];
    var hash = element.getAttribute('data-pagespeed-lsc-hash');
    var url = element.getAttribute('data-pagespeed-lsc-url');
    if (hash && url) {
      var urlkey = 'pagespeed_lsc_url:' + url;
      if (isHashInKey) {
        urlkey += ' pagespeed_lsc_hash:' + hash;
      }
      var expiry = element.getAttribute('data-pagespeed-lsc-expiry');
      var millis = (expiry ? (new Date(expiry)).getTime() : '');
      var data = dataFunc(element);
      if (!data) {
        // img.src is set to a data URI on the repeat view but is missing
        // thereafter, and we must not forget it once we have it.
        var obj = null;
        try {
          obj = window.localStorage.getItem(urlkey);
        } catch (e) {
          // Storage unavailable; treat as a cache miss.
        }
        if (obj) {
          data = this.getData(obj);
        }
      }
      if (data) {
        try {
          window.localStorage.setItem(urlkey,
                                      millis + ' ' + hash + ' ' + data);
          this.regenerate_cookie_ = true;
        } catch (e) {
          // Quota exceeded or storage disabled; skip this element.
        }
      }
    }
  }
};


/**
 * Save any inlined data to local storage.
 * @private
 */
pagespeed.LocalStorageCache.prototype.saveInlinedData_ = function() {
  this.processTags_('img', true /* isHashInKey */,
                    function(e) { return e.src; });
  this.processTags_('style', false /* isHashInKey */,
                    function(e) {
                      return (e.firstChild ? e.firstChild.nodeValue : null);
                    });
};


/**
 * Regenerate the cookie from the hash's of unexpired objects in local storage
 * and remove all expired objects.
 * @private
 */
pagespeed.LocalStorageCache.prototype.generateCookie_ = function() {
  if (this.regenerate_cookie_) {
    var deadUns = [];
    var goodUns = [];
    var minExpiry = 0;
    var currentTime = pagespeedutils.now();
    // Process every local storage object of ours.
    var numItems = 0;
    try {
      numItems = window.localStorage.length;
    } catch (e) {
      // Storage unavailable; regenerate nothing.
    }
    for (var i = 0, n = numItems; i < n; ++i) {
      try {
        var key = window.localStorage.key(i);
        if (key.indexOf('pagespeed_lsc_url:')) continue;  // Not one of ours.
        var obj = window.localStorage.getItem(key);
        var pos1 = obj.indexOf(' ');
        var expiry = parseInt(obj.substring(0, pos1), 10);
        if (!isNaN(expiry)) {
          if (expiry <= currentTime) {
            deadUns.push(key);
            continue;
          } else if (expiry < minExpiry || minExpiry == 0) {
            minExpiry = expiry;
          }
        }
        var pos2 = obj.indexOf(' ', pos1 + 1);
        var hash = obj.substring(pos1 + 1, pos2);
        goodUns.push(hash);
      } catch (e) {
        continue;  // Skip entries we cannot read.
      }
    }
    // Set the cookie.
    var expires = '';
    if (minExpiry) expires = '; expires=' + (new Date(minExpiry)).toUTCString();
    document.cookie = '_GPSLSC=' + goodUns.join('!') + ';path=/' + expires;
    // Remove all expired objects.
    for (var i = 0, n = deadUns.length; i < n; ++i) {
      try {
        window.localStorage.removeItem(deadUns[i]);
      } catch (e) {
        // Storage unavailable; leave the expired entry.
      }
    }
    this.regenerate_cookie_ = false;
  }
};


/**
 * Initializes the local storage cache module.
 */
pagespeed.localStorageCacheInit = function() {
  // Always create the cache object, even when storage is missing or throws:
  // merely reading window.localStorage can throw (e.g. SecurityError when
  // storage is disabled by browser policy), so storage access is guarded at
  // the point of use rather than gated here.
  var temp = new pagespeed.LocalStorageCache();
  pagespeed['localStorageCache'] = temp;
  pagespeedutils.addHandler(window, 'load',
      function() {
        temp.saveInlinedData_();
      });
  pagespeedutils.addHandler(window, 'load',
      function() {
        temp.generateCookie_();
      });
};

pagespeed['localStorageCacheInit'] = pagespeed.localStorageCacheInit;
