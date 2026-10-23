// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * @fileoverview Small helpers for the rewriter client scripts that replace
 * the few Closure Library functions they used. An ES module: import what you
 * need, as in
 *   import {toNumber} from '../js/client_compat.js';
 * Only the members a script uses end up in its bundle.
 */


/**
 * Converts a string to a number the way goog.string.toNumber did: the same as
 * Number(str), except that an empty or whitespace-only string is NaN instead
 * of 0. Anything else that Number() reads as a number is a number here too
 * (exponent form, hexadecimal, Infinity), and anything it cannot read is NaN.
 * @param {string} str The string to convert.
 * @return {number} The number the string represents, or NaN.
 */
export const toNumber = function(str) {
  var num = Number(str);
  if (num == 0 && /^[\s\xa0]*$/.test(str)) {
    return NaN;
  }
  return num;
};


/**
 * Returns the number of device pixels per CSS pixel as the browser reports
 * it, or 1 when the browser does not report one. goog.dom.getPixelRatio also
 * probed matchMedia in browsers that have no window.devicePixelRatio; those
 * are older than the ES2015 floor of the shipped scripts.
 * @return {number}
 */
export const getPixelRatio = function() {
  return window.devicePixelRatio !== undefined ? window.devicePixelRatio : 1;
};
